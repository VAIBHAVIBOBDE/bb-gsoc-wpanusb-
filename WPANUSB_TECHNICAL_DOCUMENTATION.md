# WPANUSB Driver Technical Documentation

## Overview

The WPANUSB driver provides IEEE 802.15.4 SoftMAC support for USB-based 802.15.4 dongles. This driver is based on the ATUSB design and implements comprehensive dynamic capability discovery, advanced driver operations, and full compliance with IEEE 802.15.4 standards.

**File:** `/home/manas/wpan-bcf-zeph-gsoc/bb-gsoc-wpanusb/wpanusb.c`
**License:** GPL-2.0
**Author:** Andrei Emeltchenko <andrei.emeltchenko@intel.com>

---

## Driver Architecture

### Core Data Structure

```c
struct wpanusb {
    struct ieee802154_hw *hw;
    struct usb_device *udev;
    int shutdown;

    /* RX variables */
    struct delayed_work work;
    struct usb_anchor idle_urbs;
    struct usb_anchor rx_urbs;

    /* TX variables */
    struct usb_ctrlrequest tx_dr;
    struct urb *tx_urb;
    struct sk_buff *tx_skb;
    u8 tx_ack_seq;

    /* Synchronization */
    struct mutex caps_mutex;
};
```

---

## Function Documentation by Category

## 1. USB Control Functions

### wpanusb_control_send()

**Signature:**
```c
static int wpanusb_control_send(struct wpanusb *wpanusb, unsigned int pipe,
                               u8 request, void *data, u16 size)
```

**Purpose:** Sends USB control messages to the device for configuration commands.

**Parameters:**
- `wpanusb`: Driver instance pointer
- `pipe`: USB pipe for the control transfer
- `request`: Command request type (from wpanusb_requests enum)
- `data`: Data payload to send (can be NULL)
- `size`: Size of data payload

**Return Value:** 0 on success, negative error code on failure

**Implementation Details:**
- Uses `usb_control_msg()` with VENDOR_OUT request type
- 1000ms timeout for USB operations
- Synchronous operation

**IEEE 802.15.4 Compliance:** Provides transport layer for all IEEE 802.15.4 configuration commands.

**Dependencies:** None

---

### wpanusb_control_recv()

**Signature:**
```c
static int wpanusb_control_recv(struct wpanusb *wpanusb, u8 request, void *data, u16 size)
```

**Purpose:** Receives data from USB device via control transfers with two-phase operation.

**Parameters:**
- `wpanusb`: Driver instance pointer
- `request`: Command request type
- `data`: Buffer to receive data
- `size`: Expected data size

**Return Value:** Number of bytes received on success, negative error code on failure

**Implementation Details:**
- Two-phase operation: first sends request, then receives response
- Uses separate USB control messages for request and response
- Error logging for debugging failed operations
- 1000ms timeout for each phase

**IEEE 802.15.4 Compliance:** Supports dynamic capability discovery as per modern 802.15.4 implementations.

**Dependencies:** wpanusb_control_send() for request phase

---

## 2. SKB and URB Management Functions

### wpanusb_submit_rx_urb()

**Signature:**
```c
static int wpanusb_submit_rx_urb(struct wpanusb *wpanusb, struct urb *urb)
```

**Purpose:** Submits URB for asynchronous packet reception.

**Parameters:**
- `wpanusb`: Driver instance
- `urb`: USB Request Block to submit

**Return Value:** 0 on success, negative error code on failure

**Implementation Details:**
- Allocates SKB if not present (MAX_RX_XFER = 131 bytes)
- SKB layout: PHR(1) + PSDU(127) + CRC(2) + LQI(1)
- Associates wpanusb instance with SKB via cb field
- Uses bulk pipe endpoint 1 for reception
- Anchors URB for proper lifecycle management

**IEEE 802.15.4 Compliance:**
- Supports maximum PSDU length (127 bytes) as per IEEE 802.15.4
- Handles LQI (Link Quality Indicator) reception

**Dependencies:** wpanusb_bulk_complete()

---

### wpanusb_work_urbs()

**Signature:**
```c
static void wpanusb_work_urbs(struct work_struct *work)
```

**Purpose:** Work queue handler for URB allocation and submission management.

**Parameters:**
- `work`: Delayed work structure

**Return Value:** void

**Implementation Details:**
- Processes idle URBs and resubmits them for reception
- Implements backoff strategy with WPANUSB_ALLOC_DELAY_MS (100ms)
- Respects shutdown flag to prevent operations during cleanup
- Rate-limited warning messages for allocation failures

**IEEE 802.15.4 Compliance:** Ensures continuous packet reception capability.

**Dependencies:** wpanusb_submit_rx_urb()

---

### wpanusb_free_urbs() / wpanusb_alloc_urbs()

**Signature:**
```c
static void wpanusb_free_urbs(struct wpanusb *wpanusb)
static int wpanusb_alloc_urbs(struct wpanusb *wpanusb, unsigned int n)
```

**Purpose:** URB lifecycle management for reception path.

**Parameters:**
- `wpanusb`: Driver instance
- `n`: Number of URBs to allocate (WPANUSB_NUM_RX_URBS = 4)

**Return Value:** 0 on success, -ENOMEM on allocation failure

**Implementation Details:**
- Maintains pool of URBs for continuous reception
- Cleans up associated SKBs during deallocation
- Uses USB anchor for proper URB lifecycle management

**IEEE 802.15.4 Compliance:** Provides sufficient buffering for burst packet reception.

**Dependencies:** None

---

## 3. Packet Processing Functions

### wpanusb_process_urb()

**Signature:**
```c
static void wpanusb_process_urb(struct urb *urb)
```

**Purpose:** Processes received packets and forwards them to the IEEE 802.15.4 stack.

**Parameters:**
- `urb`: Received USB Request Block

**Return Value:** void

**Implementation Details:**
- Validates packet length against IEEE 802.15.4 limits
- Handles special case: 1-byte URBs are TX acknowledgments
- Extracts LQI from packet trailer
- Validates PSDU length using `ieee802154_is_valid_psdu_len()`
- Forwards valid packets to stack via `ieee802154_rx_irqsafe()`
- Debug hex dump for packet inspection

**IEEE 802.15.4 Compliance:**
- Strict PSDU length validation (5-127 bytes)
- Proper LQI handling for link quality assessment
- Frame format: Length + PSDU + LQI

**Dependencies:** wpanusb_tx_done(), ieee802154_rx_irqsafe()

---

### wpanusb_bulk_complete()

**Signature:**
```c
static void wpanusb_bulk_complete(struct urb *urb)
```

**Purpose:** URB completion callback for bulk transfers.

**Parameters:**
- `urb`: Completed URB

**Return Value:** void

**Implementation Details:**
- Handles URB status codes (-ENOENT for cancellation)
- Processes successful URBs via wpanusb_process_urb()
- Requeues URB for continued reception
- Error handling with debug logging

**IEEE 802.15.4 Compliance:** Maintains continuous packet reception as required for network operation.

**Dependencies:** wpanusb_process_urb()

---

### wpanusb_tx_done()

**Signature:**
```c
static void wpanusb_tx_done(struct wpanusb *wpanusb, uint8_t seq)
```

**Purpose:** Handles transmission acknowledgment from hardware.

**Parameters:**
- `wpanusb`: Driver instance
- `seq`: Sequence number from hardware ACK

**Return Value:** void

**Implementation Details:**
- Validates sequence number against expected value
- Calls `ieee802154_xmit_complete()` for successful transmission
- Handles sequence mismatch with proper SKB cleanup
- Debug logging for transmission tracking

**IEEE 802.15.4 Compliance:** Provides transmission confirmation required for MAC layer operation.

**Dependencies:** ieee802154_xmit_complete()

---

## 4. IEEE 802.15.4 Driver Operations

### wpanusb_xmit()

**Signature:**
```c
static int wpanusb_xmit(struct ieee802154_hw *hw, struct sk_buff *skb)
```

**Purpose:** Transmits IEEE 802.15.4 frames asynchronously.

**Parameters:**
- `hw`: IEEE 802.15.4 hardware abstraction
- `skb`: Socket buffer containing frame to transmit

**Return Value:** 0 on success, negative error code on failure

**Implementation Details:**
- Generates sequence numbers (1-255, wrapping)
- Uses USB control transfer with TX command
- Stores SKB for completion handling
- Atomic context operation (GFP_ATOMIC)
- Sets up control request with sequence number and length

**IEEE 802.15.4 Compliance:** Implements standard asynchronous transmission interface.

**Dependencies:** wpanusb_xmit_complete()

---

### wpanusb_channel()

**Signature:**
```c
static int wpanusb_channel(struct ieee802154_hw *hw, u8 page, u8 channel)
```

**Purpose:** Sets channel and page for IEEE 802.15.4 operation.

**Parameters:**
- `hw`: IEEE 802.15.4 hardware abstraction
- `page`: Channel page (0-7)
- `channel`: Channel number (page-dependent)

**Return Value:** 0 on success, negative error code on failure

**Implementation Details:**
- Validates page against IEEE802154_MAX_PAGE
- Checks channel support via hw->phy->supported.channels
- Sends SET_CHANNEL command with page and channel
- Comprehensive error handling and logging

**IEEE 802.15.4 Compliance:**
- Supports all IEEE 802.15.4 channel pages
- Validates channel availability per page
- Standard channel setting interface

**Dependencies:** wpanusb_control_send()

---

### wpanusb_ed()

**Signature:**
```c
static int wpanusb_ed(struct ieee802154_hw *hw, u8 *level)
```

**Purpose:** Performs Energy Detection (ED) scan on current channel.

**Parameters:**
- `hw`: IEEE 802.15.4 hardware abstraction
- `level`: Pointer to store detected energy level

**Return Value:** 0 on success, negative error code on failure

**Implementation Details:**
- Validates level pointer with WARN_ON()
- Requests energy detection from hardware
- Returns hardware-measured energy level
- Error handling with device-specific logging

**IEEE 802.15.4 Compliance:** Implements standard ED scan as per IEEE 802.15.4-2015.

**Dependencies:** wpanusb_control_recv()

---

### wpanusb_set_hw_addr_filt()

**Signature:**
```c
static int wpanusb_set_hw_addr_filt(struct ieee802154_hw *hw,
                                   struct ieee802154_hw_addr_filt *filt,
                                   unsigned long changed)
```

**Purpose:** Configures hardware address filtering parameters.

**Parameters:**
- `hw`: IEEE 802.15.4 hardware abstraction
- `filt`: Address filter configuration
- `changed`: Bitmask of changed parameters

**Return Value:** 0 on success, negative error code on failure

**Implementation Details:**
- Supports short address (IEEE802154_AFILT_SADDR_CHANGED)
- Supports PAN ID (IEEE802154_AFILT_PANID_CHANGED)
- Supports IEEE extended address (IEEE802154_AFILT_IEEEADDR_CHANGED)
- Supports PAN coordinator mode (IEEE802154_AFILT_PANC_CHANGED)
- Individual command per changed parameter
- Comprehensive error handling for each parameter

**IEEE 802.15.4 Compliance:**
- Full address filtering support as per IEEE 802.15.4
- Proper handling of all address types
- PAN coordinator functionality

**Dependencies:** wpanusb_control_send()

---

### wpanusb_start() / wpanusb_stop()

**Signature:**
```c
static int wpanusb_start(struct ieee802154_hw *hw)
static void wpanusb_stop(struct ieee802154_hw *hw)
```

**Purpose:** Start/stop IEEE 802.15.4 hardware operation.

**Parameters:**
- `hw`: IEEE 802.15.4 hardware abstraction

**Return Value:** 0 on success, negative error code on failure (start only)

**Implementation Details:**
- **Start:** Schedules URB work, sends START command
- **Stop:** Kills URBs, sends STOP command
- Proper error handling and cleanup
- Ensures hardware state consistency

**IEEE 802.15.4 Compliance:** Standard start/stop interface for MAC layer control.

**Dependencies:** wpanusb_control_send()

---

## 5. Advanced Driver Operations (New Features)

### wpanusb_set_txpower()

**Signature:**
```c
static int wpanusb_set_txpower(struct ieee802154_hw *hw, s32 mbm)
```

**Purpose:** Sets transmission power level with validation against supported levels.

**Parameters:**
- `hw`: IEEE 802.15.4 hardware abstraction
- `mbm`: Power level in milliwatts (mbm)

**Return Value:** 0 on success, negative error code on failure

**Implementation Details:**
- Validates power level against hw->phy->supported.tx_powers array
- Iterates through all supported levels for exact match
- Sends SET_TXPOWER command with little-endian conversion
- Comprehensive error handling and logging
- Returns -EOPNOTSUPP if no power levels available
- Returns -EINVAL for unsupported power levels

**IEEE 802.15.4 Compliance:**
- Supports standard power level control
- Validates against device-specific supported levels
- Proper power management for regulatory compliance

**Dependencies:** wpanusb_control_send()

---

### wpanusb_set_lbt()

**Signature:**
```c
static int wpanusb_set_lbt(struct ieee802154_hw *hw, bool on)
```

**Purpose:** Configures Listen Before Talk (LBT) functionality.

**Parameters:**
- `hw`: IEEE 802.15.4 hardware abstraction
- `on`: Enable/disable LBT

**Return Value:** 0 on success, negative error code on failure

**Implementation Details:**
- Sets LBT enable flag (0/1)
- Uses DEFAULT_LBT_DURATION_US (1000µs) when enabled
- Sets duration to 0 when disabled
- Little-endian conversion for duration field
- Clear status reporting in debug logs

**IEEE 802.15.4 Compliance:**
- Implements LBT for spectrum etiquette
- Standard duration values for regulatory compliance
- Optional feature as per IEEE 802.15.4g

**Dependencies:** wpanusb_control_send()

---

### wpanusb_set_cca_mode()

**Signature:**
```c
static int wpanusb_set_cca_mode(struct ieee802154_hw *hw,
                               const struct wpan_phy_cca *cca)
```

**Purpose:** Configures Clear Channel Assessment (CCA) mode and options.

**Parameters:**
- `hw`: IEEE 802.15.4 hardware abstraction
- `cca`: CCA configuration structure

**Return Value:** 0 on success, negative error code on failure

**Implementation Details:**
- Supports three CCA modes:
  - NL802154_CCA_ENERGY: Energy Detection only
  - NL802154_CCA_CARRIER: Carrier Sense only
  - NL802154_CCA_ENERGY_CARRIER: Combined mode
- Validates CCA mode before sending to hardware
- Passes both mode and option parameters
- Descriptive debug logging for each mode type

**IEEE 802.15.4 Compliance:**
- Full CCA mode support as per IEEE 802.15.4-2015
- Proper validation of standard CCA modes
- Support for combined detection mechanisms

**Dependencies:** wpanusb_control_send()

---

### wpanusb_set_cca_ed_level()

**Signature:**
```c
static int wpanusb_set_cca_ed_level(struct ieee802154_hw *hw, s32 mbm)
```

**Purpose:** Sets CCA Energy Detection threshold level.

**Parameters:**
- `hw`: IEEE 802.15.4 hardware abstraction
- `mbm`: ED level in milliwatts (mbm)

**Return Value:** 0 on success, negative error code on failure

**Implementation Details:**
- Converts power level to little-endian format
- Sends SET_CCA_ED_LEVEL command
- No validation (assumes MAC layer validates range)
- Clear error reporting

**IEEE 802.15.4 Compliance:**
- Standard CCA ED level configuration
- Supports fine-grained energy detection tuning
- Required for adaptive channel access

**Dependencies:** wpanusb_control_send()

---

### wpanusb_set_csma_params()

**Signature:**
```c
static int wpanusb_set_csma_params(struct ieee802154_hw *hw, u8 min_be,
                                  u8 max_be, u8 retries)
```

**Purpose:** Configures CSMA/CA backoff and retry parameters.

**Parameters:**
- `hw`: IEEE 802.15.4 hardware abstraction
- `min_be`: Minimum backoff exponent (0-8)
- `max_be`: Maximum backoff exponent (0-8)
- `retries`: Maximum CSMA retries (0-7)

**Return Value:** 0 on success, negative error code on failure

**Implementation Details:**
- Validates IEEE 802.15.4 parameter ranges:
  - min_be, max_be: ≤ 8
  - min_be ≤ max_be
  - retries: ≤ 7
- Returns -EINVAL for invalid parameters
- Sends all three parameters in single command
- Detailed parameter logging

**IEEE 802.15.4 Compliance:**
- Full CSMA/CA parameter control per IEEE 802.15.4
- Proper validation of standard parameter ranges
- Support for adaptive backoff mechanisms

**Dependencies:** wpanusb_control_send()

---

### wpanusb_set_frame_retries()

**Signature:**
```c
static int wpanusb_set_frame_retries(struct ieee802154_hw *hw, s8 retries)
```

**Purpose:** Sets maximum number of frame retransmission attempts.

**Parameters:**
- `hw`: IEEE 802.15.4 hardware abstraction
- `retries`: Number of retries (0-7)

**Return Value:** 0 on success, negative error code on failure

**Implementation Details:**
- Validates retry count against IEEE 802.15.4 limits (0-7)
- Returns -EINVAL for out-of-range values
- Converts signed to unsigned for hardware interface
- Clear range validation error messages

**IEEE 802.15.4 Compliance:**
- Standard frame retry mechanism per IEEE 802.15.4
- Proper validation of standard retry limits
- Essential for reliable data transmission

**Dependencies:** wpanusb_control_send()

---

### wpanusb_set_promiscuous_mode()

**Signature:**
```c
static int wpanusb_set_promiscuous_mode(struct ieee802154_hw *hw, const bool on)
```

**Purpose:** Enables/disables promiscuous packet reception mode.

**Parameters:**
- `hw`: IEEE 802.15.4 hardware abstraction
- `on`: Enable/disable promiscuous mode

**Return Value:** 0 on success, negative error code on failure

**Implementation Details:**
- Simple boolean to hardware flag conversion (0/1)
- Sends SET_PROMISCUOUS_MODE command
- Clear status reporting in logs
- No validation required (boolean input)

**IEEE 802.15.4 Compliance:**
- Standard promiscuous mode for monitoring and analysis
- Required for packet capture and network analysis tools
- Bypass of hardware address filtering

**Dependencies:** wpanusb_control_send()

---

## 6. Dynamic Capability Discovery Functions

### wpanusb_get_device_info()

**Signature:**
```c
static int wpanusb_get_device_info(struct wpanusb *wpanusb, struct device_info *info)
```

**Purpose:** Retrieves basic device identification and version information.

**Parameters:**
- `wpanusb`: Driver instance
- `info`: Device information structure to populate

**Return Value:** 0 on success, negative error code on failure

**Implementation Details:**
- Queries GET_DEVICE_INFO from hardware
- Extracts device version (major.minor format)
- Extracts protocol version for compatibility checking
- Logs version information for debugging
- Little-endian conversion of version fields

**IEEE 802.15.4 Compliance:** Enables version-specific feature negotiation and compatibility validation.

**Dependencies:** wpanusb_control_recv()

---

### wpanusb_get_hardware_caps()

**Signature:**
```c
static int wpanusb_get_hardware_caps(struct wpanusb *wpanusb, struct hardware_caps *caps)
```

**Purpose:** Retrieves hardware-specific capability information.

**Parameters:**
- `wpanusb`: Driver instance
- `caps`: Hardware capabilities structure to populate

**Return Value:** 0 on success, negative error code on failure

**Implementation Details:**
- Queries GET_HARDWARE_CAPS from device
- Receives hardware flags, LBT support, CCA modes
- Extracts maximum frame retries and LBT duration limits
- Debug logging of capability flags
- Enables dynamic hardware feature discovery

**IEEE 802.15.4 Compliance:** Allows driver to adapt to hardware-specific IEEE 802.15.4 feature support.

**Dependencies:** wpanusb_control_recv()

---

### wpanusb_get_phy_caps()

**Signature:**
```c
static int wpanusb_get_phy_caps(struct wpanusb *wpanusb, struct phy_caps *caps)
```

**Purpose:** Retrieves PHY layer capability information.

**Parameters:**
- `wpanusb`: Driver instance
- `caps`: PHY capabilities structure to populate

**Return Value:** 0 on success, negative error code on failure

**Implementation Details:**
- Queries GET_PHY_CAPS from device
- Receives PHY flags, supported pages count
- Gets CCA ED level ranges and CSMA parameter limits
- Debug logging of PHY-specific capabilities
- Little-endian conversion of multi-byte fields

**IEEE 802.15.4 Compliance:** Enables PHY-specific feature configuration per IEEE 802.15.4 PHY variants.

**Dependencies:** wpanusb_control_recv()

---

### wpanusb_get_power_levels()

**Signature:**
```c
static int wpanusb_get_power_levels(struct wpanusb *wpanusb, s32 **power_levels, size_t *count)
```

**Purpose:** Retrieves available transmission power levels from hardware.

**Parameters:**
- `wpanusb`: Driver instance
- `power_levels`: Pointer to allocated power level array
- `count`: Number of power levels returned

**Return Value:** 0 on success, negative error code on failure

**Implementation Details:**
- Allocates buffer for maximum 32 power levels
- Queries GET_POWER_LEVELS with variable-length response
- Validates level count and default level index
- Converts little-endian power values to host format
- Allocates permanent storage for power level array
- Comprehensive validation of returned data

**IEEE 802.15.4 Compliance:**
- Supports device-specific power level granularity
- Enables regulatory compliance through proper power control
- Provides accurate power level information for upper layers

**Dependencies:** wpanusb_control_recv()

---

### wpanusb_get_channel_pages()

**Signature:**
```c
static int wpanusb_get_channel_pages(struct wpanusb *wpanusb, struct ieee802154_hw *hw)
```

**Purpose:** Retrieves supported channel pages and their available channels.

**Parameters:**
- `wpanusb`: Driver instance
- `hw`: IEEE 802.15.4 hardware abstraction to configure

**Return Value:** 0 on success, negative error code on failure

**Implementation Details:**
- Supports up to 8 channel pages (IEEE 802.15.4 standard)
- Queries GET_CHANNEL_PAGES with variable-length response
- Validates page count and individual page numbers
- Configures hw->phy->supported.channels per page
- Sets default page and channel from device response
- Little-endian conversion of channel masks

**IEEE 802.15.4 Compliance:**
- Full channel page support per IEEE 802.15.4
- Proper channel availability discovery
- Support for multiple PHY variants

**Dependencies:** wpanusb_control_recv()

---

### wpanusb_validate_capabilities()

**Signature:**
```c
static bool wpanusb_validate_capabilities(struct ieee802154_hw *hw,
                                         struct hardware_caps *hw_caps,
                                         struct phy_caps *phy_caps)
```

**Purpose:** Validates received capability data for correctness and consistency.

**Parameters:**
- `hw`: IEEE 802.15.4 hardware abstraction
- `hw_caps`: Hardware capabilities to validate
- `phy_caps`: PHY capabilities to validate

**Return Value:** true if valid, false if invalid

**Implementation Details:**
- Validates frame retry limits (≤ 7) per IEEE 802.15.4
- Validates page count (1-8 pages) per IEEE 802.15.4
- Validates default page against supported pages
- Returns false for any validation failure
- Comprehensive warning messages for invalid data

**IEEE 802.15.4 Compliance:** Ensures capability data conforms to IEEE 802.15.4 standards.

**Dependencies:** None

---

### wpanusb_generate_fallback_powers()

**Signature:**
```c
static s32 *wpanusb_generate_fallback_powers(size_t *count)
```

**Purpose:** Generates fallback power levels when hardware query fails.

**Parameters:**
- `count`: Returns number of generated power levels

**Return Value:** Allocated power level array or NULL on failure

**Implementation Details:**
- Provides 10 common IEEE 802.15.4 power levels
- Range: +5 dBm to -20 dBm in reasonable steps
- Allocates dynamic array for consistent memory management
- Powers in milliwatt format (mbm) as per kernel interface
- Static fallback data for maximum compatibility

**IEEE 802.15.4 Compliance:** Provides reasonable power levels covering typical IEEE 802.15.4 device ranges.

**Dependencies:** None

---

### wpanusb_get_device_capabilities()

**Signature:**
```c
static int wpanusb_get_device_capabilities(struct ieee802154_hw *hw)
```

**Purpose:** Master function for comprehensive device capability discovery with fallback.

**Parameters:**
- `hw`: IEEE 802.15.4 hardware abstraction to configure

**Return Value:** 0 (always returns success, uses fallback on failure)

**Implementation Details:**
- **Step 1:** Get basic device info
- **Step 2:** Get hardware capabilities
- **Step 3:** Get PHY capabilities
- **Step 4:** Validate all capabilities
- **Step 5:** Get power levels
- **Step 6:** Get channel pages
- **Configuration:** Set hardware and PHY flags dynamically
- **Fallback:** Complete fallback system with hardcoded values
- **Protection:** Uses caps_mutex for thread-safe capability updates

**Dynamic Configuration Features:**
- Sets IEEE802154_HW_LBT if hardware supports LBT
- Sets IEEE802154_HW_CSMA_PARAMS if CCA modes supported
- Sets IEEE802154_HW_FRAME_RETRIES if retry control available
- Sets IEEE802154_HW_PROMISCUOUS if promiscuous mode supported
- Configures WPAN_PHY_FLAG_* based on device capabilities

**Fallback System:**
- Hardcoded basic flags: IEEE802154_HW_TX_OMIT_CKSUM | IEEE802154_HW_AFILT
- Generated power levels via wpanusb_generate_fallback_powers()
- Default channel support (WPANUSB_VALID_CHANNELS = 0x07FFFFFF)
- Fallback supported channels query if dynamic discovery fails

**IEEE 802.15.4 Compliance:**
- Comprehensive feature negotiation
- Graceful degradation for older hardware
- Full standard compliance in both dynamic and fallback modes

**Dependencies:** All capability query functions, validation function, fallback generation

---

## 7. Device Lifecycle Functions

### wpanusb_set_extended_addr()

**Signature:**
```c
static int wpanusb_set_extended_addr(struct ieee802154_hw *hw)
```

**Purpose:** Retrieves and sets permanent IEEE 802.15.4 extended address.

**Parameters:**
- `hw`: IEEE 802.15.4 hardware abstraction

**Return Value:** 0 on success, negative error code on failure

**Implementation Details:**
- Queries hardware for stored extended address
- Validates address using `ieee802154_is_valid_extended_unicast_addr()`
- Falls back to random address generation if invalid/empty
- Uses `ieee802154_random_extended_addr()` for fallback
- Proper byte order handling (swab64) for logging
- Sets hw->phy->perm_extended_addr for MAC layer

**IEEE 802.15.4 Compliance:**
- Ensures valid unicast extended addresses
- Proper extended address format and validation
- Random address generation per IEEE 802.15.4 standards

**Dependencies:** wpanusb_control_send(), ieee802154_is_valid_extended_unicast_addr()

---

### wpanusb_probe()

**Signature:**
```c
static int wpanusb_probe(struct usb_interface *interface, const struct usb_device_id *id)
```

**Purpose:** USB device probe and driver initialization.

**Parameters:**
- `interface`: USB interface being probed
- `id`: Matching device ID entry

**Return Value:** 0 on success, negative error code on failure

**Implementation Details:**
- **Hardware Allocation:** `ieee802154_alloc_hw()` with driver ops
- **USB Setup:** Device reference, interface data, anchors initialization
- **URB Allocation:** WPANUSB_NUM_RX_URBS (4) receive URBs
- **TX Setup:** Control request structure and URB allocation
- **Reset:** Device reset via RESET command
- **Capability Discovery:** wpanusb_get_device_capabilities()
- **Address Setup:** wpanusb_set_extended_addr()
- **Registration:** ieee802154_register_hw()
- **Cleanup:** Comprehensive failure path cleanup
- **Synchronization:** mutex_init() for capability updates

**IEEE 802.15.4 Compliance:** Full IEEE 802.15.4 hardware initialization and registration.

**Dependencies:** All capability and setup functions

---

### wpanusb_cleanup_dynamic_caps() / wpanusb_disconnect()

**Signature:**
```c
static void wpanusb_cleanup_dynamic_caps(struct ieee802154_hw *hw)
static void wpanusb_disconnect(struct usb_interface *interface)
```

**Purpose:** Clean shutdown and resource deallocation.

**Parameters:**
- `hw`: IEEE 802.15.4 hardware abstraction (cleanup function)
- `interface`: USB interface being disconnected

**Return Value:** void

**Implementation Details:**
- **Shutdown Flag:** Prevents new operations
- **Work Cancellation:** cancel_delayed_work_sync()
- **URB Cleanup:** Kill and free all URBs
- **Dynamic Memory:** Free power level arrays and other dynamic allocations
- **IEEE Registration:** ieee802154_unregister_hw()
- **Synchronization:** mutex_destroy() for proper cleanup
- **USB Cleanup:** Interface data clearing and device reference release

**IEEE 802.15.4 Compliance:** Proper IEEE 802.15.4 hardware deregistration.

**Dependencies:** wpanusb_cleanup_dynamic_caps()

---

## 8. Driver Operations Structure

### wpanusb_ops

**Structure:**
```c
static const struct ieee802154_ops wpanusb_ops = {
    .owner                = THIS_MODULE,
    .xmit_async          = wpanusb_xmit,
    .ed                  = wpanusb_ed,
    .set_channel         = wpanusb_channel,
    .start               = wpanusb_start,
    .stop                = wpanusb_stop,
    .set_hw_addr_filt    = wpanusb_set_hw_addr_filt,
    .set_txpower         = wpanusb_set_txpower,
    .set_lbt             = wpanusb_set_lbt,
    .set_cca_mode        = wpanusb_set_cca_mode,
    .set_cca_ed_level    = wpanusb_set_cca_ed_level,
    .set_csma_params     = wpanusb_set_csma_params,
    .set_frame_retries   = wpanusb_set_frame_retries,
    .set_promiscuous_mode = wpanusb_set_promiscuous_mode,
};
```

**Purpose:** Complete IEEE 802.15.4 driver operations interface.

**IEEE 802.15.4 Compliance:**
- **Core Operations:** xmit_async, ed, set_channel, start, stop
- **Address Filtering:** set_hw_addr_filt
- **Power Management:** set_txpower
- **Channel Access:** set_lbt, set_cca_mode, set_cca_ed_level, set_csma_params
- **Reliability:** set_frame_retries
- **Monitoring:** set_promiscuous_mode

**Advanced Features:** All new driver operations are included, making this one of the most feature-complete IEEE 802.15.4 USB drivers.

---

## Constants and Macros

```c
#define WPANUSB_NUM_RX_URBS      4     /* Receive URB pool size */
#define WPANUSB_ALLOC_DELAY_MS   100   /* Allocation retry delay */
#define WPANUSB_VALID_CHANNELS   0x07FFFFFF /* Default channel mask */
#define DEFAULT_LBT_DURATION_US  1000  /* Default LBT duration */
#define MAX_PSDU                 127   /* Maximum PSDU length */
#define MAX_RX_XFER              131   /* Maximum RX transfer size */
```

---

## Key Implementation Highlights

### Thread Safety
- **caps_mutex:** Protects dynamic capability discovery and updates
- **USB anchors:** Proper URB lifecycle management
- **Atomic allocations:** GFP_ATOMIC for TX path operations

### Error Handling
- **Comprehensive validation:** All input parameters validated
- **Fallback mechanisms:** Dynamic capability discovery with fallback
- **Proper cleanup:** All error paths include complete resource cleanup
- **Rate-limited logging:** Prevents log spam during continuous failures

### IEEE 802.15.4 Compliance
- **Standard parameter ranges:** All parameters validated against IEEE 802.15.4 limits
- **Proper frame handling:** PSDU length validation, LQI processing
- **Complete feature set:** All major IEEE 802.15.4 features supported
- **Capability negotiation:** Dynamic feature discovery enables optimal hardware utilization

### Performance Optimization
- **URB pooling:** Multiple receive URBs for burst handling
- **Asynchronous operations:** Non-blocking TX and RX operations
- **Efficient memory management:** Dynamic allocation with proper cleanup
- **Work queue scheduling:** Deferred work for URB management

---

## Conclusion

The WPANUSB driver represents a comprehensive, modern IEEE 802.15.4 USB driver implementation with:

1. **Complete IEEE 802.15.4 compliance** with all standard operations
2. **Advanced driver features** including LBT, CCA modes, CSMA parameters, and promiscuous mode
3. **Dynamic capability discovery** with robust fallback mechanisms
4. **Thread-safe operation** with proper synchronization
5. **Comprehensive error handling** and resource management
6. **Performance optimization** for both latency and throughput
