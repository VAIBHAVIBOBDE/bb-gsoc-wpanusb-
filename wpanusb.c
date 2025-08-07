// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the WPANUSB IEEE 802.15.4 dongle
 *
 * Copyright (C) 2018 Intel Corp.
 *
 * The driver implements SoftMAC 802.15.4 protocol based on atusb
 * driver for ATUSB IEEE 802.15.4 dongle.
 *
 * Written by Andrei Emeltchenko <andrei.emeltchenko@intel.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/skbuff.h>
#include <linux/usb.h>

#include <net/cfg802154.h>
#include <net/mac802154.h>

#include "wpanusb.h"

#define WPANUSB_NUM_RX_URBS	4	/* allow for a bit of local latency */
#define WPANUSB_ALLOC_DELAY_MS	100	/* delay after failed allocation */

#define VENDOR_OUT		(USB_TYPE_VENDOR | USB_DIR_OUT)
#define VENDOR_IN		(USB_TYPE_VENDOR | USB_DIR_IN)

#define WPANUSB_VALID_CHANNELS	(0x07FFFFFF)

#define DEFAULT_LBT_DURATION_US	1000	/* Default LBT duration in microseconds */

struct wpanusb {
	struct ieee802154_hw *hw;
	struct usb_device *udev;
	int shutdown;			/* non-zero if shutting down */

	/* RX variables */
	struct delayed_work work;	/* memory allocations */
	struct usb_anchor idle_urbs;	/* URBs waiting to be submitted */
	struct usb_anchor rx_urbs;	/* URBs waiting for reception */

	/* TX variables */
	struct usb_ctrlrequest tx_dr;
	struct urb *tx_urb;
	struct sk_buff *tx_skb;
	u8 tx_ack_seq;			/* current TX ACK sequence number */
};

/* ----- USB commands without data ----------------------------------------- */

static int wpanusb_control_send(struct wpanusb *wpanusb, unsigned int pipe,
				u8 request, void *data, u16 size)
{
	struct usb_device *udev = wpanusb->udev;

	return usb_control_msg(udev, pipe, request, VENDOR_OUT,
			       0, 0, data, size, 1000);
}

static int wpanusb_control_recv(struct wpanusb *wpanusb, u8 request, void *data, u16 size)
{
	struct usb_device *udev = wpanusb->udev;
	int ret;

	/* First send the request */
	ret = usb_control_msg(udev, usb_sndctrlpipe(udev, 0), request, VENDOR_OUT,
			      0, 0, NULL, 0, 1000);
	if (ret < 0) {
		dev_err(&udev->dev, "Failed to send control request %u, ret %d", request, ret);
		return ret;
	}

	/* Then receive the response */
	ret = usb_control_msg(udev, usb_rcvctrlpipe(udev, 0), request, VENDOR_IN,
			      0, 0, data, size, 1000);
	if (ret < 0) {
		dev_err(&udev->dev, "Failed to receive control response %u, ret %d", request, ret);
		return ret;
	}

	return ret;
}

/* ----- skb allocation ---------------------------------------------------- */

#define MAX_PSDU	127
#define MAX_RX_XFER	(1 + MAX_PSDU + 2 + 1)	/* PHR+PSDU+CRC+LQI */

#define SKB_WPANUSB(skb)	(*(struct wpanusb **)(skb)->cb)

static void wpanusb_bulk_complete(struct urb *urb);

static int wpanusb_submit_rx_urb(struct wpanusb *wpanusb, struct urb *urb)
{
	struct usb_device *udev = wpanusb->udev;
	struct sk_buff *skb = urb->context;
	int ret;

	if (!skb) {
		skb = alloc_skb(MAX_RX_XFER, GFP_KERNEL);
		if (!skb) {
			dev_warn_ratelimited(&udev->dev,
					     "can't allocate skb\n");
			return -ENOMEM;
		}
		skb_put(skb, MAX_RX_XFER);
		SKB_WPANUSB(skb) = wpanusb;
	}

	usb_fill_bulk_urb(urb, udev, usb_rcvbulkpipe(udev, 1),
			  skb->data, MAX_RX_XFER, wpanusb_bulk_complete, skb);
	usb_anchor_urb(urb, &wpanusb->rx_urbs);

	ret = usb_submit_urb(urb, GFP_KERNEL);
	if (ret) {
		usb_unanchor_urb(urb);
		kfree_skb(skb);
		urb->context = NULL;
	}

	return ret;
}

static void wpanusb_work_urbs(struct work_struct *work)
{
	struct wpanusb *wpanusb =
		container_of(to_delayed_work(work), struct wpanusb, work);
	struct usb_device *udev = wpanusb->udev;
	struct urb *urb;
	int ret;

	if (wpanusb->shutdown)
		return;

	do {
		urb = usb_get_from_anchor(&wpanusb->idle_urbs);
		if (!urb)
			return;

		ret = wpanusb_submit_rx_urb(wpanusb, urb);
	} while (!ret);

	usb_anchor_urb(urb, &wpanusb->idle_urbs);
	dev_warn_ratelimited(&udev->dev, "can't allocate/submit URB (%d)\n",
			     ret);
	schedule_delayed_work(&wpanusb->work,
			      msecs_to_jiffies(WPANUSB_ALLOC_DELAY_MS) + 1);
}

/* ----- Asynchronous USB -------------------------------------------------- */

static void wpanusb_tx_done(struct wpanusb *wpanusb, uint8_t seq)
{
	struct usb_device *udev = wpanusb->udev;
	u8 expect = wpanusb->tx_ack_seq;

	dev_dbg(&udev->dev, "seq 0x%02x expect 0x%02x\n", seq, expect);

	if (seq == expect) {
		ieee802154_xmit_complete(wpanusb->hw, wpanusb->tx_skb, false);
	} else {
		dev_dbg(&udev->dev, "unknown ack %u\n", seq);

		if (wpanusb->tx_skb)
			dev_kfree_skb_irq(wpanusb->tx_skb);
	}
}

static void wpanusb_process_urb(struct urb *urb)
{
	struct usb_device *udev = urb->dev;
	struct sk_buff *skb = urb->context;
	struct wpanusb *wpanusb = SKB_WPANUSB(skb);
	u8 len, lqi;

	if (!urb->actual_length) {
		dev_dbg(&udev->dev, "zero-sized URB ?\n");
		return;
	}

	len = *skb->data;

	dev_dbg(&udev->dev, "urb %p urb len %u pkt len %u", urb,
		urb->actual_length, len);

	/* Handle ACK */
	if (urb->actual_length == 1) {
		wpanusb_tx_done(wpanusb, len);
		return;
	}

	if (len + 1 > urb->actual_length - 1) {
		dev_dbg(&udev->dev, "frame len %d+1 > URB %u-1\n",
			len, urb->actual_length);
		return;
	}

	if (!ieee802154_is_valid_psdu_len(len)) {
		dev_dbg(&udev->dev, "frame corrupted\n");
		return;
	}

	print_hex_dump_bytes("> ", DUMP_PREFIX_OFFSET, skb->data,
			     urb->actual_length);

	/* Get LQI at the end of the packet */
	lqi = skb->data[len + 1];
	dev_dbg(&udev->dev, "rx len %d lqi 0x%02x\n", len, lqi);
	skb_pull(skb, 1);	/* remove length */
	skb_trim(skb, len);	/* remove LQI */
	ieee802154_rx_irqsafe(wpanusb->hw, skb, lqi);
	urb->context = NULL;	/* skb is gone */
}

static void wpanusb_bulk_complete(struct urb *urb)
{
	struct usb_device *udev = urb->dev;
	struct sk_buff *skb = urb->context;
	struct wpanusb *wpanusb = SKB_WPANUSB(skb);

	dev_dbg(&udev->dev, "status %d len %d\n",
		urb->status, urb->actual_length);

	if (urb->status) {
		if (urb->status == -ENOENT) { /* being killed */
			kfree_skb(skb);
			urb->context = NULL;
			return;
		}

		dev_dbg(&udev->dev, "URB error %d\n", urb->status);
	} else {
		wpanusb_process_urb(urb);
	}

	usb_anchor_urb(urb, &wpanusb->idle_urbs);
	if (!wpanusb->shutdown)
		schedule_delayed_work(&wpanusb->work, 0);
}

/* ----- URB allocation/deallocation --------------------------------------- */

static void wpanusb_free_urbs(struct wpanusb *wpanusb)
{
	struct urb *urb;

	do {
		urb = usb_get_from_anchor(&wpanusb->idle_urbs);
		if (!urb)
			break;
		kfree_skb(urb->context);
		usb_free_urb(urb);
	} while (true);
}

static int wpanusb_alloc_urbs(struct wpanusb *wpanusb, unsigned int n)
{
	struct urb *urb;

	while (n--) {
		urb = usb_alloc_urb(0, GFP_KERNEL);
		if (!urb) {
			wpanusb_free_urbs(wpanusb);
			return -ENOMEM;
		}
		usb_anchor_urb(urb, &wpanusb->idle_urbs);
	}

	return 0;
}

/* ----- IEEE 802.15.4 interface operations -------------------------------- */

static void wpanusb_xmit_complete(struct urb *urb)
{
	dev_dbg(&urb->dev->dev, "urb transmit completed");
}

static int wpanusb_xmit(struct ieee802154_hw *hw, struct sk_buff *skb)
{
	struct wpanusb *wpanusb = hw->priv;
	struct usb_device *udev = wpanusb->udev;
	int ret = 0;

	dev_dbg(&udev->dev, "len %u", skb->len);

	/* ack_seq range is 0x01 - 0xff */
	wpanusb->tx_ack_seq++;
	if (!wpanusb->tx_ack_seq)
		wpanusb->tx_ack_seq++;

	wpanusb->tx_skb = skb;
	wpanusb->tx_dr.wIndex = cpu_to_le16(wpanusb->tx_ack_seq);
	wpanusb->tx_dr.wLength = cpu_to_le16(skb->len);

	usb_fill_control_urb(wpanusb->tx_urb, udev,
			     usb_sndctrlpipe(udev, 0),
			     (unsigned char *)&wpanusb->tx_dr, skb->data,
			     skb->len, wpanusb_xmit_complete, NULL);
	ret = usb_submit_urb(wpanusb->tx_urb, GFP_ATOMIC);

	dev_dbg(&udev->dev, "%s: ret %d len %u seq %u\n", __func__, ret,
		skb->len, wpanusb->tx_ack_seq);

	return ret;
}

static int wpanusb_channel(struct ieee802154_hw *hw, u8 page, u8 channel)
{
	struct wpanusb *wpanusb = hw->priv;
	struct usb_device *udev = wpanusb->udev;
	struct set_channel *req;
	int ret;

	/* Validate page and channel */
	if (page >= IEEE802154_MAX_PAGE) {
		dev_err(&udev->dev, "Invalid page %u", page);
		return -EINVAL;
	}

	if (!(hw->phy->supported.channels[page] & BIT(channel))) {
		dev_err(&udev->dev, "Channel %u not supported on page %u", channel, page);
		return -EINVAL;
	}

	req = kmalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	req->page = page;      /* Now includes page information */
	req->channel = channel;

	ret = wpanusb_control_send(wpanusb, usb_sndctrlpipe(udev, 0),
				   SET_CHANNEL, req, sizeof(*req));
	kfree(req);
	if (ret < 0) {
		dev_err(&udev->dev, "Failed set channel %u on page %u, ret %d", 
			channel, page, ret);
		return ret;
	}

	dev_dbg(&udev->dev, "set page %u channel %u", page, channel);

	return 0;
}

static int wpanusb_ed(struct ieee802154_hw *hw, u8 *level)
{
	WARN_ON(!level);

	*level = 0xbe;

	return 0;
}

static int wpanusb_set_hw_addr_filt(struct ieee802154_hw *hw,
				    struct ieee802154_hw_addr_filt *filt,
				    unsigned long changed)
{
	struct wpanusb *wpanusb = hw->priv;
	struct usb_device *udev = wpanusb->udev;
	int ret = 0;

	if (changed & IEEE802154_AFILT_SADDR_CHANGED) {
		struct set_short_addr *req;

		req = kmalloc(sizeof(*req), GFP_KERNEL);
		if (!req)
			return -ENOMEM;

		req->short_addr = filt->short_addr;

		ret = wpanusb_control_send(wpanusb, usb_sndctrlpipe(udev, 0),
					   SET_SHORT_ADDR, req, sizeof(*req));
		kfree(req);
		if (ret < 0) {
			dev_err(&udev->dev, "Failed to set short_addr, ret %d",
				ret);
			return ret;
		}

		dev_dbg(&udev->dev, "short addr changed to 0x%04x",
			le16_to_cpu(filt->short_addr));
	}

	if (changed & IEEE802154_AFILT_PANID_CHANGED) {
		struct set_pan_id *req;

		req = kmalloc(sizeof(*req), GFP_KERNEL);
		if (!req)
			return -ENOMEM;

		req->pan_id = filt->pan_id;

		ret = wpanusb_control_send(wpanusb, usb_sndctrlpipe(udev, 0),
					   SET_PAN_ID, req, sizeof(*req));
		kfree(req);
		if (ret < 0) {
			dev_err(&udev->dev, "Failed to set pan_id, ret %d",
				ret);
			return ret;
		}

		dev_dbg(&udev->dev, "pan id changed to 0x%04x",
			le16_to_cpu(filt->pan_id));
	}

	if (changed & IEEE802154_AFILT_IEEEADDR_CHANGED) {
		struct set_ieee_addr *req;

		req = kmalloc(sizeof(*req), GFP_KERNEL);
		if (!req)
			return -ENOMEM;

		memcpy(&req->ieee_addr, &filt->ieee_addr,
		       sizeof(req->ieee_addr));

		ret = wpanusb_control_send(wpanusb, usb_sndctrlpipe(udev, 0),
					   SET_IEEE_ADDR, req, sizeof(*req));
		kfree(req);
		if (ret < 0) {
			dev_err(&udev->dev, "Failed to set ieee_addr, ret %d",
				ret);
			return ret;
		}

		dev_dbg(&udev->dev, "IEEE addr changed");
	}

	if (changed & IEEE802154_AFILT_PANC_CHANGED) {
		dev_dbg(&udev->dev, "panc changed");

		dev_err(&udev->dev, "Not handled AFILT_PANC_CHANGED");
	}

	return ret;
}

static int wpanusb_set_extended_addr(struct ieee802154_hw *hw)
{
	struct wpanusb *wpanusb = hw->priv;
	struct usb_device *udev = wpanusb->udev;
	unsigned char *buffer;
	__le64 extended_addr;
	int ret = 0;
	u64 addr;

	buffer = kmalloc(IEEE802154_EXTENDED_ADDR_LEN, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	ret = wpanusb_control_send(wpanusb, usb_sndctrlpipe(udev, 0), GET_EXTENDED_ADDR, buffer,
					IEEE802154_EXTENDED_ADDR_LEN);
	if (ret < 0) {
		dev_err(&udev->dev, "failed to fetch extended address, random address set\n");
		ieee802154_random_extended_addr(&wpanusb->hw->phy->perm_extended_addr);
		kfree(buffer);
		return ret;
	}

	memcpy(&extended_addr, buffer, IEEE802154_EXTENDED_ADDR_LEN);
	/* Check if read address is not empty and the unicast bit is set correctly */
	if (!ieee802154_is_valid_extended_unicast_addr(extended_addr)) {
		dev_info(&udev->dev, "no permanent extended address found, random address set\n");
		ieee802154_random_extended_addr(&wpanusb->hw->phy->perm_extended_addr);
	} else {
		wpanusb->hw->phy->perm_extended_addr = extended_addr;
		addr = swab64((__force u64)wpanusb->hw->phy->perm_extended_addr);
		dev_info(&udev->dev, "Read permanent extended address %8phC from device\n", &addr);
	}

	kfree(buffer);
	return ret;
}

/* FIXME: these need to come as capabilities from the device */
static const s32 wpanusb_powers[] = {
	300, 280, 230, 180, 130, 70, 0, -100, -200, -300, -400, -500, -700,
	-900, -1200, -1700,
};

/* Dynamic capability discovery functions */

static int wpanusb_get_device_info(struct wpanusb *wpanusb, struct device_info *info)
{
	struct usb_device *udev = wpanusb->udev;
	int ret;

	ret = wpanusb_control_recv(wpanusb, GET_DEVICE_INFO, info, sizeof(*info));
	if (ret < 0) {
		dev_err(&udev->dev, "Failed to get device info, ret %d", ret);
		return ret;
	}

	dev_info(&udev->dev, "Device version: %u.%u, Protocol: %u.%u",
		 le16_to_cpu(info->device_version) >> 8,
		 le16_to_cpu(info->device_version) & 0xFF,
		 le16_to_cpu(info->protocol_version) >> 8,
		 le16_to_cpu(info->protocol_version) & 0xFF);

	return 0;
}

static int wpanusb_get_hardware_caps(struct wpanusb *wpanusb, struct hardware_caps *caps)
{
	struct usb_device *udev = wpanusb->udev;
	int ret;

	ret = wpanusb_control_recv(wpanusb, GET_HARDWARE_CAPS, caps, sizeof(*caps));
	if (ret < 0) {
		dev_err(&udev->dev, "Failed to get hardware capabilities, ret %d", ret);
		return ret;
	}

	dev_dbg(&udev->dev, "Hardware caps: flags=0x%08x, LBT=%u, CCA=0x%02x",
		le32_to_cpu(caps->hw_flags), caps->has_lbt, caps->has_cca_modes);

	return 0;
}

static int wpanusb_get_phy_caps(struct wpanusb *wpanusb, struct phy_caps *caps)
{
	struct usb_device *udev = wpanusb->udev;
	int ret;

	ret = wpanusb_control_recv(wpanusb, GET_PHY_CAPS, caps, sizeof(*caps));
	if (ret < 0) {
		dev_err(&udev->dev, "Failed to get PHY capabilities, ret %d", ret);
		return ret;
	}

	dev_dbg(&udev->dev, "PHY caps: flags=0x%08x, pages=%u, default_page=%u",
		le32_to_cpu(caps->phy_flags), caps->supported_pages, caps->current_page);

	return 0;
}

static int wpanusb_get_power_levels(struct wpanusb *wpanusb, s32 **power_levels, size_t *count)
{
	struct usb_device *udev = wpanusb->udev;
	struct power_levels *levels;
	s32 *powers;
	int ret, i;
	u8 max_levels = 32; /* Reasonable maximum */

	/* Allocate buffer for maximum possible power levels */
	levels = kmalloc(sizeof(*levels) + max_levels * sizeof(__le32), GFP_KERNEL);
	if (!levels)
		return -ENOMEM;

	ret = wpanusb_control_recv(wpanusb, GET_POWER_LEVELS, levels,
				  sizeof(*levels) + max_levels * sizeof(__le32));
	if (ret < 0) {
		dev_err(&udev->dev, "Failed to get power levels, ret %d", ret);
		kfree(levels);
		return ret;
	}

	if (levels->num_levels == 0 || levels->num_levels > max_levels ||
	    levels->default_level >= levels->num_levels) {
		dev_err(&udev->dev, "Invalid power levels count: %u or default_level: %u",
			levels->num_levels, levels->default_level);
		kfree(levels);
		return -EINVAL;
	}

	/* Convert to host byte order and allocate permanent storage */
	powers = kmalloc_array(levels->num_levels, sizeof(s32), GFP_KERNEL);
	if (!powers) {
		kfree(levels);
		return -ENOMEM;
	}

	for (i = 0; i < levels->num_levels; i++) {
		powers[i] = le32_to_cpu(levels->levels[i]);
	}

	*power_levels = powers;
	*count = levels->num_levels;

	dev_info(&udev->dev, "Found %zu power levels, default: %d mbm",
		 *count, powers[levels->default_level]);

	kfree(levels);
	return 0;
}

static int wpanusb_get_channel_pages(struct wpanusb *wpanusb, struct ieee802154_hw *hw)
{
	struct usb_device *udev = wpanusb->udev;
	struct channel_pages *pages;
	int ret, i;
	u8 max_pages = 8; /* IEEE 802.15.4 defines pages 0-7 */

	pages = kmalloc(sizeof(*pages) + max_pages * sizeof(struct channel_page), GFP_KERNEL);
	if (!pages)
		return -ENOMEM;

	ret = wpanusb_control_recv(wpanusb, GET_CHANNEL_PAGES, pages,
				  sizeof(*pages) + max_pages * sizeof(struct channel_page));
	if (ret < 0) {
		dev_err(&udev->dev, "Failed to get channel pages, ret %d", ret);
		kfree(pages);
		return ret;
	}

	if (pages->num_pages == 0 || pages->num_pages > max_pages) {
		dev_err(&udev->dev, "Invalid page count: %u", pages->num_pages);
		kfree(pages);
		return -EINVAL;
	}

	/* Configure supported channels for each page */
	for (i = 0; i < pages->num_pages; i++) {
		u8 page = pages->pages[i].page;
		u32 channels = le32_to_cpu(pages->pages[i].channels_mask);
		
		if (page < IEEE802154_MAX_PAGE) {
			hw->phy->supported.channels[page] = channels;
			dev_info(&udev->dev, "Page %u: channels 0x%08x", page, channels);
		}
	}

	/* Set current page and channel */
	hw->phy->current_page = pages->default_page;
	if (hw->phy->supported.channels[pages->default_page]) {
		hw->phy->current_channel = ffs(hw->phy->supported.channels[pages->default_page]) - 1;
	}

	kfree(pages);
	return 0;
}

static bool wpanusb_validate_capabilities(struct ieee802154_hw *hw,
					 struct hardware_caps *hw_caps,
					 struct phy_caps *phy_caps)
{
	struct usb_device *udev = ((struct wpanusb *)hw->priv)->udev;

	/* Validate hardware capabilities */
	if (hw_caps->max_frame_retries > 7) {
		dev_warn(&udev->dev, "Invalid max frame retries: %u", 
			 hw_caps->max_frame_retries);
		return false;
	}

	/* Validate PHY capabilities */
	if (phy_caps->supported_pages == 0 || phy_caps->supported_pages > 8) {
		dev_warn(&udev->dev, "Invalid page count: %u", phy_caps->supported_pages);
		return false;
	}

	if (phy_caps->current_page >= phy_caps->supported_pages) {
		dev_warn(&udev->dev, "Invalid default page: %u", phy_caps->current_page);
		return false;
	}

	return true;
}

static int wpanusb_get_device_capabilities(struct ieee802154_hw *hw)
{
	struct wpanusb *wpanusb = hw->priv;
	struct usb_device *udev = wpanusb->udev;
	struct device_info dev_info;
	struct hardware_caps hw_caps;
	struct phy_caps phy_caps;
	s32 *power_levels = NULL;
	size_t power_count = 0;
	int ret = 0;
	unsigned char *buffer = NULL;
	uint32_t valid_channels;

	/* Step 1: Get basic device information */
	ret = wpanusb_get_device_info(wpanusb, &dev_info);
	if (ret < 0) {
		dev_warn(&udev->dev, "Device info query failed, using defaults");
		goto fallback;
	}

	/* Step 2: Get hardware capabilities */
	ret = wpanusb_get_hardware_caps(wpanusb, &hw_caps);
	if (ret < 0) {
		dev_warn(&udev->dev, "Hardware caps query failed, using defaults");
		goto fallback;
	}

	/* Step 3: Get PHY capabilities */
	ret = wpanusb_get_phy_caps(wpanusb, &phy_caps);
	if (ret < 0) {
		dev_warn(&udev->dev, "PHY caps query failed, using defaults");
		goto fallback;
	}

	/* Step 4: Validate capabilities */
	if (!wpanusb_validate_capabilities(hw, &hw_caps, &phy_caps)) {
		dev_warn(&udev->dev, "Invalid capabilities, using defaults");
		goto fallback;
	}

	/* Step 5: Get supported power levels */
	ret = wpanusb_get_power_levels(wpanusb, &power_levels, &power_count);
	if (ret < 0) {
		dev_warn(&udev->dev, "Power levels query failed, using defaults");
		goto fallback;
	}

	/* Step 6: Get channel page information */
	ret = wpanusb_get_channel_pages(wpanusb, hw);
	if (ret < 0) {
		dev_warn(&udev->dev, "Channel pages query failed, using defaults");
		goto fallback;
	}

	/* Configure hardware flags from device capabilities */
	hw->flags = IEEE802154_HW_TX_OMIT_CKSUM | IEEE802154_HW_AFILT;
	
	/* Add dynamic flags based on device capabilities */
	if (hw_caps.has_lbt)
		hw->flags |= IEEE802154_HW_LBT;
	if (hw_caps.has_cca_modes)
		hw->flags |= IEEE802154_HW_CSMA_PARAMS;
	if (hw_caps.max_frame_retries > 0)
		hw->flags |= IEEE802154_HW_FRAME_RETRIES;
	if (hw_caps.has_promiscuous)
		hw->flags |= IEEE802154_HW_PROMISCUOUS;

	/* Configure PHY flags from device capabilities */
	hw->phy->flags = le32_to_cpu(phy_caps.phy_flags);

	/* Configure power levels from device */
	hw->phy->supported.tx_powers = power_levels;
	hw->phy->supported.tx_powers_size = power_count;
	hw->phy->transmit_power = power_levels[0]; /* Use first (highest) power */

	dev_info(&udev->dev, "Dynamic capabilities loaded successfully");
	dev_info(&udev->dev, "HW flags: 0x%08x, PHY flags: 0x%08x", 
		 hw->flags, hw->phy->flags);

	return 0;

fallback:
	dev_info(&udev->dev, "Using fallback hardcoded capabilities");
	
	/* Fallback to hardcoded values */
	hw->flags = IEEE802154_HW_TX_OMIT_CKSUM | IEEE802154_HW_AFILT;
	hw->phy->flags = WPAN_PHY_FLAG_TXPOWER;
	
	/* Use hardcoded power levels */
	hw->phy->supported.tx_powers = wpanusb_powers;
	hw->phy->supported.tx_powers_size = ARRAY_SIZE(wpanusb_powers);
	hw->phy->transmit_power = wpanusb_powers[0];

	/* Set default channels - try to get from device first */
	buffer = kmalloc(sizeof(valid_channels), GFP_KERNEL);
	if (buffer) {
		ret = wpanusb_control_recv(wpanusb, GET_SUPPORTED_CHANNELS, buffer, sizeof(valid_channels));
		valid_channels = *(uint32_t *)buffer;
		if (ret < 0 || !valid_channels) {
			valid_channels = WPANUSB_VALID_CHANNELS;
		}
		kfree(buffer);
	} else {
		valid_channels = WPANUSB_VALID_CHANNELS;
	}

	hw->phy->current_page = 0;
	hw->phy->current_channel = ffs(valid_channels) - 1;
	hw->phy->supported.channels[0] = valid_channels;

	/* Clean up any allocated memory */
	kfree(power_levels);

	return 0; /* Return success even with fallback */
}

static int wpanusb_start(struct ieee802154_hw *hw)
{
	struct wpanusb *wpanusb = hw->priv;
	struct usb_device *udev = wpanusb->udev;
	int ret;

	schedule_delayed_work(&wpanusb->work, 0);

	ret = wpanusb_control_send(wpanusb, usb_sndctrlpipe(udev, 0),
				   START, NULL, 0);
	if (ret < 0) {
		dev_err(&udev->dev, "Failed to start ieee802154");
		usb_kill_anchored_urbs(&wpanusb->idle_urbs);
	}

	return ret;
}

static void wpanusb_stop(struct ieee802154_hw *hw)
{
	struct wpanusb *wpanusb = hw->priv;
	struct usb_device *udev = wpanusb->udev;
	int ret;

	dev_dbg(&udev->dev, "stop");

	usb_kill_anchored_urbs(&wpanusb->idle_urbs);

	ret = wpanusb_control_send(wpanusb, usb_sndctrlpipe(udev, 0),
				   STOP, NULL, 0);
	if (ret < 0)
		dev_err(&udev->dev, "Failed to stop ieee802154");
}

static int wpanusb_set_txpower(struct ieee802154_hw *hw, s32 mbm)
{
	struct wpanusb *wpanusb = hw->priv;
	struct usb_device *udev = wpanusb->udev;
	struct set_txpower req;
	int ret, i;
	bool power_supported = false;

	/* Check if power levels are available */
	if (!hw->phy->supported.tx_powers || hw->phy->supported.tx_powers_size == 0) {
		dev_err(&udev->dev, "No supported power levels available");
		return -EOPNOTSUPP;
	}

	/* Validate power level against supported values */
	for (i = 0; i < hw->phy->supported.tx_powers_size; i++) {
		if (hw->phy->supported.tx_powers[i] == mbm) {
			power_supported = true;
			break;
		}
	}

	if (!power_supported) {
		dev_err(&udev->dev, "Unsupported TX power %d mbm", mbm);
		return -EINVAL;
	}

	req.power_mbm = cpu_to_le32(mbm);

	ret = wpanusb_control_send(wpanusb, usb_sndctrlpipe(udev, 0),
				   SET_TXPOWER, &req, sizeof(req));
	
	if (ret < 0) {
		dev_err(&udev->dev, "Failed to set TX power to %d mbm, ret %d", 
			mbm, ret);
		return ret;
	}

	dev_dbg(&udev->dev, "TX power set to %d mbm", mbm);
	return 0;
}

static int wpanusb_set_cca_mode(struct ieee802154_hw *hw,
				const struct wpan_phy_cca *cca)
{
	struct wpanusb *wpanusb = hw->priv;
	struct usb_device *udev = wpanusb->udev;
	struct set_cca_mode req;
	int ret;

	req.mode = cca->mode;
	req.opt = cca->opt;

	switch (cca->mode) {
	case NL802154_CCA_ENERGY:
		dev_dbg(&udev->dev, "Setting CCA mode to Energy Detection");
		break;
	case NL802154_CCA_CARRIER:
		dev_dbg(&udev->dev, "Setting CCA mode to Carrier Sense");
		break;
	case NL802154_CCA_ENERGY_CARRIER:
		dev_dbg(&udev->dev, "Setting CCA mode to Energy + Carrier");
		break;
	default:
		dev_err(&udev->dev, "Unsupported CCA mode: %d", cca->mode);
		return -EINVAL;
	}

	ret = wpanusb_control_send(wpanusb, usb_sndctrlpipe(udev, 0),
				   SET_CCA_MODE, &req, sizeof(req));
	
	if (ret < 0) {
		dev_err(&udev->dev, "Failed to set CCA mode %u opt %u, ret %d",
			cca->mode, cca->opt, ret);
		return ret;
	}

	dev_dbg(&udev->dev, "CCA mode set to %u with option %u", 
		cca->mode, cca->opt);
	return 0;
}

static int wpanusb_set_lbt(struct ieee802154_hw *hw, bool on)
{
	struct wpanusb *wpanusb = hw->priv;
	struct usb_device *udev = wpanusb->udev;
	struct set_lbt req = { 0 };
	int ret;

	req.enable = on ? 1 : 0;
	req.duration = cpu_to_le16(on ? DEFAULT_LBT_DURATION_US : 0);

	ret = wpanusb_control_send(wpanusb, usb_sndctrlpipe(udev, 0),
				   SET_LBT, &req, sizeof(req));
	if (ret < 0) {
		dev_err(&udev->dev, "Failed to set LBT state to %s, ret %d",
			on ? "ON" : "OFF", ret);
		return ret;
	}

	if (on) {
		dev_dbg(&udev->dev, "LBT enabled with duration: %u us",
			DEFAULT_LBT_DURATION_US);
	} else {
		dev_dbg(&udev->dev, "LBT disabled");
	}

	return 0;
}

static int wpanusb_set_frame_retries(struct ieee802154_hw *hw, s8 retries)
{
	struct wpanusb *wpanusb = hw->priv;
	struct usb_device *udev = wpanusb->udev;
	struct set_frame_retries req;
	int ret;

	/* IEEE 802.15.4 standard: retries must be 0-7 */
	if (retries < 0 || retries > 7) {
		dev_err(&udev->dev, "Invalid frame retries %d, must be 0-7", retries);
		return -EINVAL;
	}

	req.retries = (u8)retries;

	ret = wpanusb_control_send(wpanusb, usb_sndctrlpipe(udev, 0),
				   SET_FRAME_RETRIES, &req, sizeof(req));
	
	if (ret < 0) {
		dev_err(&udev->dev, "Failed to set frame retries to %d, ret %d", 
			retries, ret);
		return ret;
	}

	dev_dbg(&udev->dev, "Frame retries set to %d", retries);
	return 0;
}

static int wpanusb_set_cca_ed_level(struct ieee802154_hw *hw, s32 mbm)
{
	struct wpanusb *wpanusb = hw->priv;
	struct usb_device *udev = wpanusb->udev;
	struct set_cca_ed_level req;
	int ret;

	req.level_mbm = cpu_to_le32(mbm);

	ret = wpanusb_control_send(wpanusb, usb_sndctrlpipe(udev, 0),
				   SET_CCA_ED_LEVEL, &req, sizeof(req));
	
	if (ret < 0) {
		dev_err(&udev->dev, "Failed to set CCA ED level to %d mbm, ret %d",
			mbm, ret);
		return ret;
	}

	dev_dbg(&udev->dev, "CCA ED level set to %d mbm", mbm);
	return 0;
}

static int wpanusb_set_csma_params(struct ieee802154_hw *hw, u8 min_be,
				   u8 max_be, u8 retries)
{
	struct wpanusb *wpanusb = hw->priv;
	struct usb_device *udev = wpanusb->udev;
	struct set_csma_params req;
	int ret;

	/* IEEE 802.15.4 parameter validation */
	if (min_be > 8 || max_be > 8 || min_be > max_be || retries > 7) {
		dev_err(&udev->dev, "Invalid CSMA params: min_be=%u max_be=%u retries=%u",
			min_be, max_be, retries);
		return -EINVAL;
	}

	req.min_be = min_be;
	req.max_be = max_be;
	req.retries = retries;

	ret = wpanusb_control_send(wpanusb, usb_sndctrlpipe(udev, 0),
				   SET_CSMA_PARAMS, &req, sizeof(req));
	
	if (ret < 0) {
		dev_err(&udev->dev, "Failed to set CSMA params, ret %d", ret);
		return ret;
	}

	dev_dbg(&udev->dev, "CSMA params set: min_be=%u max_be=%u retries=%u",
		min_be, max_be, retries);
	return 0;
}

static int wpanusb_set_promiscuous_mode(struct ieee802154_hw *hw, const bool on)
{
	struct wpanusb *wpanusb = hw->priv;
	struct usb_device *udev = wpanusb->udev;
	struct set_promiscuous_mode req;
	int ret;

	req.enable = on ? 1 : 0;

	ret = wpanusb_control_send(wpanusb, usb_sndctrlpipe(udev, 0),
				   SET_PROMISCUOUS_MODE, &req, sizeof(req));
	
	if (ret < 0) {
		dev_err(&udev->dev, "Failed to set promiscuous mode to %s, ret %d",
			on ? "ON" : "OFF", ret);
		return ret;
	}

	dev_dbg(&udev->dev, "Promiscuous mode set to %s", on ? "ON" : "OFF");
	return 0;
}

static const struct ieee802154_ops wpanusb_ops = {
	.owner			= THIS_MODULE,
	.xmit_async		= wpanusb_xmit,
	.ed			= wpanusb_ed,
	.set_channel		= wpanusb_channel,
	.start			= wpanusb_start,
	.stop			= wpanusb_stop,
	.set_hw_addr_filt	= wpanusb_set_hw_addr_filt,
	.set_txpower		= wpanusb_set_txpower,
	.set_lbt		= wpanusb_set_lbt,
	.set_cca_mode		= wpanusb_set_cca_mode,
	.set_cca_ed_level	= wpanusb_set_cca_ed_level,
	.set_csma_params	= wpanusb_set_csma_params,
	.set_frame_retries	= wpanusb_set_frame_retries,
	.set_promiscuous_mode	= wpanusb_set_promiscuous_mode,
};

/* ----- Setup ------------------------------------------------------------- */

static int wpanusb_probe(struct usb_interface *interface,
			 const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(interface);
	struct ieee802154_hw *hw;
	struct wpanusb *wpanusb;
	int ret;

	hw = ieee802154_alloc_hw(sizeof(struct wpanusb), &wpanusb_ops);
	if (!hw)
		return -ENOMEM;

	wpanusb = hw->priv;
	wpanusb->hw = hw;
	wpanusb->udev = usb_get_dev(udev);
	usb_set_intfdata(interface, wpanusb);

	wpanusb->shutdown = 0;
	INIT_DELAYED_WORK(&wpanusb->work, wpanusb_work_urbs);
	init_usb_anchor(&wpanusb->idle_urbs);
	init_usb_anchor(&wpanusb->rx_urbs);

	ret = wpanusb_alloc_urbs(wpanusb, WPANUSB_NUM_RX_URBS);
	if (ret)
		goto fail;

	wpanusb->tx_dr.bRequestType = VENDOR_OUT;
	wpanusb->tx_dr.bRequest = TX;
	wpanusb->tx_dr.wValue = cpu_to_le16(0);

	wpanusb->tx_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!wpanusb->tx_urb)
		goto fail;

	hw->parent = &udev->dev;

	ret = wpanusb_control_send(wpanusb, usb_sndctrlpipe(udev, 0), RESET,
				   NULL, 0);
	if (ret < 0) {
		dev_err(&udev->dev, "Failed to RESET ieee802154");
		goto fail;
	}

	ret = wpanusb_get_device_capabilities(hw);

	if (ret < 0) {
		dev_err(&udev->dev, "Failed to get device capabilities");
		goto fail;
	}

	ret = wpanusb_set_extended_addr(hw);

	if (ret < 0) {
		dev_err(&udev->dev, "Failed to set permanent address");
		goto fail;
	}

	ret = ieee802154_register_hw(hw);
	if (ret) {
		dev_err(&udev->dev, "Failed to register ieee802154");
		goto fail;
	}

	dev_dbg(&udev->dev, "ieee802154 ready to go");

	return 0;

fail:
	dev_err(&udev->dev, "Failed ieee802154 probe");
	wpanusb_free_urbs(wpanusb);
	usb_kill_urb(wpanusb->tx_urb);
	usb_free_urb(wpanusb->tx_urb);
	usb_put_dev(udev);
	ieee802154_free_hw(hw);

	return ret;
}

static void wpanusb_cleanup_dynamic_caps(struct ieee802154_hw *hw)
{
	/* Free dynamically allocated power levels array */
	if (hw->phy->supported.tx_powers) {
		/* Only free if it's not the static fallback array */
		if (hw->phy->supported.tx_powers != wpanusb_powers) {
			kfree(hw->phy->supported.tx_powers);
			hw->phy->supported.tx_powers = NULL;
		}
	}
}

static void wpanusb_disconnect(struct usb_interface *interface)
{
	struct wpanusb *wpanusb = usb_get_intfdata(interface);

	wpanusb->shutdown = 1;
	cancel_delayed_work_sync(&wpanusb->work);

	usb_kill_anchored_urbs(&wpanusb->rx_urbs);
	wpanusb_free_urbs(wpanusb);
	usb_kill_urb(wpanusb->tx_urb);
	usb_free_urb(wpanusb->tx_urb);

	/* Clean up dynamic capabilities */
	wpanusb_cleanup_dynamic_caps(wpanusb->hw);

	ieee802154_unregister_hw(wpanusb->hw);

	ieee802154_free_hw(wpanusb->hw);

	usb_set_intfdata(interface, NULL);
	usb_put_dev(wpanusb->udev);
}

/* The devices we work with */
static const struct usb_device_id wpanusb_device_table[] = {
	{
		USB_DEVICE_AND_INTERFACE_INFO(WPANUSB_VENDOR_ID,
					      WPANUSB_PRODUCT_ID,
					      USB_CLASS_VENDOR_SPEC,
					      0, 0),
	},
	{
		USB_DEVICE_AND_INTERFACE_INFO(BEAGLECONNECT_VENDOR_ID,
					      BEAGLECONNECT_PRODUCT_ID,
					      USB_CLASS_VENDOR_SPEC,
					      0, 0)
	},
	/* end with null element */
	{}
};
MODULE_DEVICE_TABLE(usb, wpanusb_device_table);

static struct usb_driver wpanusb_driver = {
	.name		= "wpanusb",
	.probe		= wpanusb_probe,
	.disconnect	= wpanusb_disconnect,
	.id_table	= wpanusb_device_table,
};
module_usb_driver(wpanusb_driver);

MODULE_AUTHOR("Andrei Emeltchenko <andrei.emeltchenko@intel.com>");
MODULE_DESCRIPTION("WPANUSB IEEE 802.15.4 over USB Driver");
MODULE_LICENSE("GPL");
