/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Definitions shared between kernel and WPANUSB firmware
 *
 * Copyright (C) 2018 Intel Corp.
 * Copyright (C) 2025 BeagleBoard.org Foundation
 *
 * Written by Andrei Emeltchenko <andrei.emeltchenko@intel.com>
 * Enhanced by Manas Gupta <manasgupta3131@gmail.com>
 */

#define WPANUSB_VENDOR_ID	0x2fe3
#define WPANUSB_PRODUCT_ID	0x0101

#define BEAGLECONNECT_VENDOR_ID  0x2047
#define BEAGLECONNECT_PRODUCT_ID 0x0aa5

/* IEEE 802.15.4 defines pages 0-7, but use 32 for safety */
#ifndef IEEE802154_MAX_PAGE
#define IEEE802154_MAX_PAGE 32
#endif

enum wpanusb_requests {
	RESET,
	TX,
	XMIT_ASYNC,
	ED,
	SET_CHANNEL,
	START,
	STOP,
	SET_SHORT_ADDR,
	SET_PAN_ID,
	SET_IEEE_ADDR,
	SET_TXPOWER,
	SET_CCA_MODE,
	SET_CCA_ED_LEVEL,
	SET_CSMA_PARAMS,
	SET_LBT,
	SET_FRAME_RETRIES,
	SET_PROMISCUOUS_MODE,
	SET_PANC,				/* PAN Coordinator support */
	GET_EXTENDED_ADDR,
	GET_SUPPORTED_CHANNELS,

	/* New capability discovery commands */
	GET_DEVICE_INFO,
	GET_HARDWARE_CAPS,
	GET_PHY_CAPS,
	GET_POWER_LEVELS,
	GET_CHANNEL_PAGES,
};

struct set_channel {
	__u8 page;
	__u8 channel;
} __packed;

struct set_short_addr {
	__le16 short_addr;
} __packed;

struct set_pan_id {
	__le16 pan_id;
} __packed;

struct set_ieee_addr {
	__le64 ieee_addr;
} __packed;

struct set_lbt {
	__u8 enable;        /* 0 = disable, 1 = enable */
	__u8 reserved;      /* padding for alignment */
	__le16 duration;    /* LBT duration in microseconds */
} __packed;

struct set_frame_retries {
	__u8 retries;       /* Number of frame retries (0-7) */
} __packed;

struct set_txpower {
	__le32 power_mbm;   /* TX power in mBm (1/100 dBm) */
} __packed;

struct set_cca_mode {
	__u8 mode;          /* CCA mode (energy/carrier/both) */
	__u8 opt;           /* CCA option parameter */
} __packed;

struct set_cca_ed_level {
	__le32 level_mbm;   /* CCA ED level in milliwatts */
} __packed;

struct set_csma_params {
	__u8 min_be;        /* Minimum backoff exponent (0-5) */
	__u8 max_be;        /* Maximum backoff exponent (0-5) */
	__u8 retries;       /* CSMA retries (0-7) */
} __packed;

struct set_promiscuous_mode {
	__u8 enable;        /* 0 = disable, 1 = enable promiscuous mode */
} __packed;

struct set_panc {
	__u8 enable;        /* 0 = disable, 1 = enable PAN coordinator mode */
} __packed;

/* Device information structure */
struct device_info {
	__u16 device_version;    /* Device firmware version */
	__u16 protocol_version;  /* Communication protocol version */
	__u8 device_type;        /* Device type identifier */
	__u8 capabilities_len;   /* Length of capabilities data */
} __packed;

/* Hardware capabilities structure */
struct hardware_caps {
	__le32 hw_flags;         /* IEEE802154_HW_* flags supported */
	__u8 max_frame_retries;  /* Maximum frame retries supported */
	__u8 has_lbt;           /* Listen Before Talk support */
	__u8 has_cca_modes;     /* Supported CCA modes bitmask */
	__u8 has_promiscuous;   /* Promiscuous mode support */
	__le16 max_lbt_duration; /* Maximum LBT duration in microseconds */
	__u8 reserved[2];       /* Reserved for future use */
} __packed;

/* PHY capabilities structure */
struct phy_caps {
	__le32 phy_flags;       /* WPAN_PHY_FLAG_* flags supported */
	__u8 supported_pages;   /* Number of supported pages */
	__u8 current_page;      /* Default page */
	__le16 cca_ed_level_min; /* Minimum CCA ED level (mbm) */
	__le16 cca_ed_level_max; /* Maximum CCA ED level (mbm) */
	__u8 csma_min_be_range; /* Min/Max BE range (4 bits each) */
	__u8 csma_max_retries;  /* Maximum CSMA retries */
} __packed;

/* Power levels structure - variable length */
struct power_levels {
	__u8 num_levels;        /* Number of power levels */
	__u8 default_level;     /* Default power level index */
	__le32 levels[];        /* Power levels in mbm */
} __packed;

/* Channel page information */
struct channel_page {
	__u8 page;              /* Page number */
	__u8 reserved;          /* Padding */
	__le32 channels_mask;   /* Supported channels bitmask */
} __packed;

/* Channel pages structure - variable length */
struct channel_pages {
	__u8 num_pages;         /* Number of pages */
	__u8 default_page;      /* Default page */
	struct channel_page pages[]; /* Page information */
} __packed;

