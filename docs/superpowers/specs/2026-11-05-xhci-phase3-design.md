# xHCI Phase 3: Configuration Descriptor Parsing, Endpoint Configuration & Set Configuration

**Author**: Antigravity  
**Date**: 2026-11-05  
**Target Milestone**: xHCI Phase 3 (Device Configuration & Endpoint Ring Setup)  
**Status**: Revised Specification (Incorporating Review Feedback)  

---

## 1. Executive Summary & Goals

Phase 1 established the xHCI host controller skeleton, MMIO accessors, 5-step reset handshake, DCBAA, Command Ring (with Link TRB wrap), Event Ring (ERST), Interrupter 0, and Run/Stop state machine.  
Phase 2 completed root port reset, slot allocation, device addressing, EP0 Transfer Ring management, and standard USB 18-byte Device Descriptor retrieval.

Phase 3 transitions the discovered USB device from the **Addressed** state to the **Configured** state:
1. **Full Configuration Descriptor Hierarchy Retrieval**:
   - Read 9-byte Configuration Descriptor header using EP0 control transfer (`GET_DESCRIPTOR`, Type 2, Index 0) to extract `wTotalLength`.
   - Validate `9 <= wTotalLength <= 4096`.
   - Read full descriptor hierarchy into the slot's dedicated 4KB DMA buffer. Accept both `XHCI_COMP_SUCCESS` and `XHCI_COMP_SHORT_TX`, clamping the valid parse buffer to $\min(\text{actual}, w\text{TotalLength})$ with a minimum required length of 9 bytes.
   - Parse the descriptor hierarchy with strict malformed-descriptor guards: Configuration Descriptor (`bConfigurationValue`), Interface Descriptors (`bInterfaceNumber`, `bInterfaceClass`, `bInterfaceSubClass`, `bInterfaceProtocol`), HID Descriptors (if present), and Endpoint Descriptors (`bEndpointAddress`, `bmAttributes`, `wMaxPacketSize`, `bInterval`).
2. **Dynamic Endpoint Ring Allocation & Safe Lifecycle**:
   - Strict dynamic allocation per user requirement: allocate pointer table `struct xhci_ep_ring **ep_rings` dynamically sized strictly to `(max_dci + 1)` pointers.
   - For each active device endpoint (e.g., Interrupt IN endpoint for USB Tablet/Mouse/Keyboard, $2 \le \text{DCI} \le 31$), dynamically allocate `struct xhci_ep_ring` and its dedicated 4KB DMA page (initialized with Link TRB wrap at TRB 255 with `TC=1`).
   - Provide centralized, leak-free teardown via `xhci_free_endpoint_rings(slot)` for both OOM rollback and device removal.
3. **Endpoint Context Configuration & `Configure Endpoint Command`**:
   - Populate `Input Control Context`: `Drop Flags = 0`, `Add Flags = (1U << 0) | (1U << dci_1) | ...`.
   - Populate `Slot Context`: update `Context Entries = max_dci`.
   - Calculate xHCI Interval exponents accurately ($2^{\text{Interval}-3} \times 1\text{ms}$ for Full/Low-speed Interrupt).
   - Populate `Endpoint Context` for each active endpoint: `EP Type` (e.g. Interrupt IN = 7), `Max Packet Size`, `Interval`, `CErr = 3`, `DCS = 1`, and Transfer Ring physical base address.
   - Issue `Configure Endpoint Command` (TRB Type 12) via Command Ring, ring Doorbell 0, and poll for `Command Completion Event` matching the command TRB.
4. **Device Activation via `SET_CONFIGURATION`**:
   - Issue standard 2-stage control transfer (Setup Stage TRB -> Status Stage TRB, without data stage) with `bRequest = 0x09` (`SET_CONFIGURATION`) and `wValue = bConfigurationValue`.
   - Ring Slot Doorbell (Target = 1, EP0) and poll for `Transfer Event` (`SUCCESS = 1`).
   - Log configured device state: interface class/subclass, active endpoints, and configuration value.
5. **Robust Resource Teardown & Lifecycle**:
   - In `xhci_free_slot()`, invoke `xhci_free_endpoint_rings()` to release all dynamically allocated endpoint ring structures and their DMA pages.

---

## 2. Architecture & Data Structures

### 2.1 Standard USB Configuration & Interface Descriptors
Add definitions to `kernel/include/driver/xhci.h`:

```c
/* USB Descriptor Types */
#define USB_DESC_TYPE_DEVICE        1
#define USB_DESC_TYPE_CONFIGURATION 2
#define USB_DESC_TYPE_STRING        3
#define USB_DESC_TYPE_INTERFACE     4
#define USB_DESC_TYPE_ENDPOINT      5
#define USB_DESC_TYPE_HID           0x21
#define USB_DESC_TYPE_REPORT        0x22

/* xHCI Command TRB Types */
#define XHCI_TRB_TYPE_CONFIG_EP     12

/* USB Standard Configuration Descriptor (9 bytes) */
struct usb_config_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t wTotalLength;
    uint8_t  bNumInterfaces;
    uint8_t  bConfigurationValue;
    uint8_t  iConfiguration;
    uint8_t  bmAttributes;
    uint8_t  bMaxPower;
} __attribute__((packed));

/* USB Standard Interface Descriptor (9 bytes) */
struct usb_interface_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bInterfaceNumber;
    uint8_t  bAlternateSetting;
    uint8_t  bNumEndpoints;
    uint8_t  bInterfaceClass;
    uint8_t  bInterfaceSubClass;
    uint8_t  bInterfaceProtocol;
    uint8_t  iInterface;
} __attribute__((packed));

/* USB Standard Endpoint Descriptor (7 bytes) */
struct usb_endpoint_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint8_t  bEndpointAddress;  /* bit 7: 1=IN, 0=OUT; bits 3:0: EP num */
    uint8_t  bmAttributes;      /* bits 1:0: 0=Ctrl, 1=Isoch, 2=Bulk, 3=Intr */
    uint16_t wMaxPacketSize;
    uint8_t  bInterval;
} __attribute__((packed));

/* USB HID Descriptor (9 bytes, optional) */
struct usb_hid_descriptor {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdHID;
    uint8_t  bCountryCode;
    uint8_t  bNumDescriptors;
    uint8_t  bReportDescriptorType;
    uint16_t wReportDescriptorLength;
} __attribute__((packed));
```

### 2.2 Dynamic Endpoint Ring Structure & Slot State
```c
/* xHCI Endpoint Types (Endpoint Context DW1 bits [5:3]) */
#define XHCI_EP_TYPE_INVALID        0
#define XHCI_EP_TYPE_ISOCH_OUT      1
#define XHCI_EP_TYPE_BULK_OUT       2
#define XHCI_EP_TYPE_INTR_OUT       3
#define XHCI_EP_TYPE_CONTROL        4
#define XHCI_EP_TYPE_ISOCH_IN       5
#define XHCI_EP_TYPE_BULK_IN        6
#define XHCI_EP_TYPE_INTR_IN        7

/* Single Endpoint Transfer Ring Instance */
struct xhci_ep_ring {
    uint64_t ring_phys;             /* 4KB DMA page physical address */
    struct xhci_trb *ring_virt;     /* Virtual address */
    uint32_t enqueue;               /* Enqueue index (0..254) */
    uint8_t cycle;                  /* Producer cycle state */
    uint8_t dci;                    /* Device Context Index (1..31) */
    uint8_t ep_type;                /* XHCI_EP_TYPE_* */
    uint16_t max_packet_size;       /* Endpoint max packet size */
    uint8_t interval;               /* Computed xHCI interval exponent */
    bool active;
};

/* Parsed Interface Summary */
struct xhci_interface_info {
    uint8_t interface_num;
    uint8_t class_code;
    uint8_t subclass_code;
    uint8_t protocol;
    uint8_t num_endpoints;
    bool active;
};

/* Updated struct xhci_slot in kernel/include/driver/xhci.h */
struct xhci_slot {
    uint8_t slot_id;
    uint8_t port_num;
    uint8_t speed;
    uint16_t max_packet_size;
    bool active;
    bool configured;

    /* Base DMA Contexts */
    uint64_t input_ctx_phys;
    void *input_ctx_virt;

    uint64_t dev_ctx_phys;
    void *dev_ctx_virt;

    /* Dedicated 4KB DMA Data Buffer for Control Transfers */
    uint64_t dma_buf_phys;
    void *dma_buf_virt;

    /* EP0 Transfer Ring */
    uint64_t ep0_ring_phys;
    struct xhci_trb *ep0_ring_virt;
    uint32_t ep0_ring_enqueue;
    uint8_t ep0_ring_cycle;

    /* Dynamically Allocated Endpoint Rings Table (Array of pointers sized max_dci + 1) */
    struct xhci_ep_ring **ep_rings;
    uint8_t max_dci;

    /* Descriptors & State */
    struct usb_device_descriptor desc;
    struct usb_config_descriptor config_desc;
    struct xhci_interface_info iface;
};

/* Function prototypes */
void xhci_free_endpoint_rings(struct xhci_slot *slot);
int xhci_get_config_descriptor(struct xhci_controller *ctrl, uint8_t slot_id, struct usb_config_descriptor *out_desc);
int xhci_parse_config_descriptor(struct xhci_slot *slot, const uint8_t *buffer, uint16_t length);
int xhci_setup_endpoint_rings(struct xhci_slot *slot, const uint8_t *buffer, uint16_t length);
int xhci_configure_endpoints(struct xhci_controller *ctrl, uint8_t slot_id);
int xhci_set_configuration(struct xhci_controller *ctrl, uint8_t slot_id, uint8_t config_value);
```

### 2.3 Context Offsets & DCI Indexing Formula
xHCI specification defines:
- **DCI (Device Context Index)**:
  - EP0 (Control bidirectional): $\text{DCI} = 1$
  - Other endpoints:
    - If OUT direction (`(bEndpointAddress & 0x80) == 0`): $\text{DCI} = \text{EP\_Num} \times 2$
    - If IN direction (`(bEndpointAddress & 0x80) != 0`): $\text{DCI} = \text{EP\_Num} \times 2 + 1$
- **Input Context Addressing**:
  - Input Control Context: index 0 -> offset `0 * ctx_size`
  - Slot Context: index 1 -> offset `1 * ctx_size`
  - Endpoint Context for given DCI: index $(1 + \text{DCI})$ -> offset $(1 + \text{DCI}) \times \text{ctx\_size}$
- Helper functions:
  ```c
  static inline void *xhci_input_ep_ctx(void *base, uint8_t csz, uint8_t dci) {
      uint32_t step = csz ? 64 : 32;
      return (uint8_t *)base + (1 + dci) * step;
  }
  static inline void *xhci_dev_ep_ctx(void *base, uint8_t csz, uint8_t dci) {
      uint32_t step = csz ? 64 : 32;
      return (uint8_t *)base + dci * step;
  }
  ```

---

## 3. Detailed Workflows & State Machines

### 3.1 Get Configuration Descriptor Hierarchy
`xhci_get_config_descriptor(ctrl, slot_id, out_config_desc)`:
1. **Stage 1 (Header 9 bytes)**:
   - Prepare Setup Stage TRB:
     - `bmRequestType = 0x80` (IN, Standard, Device)
     - `bRequest = 0x06` (`GET_DESCRIPTOR`)
     - `wValue = (USB_DESC_TYPE_CONFIGURATION << 8) | 0` (Config 0)
     - `wIndex = 0`
     - `wLength = 9`
   - Enqueue Setup, Data (9 bytes to `slot->dma_buf_phys`), Status Stage TRBs.
   - Ring DB (Target = 1), poll Type 32 Transfer Event.
   - Read 9 bytes into `struct usb_config_descriptor temp_cfg = *(struct usb_config_descriptor *)slot->dma_buf_virt;`.
   - Validate `temp_cfg.bLength == 9`, `temp_cfg.bDescriptorType == USB_DESC_TYPE_CONFIGURATION`.
   - Validate `temp_cfg.wTotalLength >= 9 && temp_cfg.wTotalLength <= 4096`.
2. **Stage 2 (Full Hierarchy)**:
   - Issue second control transfer with `wLength = temp_cfg.wTotalLength`.
   - Transfer Data stage reads `temp_cfg.wTotalLength` bytes into `slot->dma_buf_phys`.
   - Poll Type 32 Transfer Event; accept `XHCI_COMP_SUCCESS` and `XHCI_COMP_SHORT_TX`.
   - Calculate actual length: `uint32_t residual = evt.status & 0xFFFFFF; uint32_t actual = (residual <= temp_cfg.wTotalLength) ? (temp_cfg.wTotalLength - residual) : 0;`.
   - Validate `actual >= sizeof(struct usb_config_descriptor)`.
   - Limit parsed length: `uint16_t parse_len = (actual < temp_cfg.wTotalLength) ? (uint16_t)actual : temp_cfg.wTotalLength;`.
   - Store `slot->config_desc = temp_cfg;`.
   - If `out_desc` is non-NULL, `*out_desc = temp_cfg;`.
   - Return `parse_len`.

### 3.2 Descriptor Hierarchy Parsing & Endpoint Discovery
`xhci_parse_config_descriptor(slot, const uint8_t *buffer, uint16_t length)`:
1. Traverse buffer: `uint16_t offset = 0; uint8_t max_dci = 1;`
2. Loop while `offset + 2 <= length`:
   - `uint8_t len = buffer[offset]; uint8_t type = buffer[offset + 1];`
   - If `len < 2 || offset + len > length`: break (corrupt descriptor guard).
   - If `type == USB_DESC_TYPE_INTERFACE`:
     - If `len < sizeof(struct usb_interface_descriptor)`: break;
     - Unpack `const struct usb_interface_descriptor *iface = (const struct usb_interface_descriptor *)(buffer + offset);`
     - Filter primary interface: if `iface->bInterfaceNumber == 0 && iface->bAlternateSetting == 0`:
       - Record `slot->iface.interface_num = iface->bInterfaceNumber;`
       - Record `slot->iface.class_code = iface->bInterfaceClass;`
       - Record `slot->iface.subclass_code = iface->bInterfaceSubClass;`
       - Record `slot->iface.protocol = iface->bInterfaceProtocol;`
       - Record `slot->iface.num_endpoints = iface->bNumEndpoints;`
       - Set `slot->iface.active = true;`
   - If `type == USB_DESC_TYPE_ENDPOINT`:
     - If `len < sizeof(struct usb_endpoint_descriptor)`: break;
     - Unpack `const struct usb_endpoint_descriptor *ep = (const struct usb_endpoint_descriptor *)(buffer + offset);`
     - Extract `ep_num = ep->bEndpointAddress & 0x0F`.
     - Extract `is_in = (ep->bEndpointAddress & 0x80) != 0`.
     - Defensive checks: if `ep_num == 0` or `ep_num > 15`: { `offset += len; continue;` }
     - Compute `dci = ep_num * 2 + (is_in ? 1 : 0)`.
     - If `dci < 2 || dci > 31`: { `offset += len; continue;` }
     - If `dci > max_dci`: `max_dci = dci;`
   - `offset += len;`
3. Return `max_dci`.

### 3.3 Dynamic Endpoint Ring Allocation & Safe Lifecycle
`xhci_free_endpoint_rings(slot)`:
```c
void xhci_free_endpoint_rings(struct xhci_slot *slot)
{
    if (!slot || !slot->ep_rings) return;
    for (uint8_t dci = 2; dci <= slot->max_dci; dci++) {
        if (slot->ep_rings[dci]) {
            if (slot->ep_rings[dci]->ring_phys) {
                free_4k_page(slot->ep_rings[dci]->ring_phys);
                slot->ep_rings[dci]->ring_phys = 0;
            }
            kfree(slot->ep_rings[dci]);
            slot->ep_rings[dci] = NULL;
        }
    }
    kfree(slot->ep_rings);
    slot->ep_rings = NULL;
    slot->max_dci = 0;
}
```

`xhci_setup_endpoint_rings(slot, buffer, length)`:
1. Call `max_dci = xhci_parse_config_descriptor(slot, buffer, length);`.
   If `max_dci < 2` (no non-EP0 endpoints), return 0.
2. Free any existing rings: `xhci_free_endpoint_rings(slot);`.
3. Allocate pointer table:
   ```c
   slot->ep_rings = kmalloc(sizeof(struct xhci_ep_ring *) * (max_dci + 1));
   if (!slot->ep_rings) return -ENOMEM;
   memset(slot->ep_rings, 0, sizeof(struct xhci_ep_ring *) * (max_dci + 1));
   slot->max_dci = max_dci;
   ```
4. Interval calculation helper:
   ```c
   static inline uint8_t xhci_calc_interval(uint8_t speed, uint8_t ep_type, uint8_t bInterval)
   {
       if (bInterval == 0) return 3;
       if (speed == XHCI_PORT_SPEED_HIGH || speed == XHCI_PORT_SPEED_SUPER) {
           return (bInterval <= 16) ? (bInterval - 1) : 15;
       }
       /* Full / Low Speed Interrupt: period = 2^(interval - 3) ms */
       uint8_t interval = 3;
       while ((1U << (interval - 3 + 1)) <= bInterval && interval < 10) {
           interval++;
       }
       return interval; /* bInterval=1..1 -> 3; bInterval=10 -> 6 (8ms) */
   }
   ```
5. Iterate through descriptor buffer to instantiate endpoints:
   - For each Endpoint Descriptor:
     - `ep_num = ep->bEndpointAddress & 0x0F; is_in = (ep->bEndpointAddress & 0x80) != 0;`
     - `dci = ep_num * 2 + (is_in ? 1 : 0);`
     - If `dci < 2 || dci > max_dci || slot->ep_rings[dci] != NULL` continue; (prevent duplicates)
     - Allocate ring struct:
       ```c
       struct xhci_ep_ring *ring = kmalloc(sizeof(struct xhci_ep_ring));
       if (!ring) {
           xhci_free_endpoint_rings(slot);
           return -ENOMEM;
       }
       memset(ring, 0, sizeof(struct xhci_ep_ring));
       ring->ring_phys = alloc_4k_page();
       if (!ring->ring_phys) {
           kfree(ring);
           xhci_free_endpoint_rings(slot);
           return -ENOMEM;
       }
       ring->ring_virt = (struct xhci_trb *)Phy_To_Virt(ring->ring_phys);
       memset(ring->ring_virt, 0, 4096);
       ```
     - Initialize Link TRB at index 255:
       `ring->ring_virt[255].parameter = ring->ring_phys;`
       `ring->ring_virt[255].control = (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_LINK_TC;`
     - `ring->cycle = 1; ring->enqueue = 0; ring->dci = dci; ring->active = true;`
     - Determine EP Type:
       - If attributes `(bmAttributes & 3) == 3` (Interrupt):
         `ring->ep_type = is_in ? XHCI_EP_TYPE_INTR_IN : XHCI_EP_TYPE_INTR_OUT;`
       - If attributes `(bmAttributes & 3) == 2` (Bulk):
         `ring->ep_type = is_in ? XHCI_EP_TYPE_BULK_IN : XHCI_EP_TYPE_BULK_OUT;`
     - `ring->interval = xhci_calc_interval(slot->speed, ring->ep_type, ep->bInterval);`
     - `ring->max_packet_size = ep->wMaxPacketSize & 0x7FF;`
     - Store: `slot->ep_rings[dci] = ring;`
6. Return 0.

### 3.4 Configure Endpoint Command Execution
`xhci_configure_endpoints(ctrl, slot_id)`:
1. Zero `slot->input_ctx_virt`.
2. Populate `Input Control Context`:
   - `drop_flags = 0;`
   - `add_flags = (1U << 0);` (Slot Context)
   - For `dci = 2` to `slot->max_dci`:
     - If `slot->ep_rings && slot->ep_rings[dci] && slot->ep_rings[dci]->active`:
       `add_flags |= (1U << dci);`
   - `uint32_t *ctrl_ctx = (uint32_t *)xhci_input_ctrl_ctx(slot->input_ctx_virt, ctrl->csz);`
   - `ctrl_ctx[0] = drop_flags; ctrl_ctx[1] = add_flags;`
3. Populate `Slot Context`:
   - `uint32_t *slot_ctx = (uint32_t *)xhci_input_slot_ctx(slot->input_ctx_virt, ctrl->csz);`
   - `slot_ctx[0] = (slot->speed << 20) | ((uint32_t)slot->max_dci << 27);` (Context Entries = `max_dci`)
   - `slot_ctx[1] = (slot->port_num << 16);`
4. Populate `Endpoint Context` for each active `dci`:
   - `struct xhci_ep_ring *ring = slot->ep_rings[dci];`
   - `uint32_t *ep_ctx = (uint32_t *)xhci_input_ep_ctx(slot->input_ctx_virt, ctrl->csz, dci);`
   - DW0: `(uint32_t)ring->interval << 16;`
   - DW1: `(3U << 1) | ((uint32_t)ring->ep_type << 3) | ((uint32_t)ring->max_packet_size << 16);`
   - DW2: `(uint32_t)(ring->ring_phys & 0xFFFFFFF0ULL) | 1U;` (DCS = 1)
   - DW3: `(uint32_t)(ring->ring_phys >> 32);`
   - DW4: `(uint32_t)ring->max_packet_size;` (Average TRB Length)
5. Enqueue `Configure Endpoint Command TRB` (Type 12):
   - `trb.parameter = slot->input_ctx_phys;`
   - `trb.status = 0;`
   - `trb.control = ((uint32_t)slot_id << 24) | (XHCI_TRB_TYPE_CONFIG_EP << 10) | ctrl->cmd_ring_cycle;`
6. Ring DB 0, poll Type 33 Command Completion Event using TRB pointer matching. Verify `completion_code == XHCI_COMP_SUCCESS`.
   If error, return `-EIO`.

### 3.5 Set Configuration Command
`xhci_set_configuration(ctrl, slot_id, config_value)`:
1. Issue standard 2-stage control transfer without Data stage:
   - **Setup Stage TRB (Type 2)**:
     - `parameter = ((uint64_t)config_value << 16) | (0x09ULL << 8) | 0x00ULL;`
       (`bmRequestType = 0x00`, `bRequest = 0x09`, `wValue = config_value`, `wIndex = 0`, `wLength = 0`)
     - `status = 8;`
     - `control = (0U << 16) | (1U << 6) | (XHCI_TRB_TYPE_SETUP_STAGE << 10);`
       (`TRT = 0` No Data Stage, `IDT = 1`)
   - **Status Stage TRB (Type 4)**:
     - `parameter = 0; status = 0;`
     - `control = (1U << 16) | (1U << 5) | (XHCI_TRB_TYPE_STATUS_STAGE << 10);`
       (`DIR = 1` IN Status Stage for OUT control transfer, `IOC = 1`)
2. Enqueue the 2 TRBs into `slot->ep0_ring_virt` via `xhci_ep0_ring_enqueue()`.
3. Ring Slot Doorbell (Target = 1, EP0).
4. Poll Type 32 Transfer Event; verify `completion_code == XHCI_COMP_SUCCESS`.
5. Set `slot->configured = true;`.
6. Log:
   `[xhci] slot %u: configured with config %u (iface class %02x:%02x, ep_count=%u)\n`

### 3.6 Overall Port Probing Orchestration in `xhci_probe_ports`
Orchestration sequence per connected port:
```c
/* 1. Port reset & speed */
rc = xhci_port_reset(ctrl, p, &speed);
if (rc != 0) continue;

/* 2. Enable Slot */
rc = xhci_enable_slot(ctrl, p, speed, &slot_id);
if (rc != 0) continue;

/* 3. Address Device */
rc = xhci_address_device(ctrl, slot_id);
if (rc != 0) { xhci_free_slot(ctrl, slot_id); continue; }

/* 4. Get Device Descriptor (Phase 2) */
rc = xhci_get_device_descriptor(ctrl, slot_id, &desc);
if (rc != 0) { xhci_free_slot(ctrl, slot_id); continue; }

/* 5. Get Configuration Descriptor (Phase 3) */
struct usb_config_descriptor cfg;
rc = xhci_get_config_descriptor(ctrl, slot_id, &cfg);
if (rc <= 0) { xhci_free_slot(ctrl, slot_id); continue; }

/* 6. Setup dynamic endpoint rings (Phase 3) */
rc = xhci_setup_endpoint_rings(slot, (const uint8_t *)slot->dma_buf_virt, (uint16_t)rc);
if (rc != 0) { xhci_free_slot(ctrl, slot_id); continue; }

/* 7. Configure Endpoints in xHCI (Phase 3) */
rc = xhci_configure_endpoints(ctrl, slot_id);
if (rc != 0) { xhci_free_slot(ctrl, slot_id); continue; }

/* 8. Set Configuration on device (Phase 3) */
rc = xhci_set_configuration(ctrl, slot_id, cfg.bConfigurationValue);
if (rc != 0) { xhci_free_slot(ctrl, slot_id); continue; }
```

---

## 4. Testing & Verification Plan

### 4.1 Host Unit Tests (`hosttests/cases/test_xhci_phase3.c`)
1. **`test_xhci_parse_config_descriptor_hierarchy`**:
   - Construct raw byte stream containing Configuration Descriptor (len=9, total=34), Interface Descriptor (HID Tablet class 3, iface 0, ep_count=1), HID Descriptor (len=9), Endpoint Descriptor (Interrupt IN EP 1 -> DCI 3, MPS=8, bInterval=10).
   - Verify parsing extracts `wTotalLength=34`, `bConfigurationValue=1`, `max_dci=3`, `class=3`.
2. **`test_xhci_parse_corrupt_descriptor_guards`**:
   - Test sub-descriptor with `len < sizeof(descriptor)`.
   - Test `ep_num == 0` (corrupt endpoint address `0x80`).
   - Test duplicate endpoint descriptor in same hierarchy.
   - Test `wTotalLength > 4096` rejection.
3. **`test_xhci_dynamic_ep_rings_lifecycle_and_oom_rollback`**:
   - Verify dynamic allocation of `slot->ep_rings` pointer table and individual `struct xhci_ep_ring`.
   - Verify Link TRB setup at TRB 255.
   - Inject `alloc_4k_page` failure during endpoint allocation: verify `xhci_free_endpoint_rings()` cleans up all allocated pages and structures without leaks (`s_mock_page_count` returns to baseline).
4. **`test_xhci_configure_endpoint_input_context`**:
   - Verify `Input Control Context`: `Drop=0`, `Add=(1<<0) | (1<<3)`.
   - Verify `Slot Context`: `Context Entries = 3`.
   - Verify `Endpoint Context (DCI=3)`: `EP Type = 7 (Interrupt IN)`, `Interval = 6` (for `bInterval=10`), `Max Packet Size = 8`, `DCS = 1`.
   - Feed mock Command Completion Event (Type 33) and verify success.
5. **`test_xhci_set_configuration_2_stage_transfer`**:
   - Verify Setup Stage TRB: `TRT = 0` (No Data Stage), `IDT = 1`, `wValue = 1`.
   - Verify Status Stage TRB: `DIR = 1` (IN), `IOC = 1`.
   - Feed mock Transfer Event (Type 32, Success) and verify `slot->configured == true`.

### 4.2 QEMU E2E Integration
- Boot QEMU with `-device usb-tablet,bus=xhci.0`.
- Verify serial log output:
  ```
  [xhci] port 1: connected, reset complete, speed=3
  [xhci] slot 1 allocated (port 1)
  [xhci] slot 1 addressed successfully
  [xhci] slot 1: USB device 0x0627:0x0001 (class 00:00, ep0_max=64)
  [xhci] slot 1: config 1 total_len=34 interfaces=1
  [xhci] slot 1: endpoint DCI 3 configured (type=7, mps=8, interval=6)
  [xhci] slot 1: configured with config 1 (iface class 03:00, ep_count=1)
  ```
- Run standard suites: `make test-host`, `make test-static`.
