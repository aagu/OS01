# xHCI Phase 2: Port Reset, Slot Allocation, EP0 Control Transfer & Descriptor Enumeration

**Author**: Antigravity  
**Date**: 2026-11-04  
**Target Milestone**: xHCI Phase 2 (Device Enumeration & Control Transfer)  
**Status**: Revised Specification (Incorporating Review Feedback)  

---

## 1. Executive Summary & Goals

Phase 1 established the xHCI host controller skeleton, MMIO accessors, 5-step reset handshake, DCBAA, Command Ring (with Link TRB wrap), Event Ring (ERST), Interrupter 0, and Run/Stop state machine.

Phase 2 advances the driver from an idle host controller to active device discovery and communication on attached root hub ports:
1. **Port Reset & Speed Detection**: Iterate over root ports (`1..max_ports`), detect connection state (`PORTSC.CCS`), issue port reset (`PORTSC.PR`), wait for reset completion (`PORTSC.PRC` and `PORTSC.PED`), enforce USB 2.0 reset recovery time ($T_{RSTRCY} \ge 10\text{ms}$), and read port operating speed (`PORTSC.PortSpeed`).
2. **Command/Event Polling Subsystem with Event Filtering**: Implement a robust synchronous command dispatch and event polling engine (`xhci_poll_event_filtered`) with timeout handling, ERDP advancing with EHB toggle, and automatic consumption of asynchronous Port Status Change Events (TRB Type 34) arriving during command/transfer waits.
3. **Slot Allocation**: Issue `Enable Slot Command` (TRB Type 9), await `Command Completion Event` (TRB Type 33), and extract the allocated `slot_id` (1-based, bound-checked against `1 <= slot_id <= XHCI_MAX_MANAGED_SLOTS`).
4. **Context Allocation & Address Device**:
   - Dynamic 32-byte vs 64-byte context sizes based on `HCCPARAMS1.CSZ` (bit 2).
   - Allocate and initialize Input Context (Input Control Context, Slot Context, Endpoint 0 Context) using standardized DW-level bitfield offsets.
   - Configure EP0 context with appropriate Max Packet Size for detected speed (LS=8, FS=64, HS=64, SS=512) and EP0 Transfer Ring base address (DCS=1).
   - Allocate Output Device Context and store its physical address into `DCBAA[slot_id]`.
   - Issue `Address Device Command` (TRB Type 11, BSR=0) and await completion.
5. **EP0 Transfer Ring & GET_DESCRIPTOR Control Transfer**:
   - Allocate EP0 Transfer Ring (4KB page with Link TRB wrap).
   - Allocate dedicated 4KB DMA page per slot for control transfer payloads.
   - Construct standard USB control transfer (Setup Stage TRB -> Data Stage TRB -> Status Stage TRB) requesting 18-byte standard USB Device Descriptor (`bRequest = 0x06`, `wValue = 0x0100`).
   - Ring Slot Doorbell (Target = 1 for Endpoint 0), await `Transfer Event` (TRB Type 32) on Event Ring, and verify completion code (`SUCCESS = 1` or `SHORT_TX = 13`).
   - Calculate actual transfer length using residual length (`actual = requested - (status & 0xFFFFFF)`).
   - Parse and log device properties (`idVendor`, `idProduct`, `bDeviceClass`, etc.).
6. **QEMU Integration & Host Testing**:
   - Add a test USB device (`-device usb-tablet,bus=xhci.0`) to QEMU invocation.
   - Provide comprehensive host-level mock unit tests covering port reset, RW1C safety, intervening Type 34 events, context layout (32-byte & 64-byte), command submissions, and descriptor parsing.

---

## 2. Architecture & Data Structures

### 2.1 xHCI Register Bitfields & Offsets
New register offsets and constants added to `kernel/include/driver/xhci.h`:

- **Port Register Set**:
  - Base offset: `ctrl->op_regs + 0x400 + (port_index - 1) * 0x10`
  - Bitfields in `PORTSC`:
    - `XHCI_PORTSC_CCS`       = `(1U << 0)`   (Current Connect Status, RO)
    - `XHCI_PORTSC_PED`       = `(1U << 1)`   (Port Enabled/Disabled, RW1C)
    - `XHCI_PORTSC_OCA`       = `(1U << 3)`   (Over-current Active, RO)
    - `XHCI_PORTSC_PR`        = `(1U << 4)`   (Port Reset, RW)
    - `XHCI_PORTSC_PLS_MASK`  = `(0xFU << 5)` (Port Link State)
    - `XHCI_PORTSC_PP`        = `(1U << 9)`   (Port Power, RW)
    - `XHCI_PORTSC_SPEED_MASK`= `(0xFU << 10)`(Port Speed, RO)
    - `XHCI_PORTSC_CSC`       = `(1U << 17)`  (Connect Status Change, RW1C)
    - `XHCI_PORTSC_PEC`       = `(1U << 18)`  (Port Enable/Disable Change, RW1C)
    - `XHCI_PORTSC_WRC`       = `(1U << 19)`  (Warm Port Reset Change, RW1C)
    - `XHCI_PORTSC_OCC`       = `(1U << 20)`  (Over-current Change, RW1C)
    - `XHCI_PORTSC_PRC`       = `(1U << 21)`  (Port Reset Change, RW1C)
    - `XHCI_PORTSC_PLC`       = `(1U << 22)`  (Port Link State Change, RW1C)
    - `XHCI_PORTSC_CEC`       = `(1U << 23)`  (Config Error Change, RW1C)
    - `XHCI_PORTSC_WPR`       = `(1U << 31)`  (Warm Port Reset, RW)
  - **RW1C Preservation Mask**:
    - `XHCI_PORTSC_RW1C_MASK = (XHCI_PORTSC_PED | XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | XHCI_PORTSC_WRC | XHCI_PORTSC_OCC | XHCI_PORTSC_PRC | XHCI_PORTSC_PLC | XHCI_PORTSC_CEC)`
    - *Critical Invariant*: When writing to `PORTSC` to modify control bits (e.g., `PP` or `PR`), always mask off `XHCI_PORTSC_RW1C_MASK` unless deliberately clearing a change flag.

- **Port Speed Values** (bits [13:10]):
  - `XHCI_PORT_SPEED_FULL`  = 1 (12 Mb/s)
  - `XHCI_PORT_SPEED_LOW`   = 2 (1.5 Mb/s)
  - `XHCI_PORT_SPEED_HIGH`  = 3 (480 Mb/s)
  - `XHCI_PORT_SPEED_SUPER` = 4 (5 Gb/s)

- **TRB Types (Extended)**:
  - `XHCI_TRB_TYPE_NORMAL`         = 1
  - `XHCI_TRB_TYPE_SETUP_STAGE`    = 2
  - `XHCI_TRB_TYPE_DATA_STAGE`     = 3
  - `XHCI_TRB_TYPE_STATUS_STAGE`   = 4
  - `XHCI_TRB_TYPE_LINK`           = 6
  - `XHCI_TRB_TYPE_ENABLE_SLOT`    = 9
  - `XHCI_TRB_TYPE_DISABLE_SLOT`   = 10
  - `XHCI_TRB_TYPE_ADDRESS_DEV`    = 11
  - `XHCI_TRB_TYPE_TRANSFER_EVT`   = 32
  - `XHCI_TRB_TYPE_CMD_CMPL_EVT`   = 33
  - `XHCI_TRB_TYPE_PORT_STATUS_EVT`= 34

- **Completion Codes** (bits [31:24] of TRB status field):
  - `XHCI_COMP_INVALID`     = 0
  - `XHCI_COMP_SUCCESS`     = 1
  - `XHCI_COMP_DATA_BUFFER` = 2
  - `XHCI_COMP_BABBLE`      = 3
  - `XHCI_COMP_TX_ERR`      = 4
  - `XHCI_COMP_TRB_ERR`     = 5
  - `XHCI_COMP_STALL`       = 6
  - `XHCI_COMP_SHORT_TX`    = 13

### 2.2 Device & Input Context Layouts (32-byte vs 64-byte)
xHCI section 6.2 specifies context structures:
- `ctrl->csz` parsed from `HCCPARAMS1` (offset 0x10, bit 2):
  - `csz = 0`: 32 bytes per context entry (`ctx_size = 32`).
  - `csz = 1`: 64 bytes per context entry (`ctx_size = 64`).

- **Input Context Structure**:
  - Contains `Input Control Context` (index 0) + `Slot Context` (index 1) + 31 `Endpoint Contexts` (EP0 is index 2, DCI = 1).
  - Offsets:
    - Input Control: `0 * ctx_size`
    - Slot Context: `1 * ctx_size`
    - EP0 Context: `2 * ctx_size`
  - Inline helpers:
    ```c
    static inline void *xhci_input_ctrl_ctx(void *base, uint8_t csz) {
        return (uint8_t *)base;
    }
    static inline void *xhci_input_slot_ctx(void *base, uint8_t csz) {
        return (uint8_t *)base + (csz ? 64 : 32);
    }
    static inline void *xhci_input_ep0_ctx(void *base, uint8_t csz) {
        return (uint8_t *)base + (csz ? 128 : 64);
    }
    ```

- **Device Context Structure (pointed to by `DCBAA[slot_id]`)**:
  - Slot Context (output): `0 * ctx_size`
  - EP0 Context (output): `1 * ctx_size`
  - Inline helpers:
    ```c
    static inline void *xhci_dev_slot_ctx(void *base, uint8_t csz) {
        return (uint8_t *)base;
    }
    static inline void *xhci_dev_ep0_ctx(void *base, uint8_t csz) {
        return (uint8_t *)base + (csz ? 64 : 32);
    }
    ```

- **Standard Field Offsets (32-bit DWords)**:
  - **Input Control Context**:
    - DW0 (`drop_flags`): `0`
    - DW1 (`add_flags`): `(1U << 0) | (1U << 1)` (Slot Context + EP0 Context)
  - **Slot Context**:
    - DW0: `(speed << 20) | (1U << 27)` (Route String = 0, Speed, Context Entries = 1)
    - DW1: `(port_num << 16)` (Root Hub Port Number)
    - DW2: `0`
    - DW3: `0` (Hardware writes device address upon Address Device)
  - **Endpoint 0 Context**:
    - DW0: `0` (EP State = 0)
    - DW1: `(3U << 1) | (4U << 3) | ((uint32_t)max_packet_size << 16)` (CErr = 3, EP Type = 4 [Control], Max Packet Size)
    - DW2: `(uint32_t)(slot->ep0_ring_phys & 0xFFFFFFF0ULL) | 1U` (TR Dequeue Pointer Lo with DCS = 1)
    - DW3: `(uint32_t)(slot->ep0_ring_phys >> 32)` (TR Dequeue Pointer Hi)
    - DW4: `8U` (Average TRB Length)

### 2.3 Per-Slot In-Memory Representation
Because `slot_id` is 1-based (from 1 up to `max_slots`), the managed slot array in `struct xhci_controller` is sized to include index 0:
```c
#define XHCI_MAX_MANAGED_SLOTS 32

struct xhci_slot {
    uint8_t slot_id;
    uint8_t port_num;
    uint8_t speed;
    uint16_t max_packet_size;
    bool active;

    /* DMA Allocations (each 4KB page) */
    uint64_t input_ctx_phys;
    void *input_ctx_virt;

    uint64_t dev_ctx_phys;
    void *dev_ctx_virt;

    /* EP0 Transfer Ring (single 4KB page) */
    uint64_t ep0_ring_phys;
    struct xhci_trb *ep0_ring_virt;
    uint32_t ep0_ring_enqueue;
    uint8_t ep0_ring_cycle;

    /* Dedicated 4KB DMA Data Buffer for Control Transfers */
    uint64_t dma_buf_phys;
    void *dma_buf_virt;

    /* Cached Device Descriptor */
    struct usb_device_descriptor desc;
};
```
In `struct xhci_controller`:
```c
struct xhci_slot *slots[XHCI_MAX_MANAGED_SLOTS + 1];
uint8_t csz; /* 0 for 32-byte, 1 for 64-byte */
```

### 2.4 USB Device Descriptor Layout
Standard USB 2.0/3.0 18-byte Device Descriptor:
```c
struct usb_device_descriptor {
    uint8_t  bLength;            /* 18 */
    uint8_t  bDescriptorType;    /* 1 = DEVICE */
    uint16_t bcdUSB;             /* USB Spec release (e.g. 0x0200, 0x0300) */
    uint8_t  bDeviceClass;       /* Class code */
    uint8_t  bDeviceSubClass;    /* Subclass code */
    uint8_t  bDeviceProtocol;    /* Protocol code */
    uint8_t  bMaxPacketSize0;    /* Max packet size for EP0 */
    uint16_t idVendor;           /* Vendor ID */
    uint16_t idProduct;          /* Product ID */
    uint16_t bcdDevice;          /* Device release number */
    uint8_t  iManufacturer;      /* Index of string descriptor */
    uint8_t  iProduct;           /* Index of string descriptor */
    uint8_t  iSerialNumber;      /* Index of string descriptor */
    uint8_t  bNumConfigurations; /* Number of possible configurations */
} __attribute__((packed));
```

---

## 3. Detailed Workflows & State Machines

### 3.1 Event Ring Polling with Event Filtering
`xhci_poll_event_filtered(ctrl, target_type, out_evt, timeout_loops)`:
1. Loop up to `timeout_loops`:
   - Access current dequeue TRB using volatile access to prevent compiler hoisting:
     ```c
     volatile struct xhci_trb *trb = &ctrl->event_ring_virt[ctrl->event_ring_dequeue];
     uint32_t ctrl_word = *(volatile uint32_t *)&trb->control;
     ```
   - Check if `(ctrl_word & XHCI_TRB_CYCLE_BIT) == ctrl->event_ring_cycle`:
     - If not set, wait/spin and continue.
   - TRB is valid. Read full TRB into local copy `struct xhci_trb evt = *trb;`.
   - Advance dequeue pointer and toggle cycle bit if wrapping:
     ```c
     ctrl->event_ring_dequeue = (ctrl->event_ring_dequeue + 1) % XHCI_TRB_RING_SIZE;
     if (ctrl->event_ring_dequeue == 0) {
         ctrl->event_ring_cycle ^= 1;
     }
     ```
   - Advance ERDP register with `XHCI_ERDP_EHB` set to clear interrupt pending state:
     ```c
     uint64_t erdp = ctrl->event_ring_phys + ctrl->event_ring_dequeue * sizeof(struct xhci_trb);
     xhci_write64(ctrl->rt_regs + 0x20 + XHCI_INTR_ERDP, erdp | XHCI_ERDP_EHB);
     ```
   - Check TRB Type `uint32_t evt_type = (evt.control >> 10) & 0x3F;`:
     - If `evt_type == target_type` (or `target_type == 0` for any):
       - Copy `evt` to `*out_evt` and return 0.
     - If `evt_type == XHCI_TRB_TYPE_PORT_STATUS_EVT` (Type 34):
       - Log debug notification: `[xhci] consumed asynchronous Port Status Change Event (port %u)\n`.
       - Continue looping to wait for the expected target event.
     - Else:
       - Log warning for unexpected event type, continue looping.
2. Return `-ETIMEDOUT` if timeout loops exhausted without receiving `target_type`.

### 3.2 Port Reset & Speed Detection Sequence
For port index `p` from 1 to `ctrl->max_ports`:
1. Calculate `port_reg = ctrl->op_regs + 0x400 + (p - 1) * 0x10;`.
2. Read `PORTSC`: `uint32_t portsc = xhci_read32(port_reg);`.
3. If `!(portsc & XHCI_PORTSC_CCS)`, no device connected; continue to next port.
4. Port Power Check:
   - If `!(portsc & XHCI_PORTSC_PP)`, power on the port:
     ```c
     uint32_t val = (portsc & ~XHCI_PORTSC_RW1C_MASK) | XHCI_PORTSC_PP;
     xhci_write32(port_reg, val);
     // Wait 20ms for port power stabilization
     xhci_delay_ms(20);
     portsc = xhci_read32(port_reg);
     ```
5. Initiate Port Reset:
   - Protect RW1C flags and write `PR = 1`:
     ```c
     uint32_t val = (portsc & ~XHCI_PORTSC_RW1C_MASK) | XHCI_PORTSC_PR;
     xhci_write32(port_reg, val);
     ```
6. Wait for reset completion:
   - Poll `PORTSC` until `(portsc & XHCI_PORTSC_PR) == 0` and `((portsc & XHCI_PORTSC_PRC) || (portsc & XHCI_PORTSC_PED))`.
   - Clear change flags:
     ```c
     xhci_write32(port_reg, (portsc & ~XHCI_PORTSC_RW1C_MASK) | XHCI_PORTSC_PRC);
     ```
   - **Strict PED Verification**:
     ```c
     portsc = xhci_read32(port_reg);
     if (!(portsc & XHCI_PORTSC_PED)) {
         log_warn("[xhci] port %u: reset completed but port not enabled (PED=0)\n", p);
         continue;
     }
     ```
7. **USB Reset Recovery Delay ($T_{RSTRCY}$)**:
   - Wait at least 10ms for device microcontroller stabilization:
     `xhci_delay_ms(10);`
8. Read `port_speed = (portsc & XHCI_PORTSC_SPEED_MASK) >> 10`.
9. Log port discovery: `[xhci] port %u: connected, reset complete, speed=%u\n`, `p`, `port_speed`.

### 3.3 Slot Allocation Workflow
1. Prepare `Enable Slot Command TRB` (Type 9):
   - `trb.parameter = 0;`
   - `trb.status = 0;`
   - `trb.control = (XHCI_TRB_TYPE_ENABLE_SLOT << 10) | ctrl->cmd_ring_cycle;`
2. Submit via `xhci_cmd_ring_enqueue(ctrl, &trb);`
3. Ring Host Controller Doorbell 0:
   `xhci_write32(ctrl->db_regs, 0);`
4. Poll Event Ring for `Command Completion Event` (TRB Type 33):
   - `rc = xhci_poll_event_filtered(ctrl, XHCI_TRB_TYPE_CMD_CMPL_EVT, &evt, XHCI_TIMEOUT_LOOPS);`
   - Verify `completion_code == XHCI_COMP_SUCCESS`.
   - Extract `slot_id = (evt.control >> 24) & 0xFF`.
   - Validate range: `if (slot_id < 1 || slot_id > XHCI_MAX_MANAGED_SLOTS) return -ERANGE;`
5. Allocate `struct xhci_slot *slot = kmalloc(sizeof(struct xhci_slot));`.
   - Initialize `slot->slot_id = slot_id; slot->port_num = p; slot->speed = port_speed;`
   - Save to `ctrl->slots[slot_id] = slot;`

### 3.4 Address Device Workflow
1. Allocate DMA pages for the slot:
   - `slot->input_ctx_virt = alloc_4k_page(); slot->input_ctx_phys = Virt_To_Phy(slot->input_ctx_virt);`
   - `slot->dev_ctx_virt = alloc_4k_page(); slot->dev_ctx_phys = Virt_To_Phy(slot->dev_ctx_virt);`
   - `slot->ep0_ring_virt = alloc_4k_page(); slot->ep0_ring_phys = Virt_To_Phy(slot->ep0_ring_virt);`
   - `slot->dma_buf_virt = alloc_4k_page(); slot->dma_buf_phys = Virt_To_Phy(slot->dma_buf_virt);`
   - Zero all allocated pages.
2. Register `slot->dev_ctx_phys` into `ctrl->dcbaa_virt[slot_id]`.
3. Initialize EP0 Transfer Ring (256 TRBs):
   - Setup Link TRB at index 255:
     ```c
     struct xhci_trb *link = &slot->ep0_ring_virt[XHCI_TRB_RING_SIZE - 1];
     link->parameter = slot->ep0_ring_phys;
     link->status = 0;
     link->control = (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_LINK_TC;
     slot->ep0_ring_cycle = 1;
     slot->ep0_ring_enqueue = 0;
     ```
4. Determine `max_packet_size` from `port_speed`:
   - `XHCI_PORT_SPEED_LOW`: 8
   - `XHCI_PORT_SPEED_FULL`: 64 (or 8 initially, 64 is safe standard)
   - `XHCI_PORT_SPEED_HIGH`: 64
   - `XHCI_PORT_SPEED_SUPER`: 512
   - `slot->max_packet_size = max_packet_size;`
5. Populate Input Control Context:
   - `uint32_t *ctrl_ctx = (uint32_t *)xhci_input_ctrl_ctx(slot->input_ctx_virt, ctrl->csz);`
   - `ctrl_ctx[0] = 0;` (Drop Flags)
   - `ctrl_ctx[1] = (1U << 0) | (1U << 1);` (Add Flags: Slot + EP0)
6. Populate Slot Context:
   - `uint32_t *slot_ctx = (uint32_t *)xhci_input_slot_ctx(slot->input_ctx_virt, ctrl->csz);`
   - `slot_ctx[0] = (slot->speed << 20) | (1U << 27);` (Context Entries = 1)
   - `slot_ctx[1] = (slot->port_num << 16);` (Root Hub Port Num)
7. Populate Endpoint 0 Context:
   - `uint32_t *ep0_ctx = (uint32_t *)xhci_input_ep0_ctx(slot->input_ctx_virt, ctrl->csz);`
   - `ep0_ctx[0] = 0;`
   - `ep0_ctx[1] = (3U << 1) | (4U << 3) | ((uint32_t)max_packet_size << 16);` (CErr=3, EP Type=4, MPS)
   - `ep0_ctx[2] = (uint32_t)(slot->ep0_ring_phys & 0xFFFFFFF0ULL) | 1U;` (DCS=1)
   - `ep0_ctx[3] = (uint32_t)(slot->ep0_ring_phys >> 32);`
   - `ep0_ctx[4] = 8U;` (Average TRB Length)
8. Enqueue `Address Device Command TRB` (Type 11):
   - `trb.parameter = slot->input_ctx_phys;`
   - `trb.status = 0;`
   - `trb.control = (slot_id << 24) | (XHCI_TRB_TYPE_ADDRESS_DEV << 10) | ctrl->cmd_ring_cycle;`
9. Submit to Command Ring, ring Doorbell 0, poll for Command Completion Event (Type 33).
   - If error, teardown slot and return error code.

### 3.5 EP0 Control Transfer (GET_DESCRIPTOR)
Control transfer setup:
1. **Setup Stage TRB (Type 2)**:
   - 8-byte USB Request:
     - `bmRequestType` = 0x80 (IN, Standard, Device)
     - `bRequest` = 0x06 (GET_DESCRIPTOR)
     - `wValue` = 0x0100 (Type 1: DEVICE, Index 0)
     - `wIndex` = 0x0000
     - `wLength` = 18
   - Immediate value packed in `parameter`: `0x0012000001000680ULL`
   - `status`: Transfer Length = 8
   - `control`: `(3U << 16) | (1U << 6) | (XHCI_TRB_TYPE_SETUP_STAGE << 10) | slot->ep0_ring_cycle`
     - `TRT = 3` (IN Data Stage)
     - `IDT = 1` (Immediate Data)
2. **Data Stage TRB (Type 3)**:
   - `parameter`: `slot->dma_buf_phys`
   - `status`: Transfer Length = 18
   - `control`: `(1U << 16) | (XHCI_TRB_TYPE_DATA_STAGE << 10) | slot->ep0_ring_cycle`
     - `DIR = 1` (IN)
3. **Status Stage TRB (Type 4)**:
   - `parameter`: 0
   - `status`: 0
   - `control`: `(1U << 5) | (XHCI_TRB_TYPE_STATUS_STAGE << 10) | slot->ep0_ring_cycle`
     - `DIR = 0` (OUT Handshake for IN Transfer)
     - `IOC = 1` (Interrupt On Completion)
4. Enqueue the 3 TRBs into `slot->ep0_ring_virt` using `xhci_ep0_ring_enqueue`.
5. Ring Slot Doorbell:
   - `xhci_write32(ctrl->db_regs + slot_id * 4, 1);` (Target = 1, EP0)
6. Poll Event Ring for `Transfer Event` (Type 32):
   - `rc = xhci_poll_event_filtered(ctrl, XHCI_TRB_TYPE_TRANSFER_EVT, &evt, XHCI_TIMEOUT_LOOPS);`
   - Check `completion_code == XHCI_COMP_SUCCESS || completion_code == XHCI_COMP_SHORT_TX`.
   - Calculate actual length: `uint32_t residual = evt.status & 0xFFFFFF; uint32_t actual = (residual <= 18) ? (18 - residual) : 0;`.
   - Verify `actual >= 8` (enough to read basic descriptor info, 18 for full descriptor).
7. Copy from `slot->dma_buf_virt` to `slot->desc`:
   - Parse `idVendor`, `idProduct`, `bDeviceClass`, `bDeviceSubClass`, `bMaxPacketSize0`.
   - Log discovered USB device:
     ```
     [xhci] slot %u: USB device %04x:%04x (class %02x, subclass %02x, ep0_max=%u)
     ```

### 3.6 Resource Teardown & Slot Freeing
`xhci_free_slot(ctrl, slot_id)`:
1. If slot is registered in DCBAA, clear `ctrl->dcbaa_virt[slot_id] = 0;`.
2. Free 4KB DMA pages:
   - `free_4k_page(slot->input_ctx_virt);`
   - `free_4k_page(slot->dev_ctx_virt);`
   - `free_4k_page(slot->ep0_ring_virt);`
   - `free_4k_page(slot->dma_buf_virt);`
3. Free host struct: `kfree(slot);`
4. Clear `ctrl->slots[slot_id] = NULL;`

---

## 4. Testing & Verification Plan

### 4.1 Host Unit Tests (`hosttests/cases/test_xhci_phase2.c`)
1. **`test_xhci_port_reset_and_ped_check`**:
   - Mock PORTSC register with `CCS=1`.
   - Write `PR=1` -> Mock clears `PR=0` and sets `PRC=1, PED=1`.
   - Verify RW1C flags are never mistakenly clobbered.
   - Verify failure handling when `PR=0` but `PED=0`.
2. **`test_xhci_context_layout_32_vs_64`**:
   - Verify byte offsets and field packing for `csz=0` (32 bytes) vs `csz=1` (64 bytes).
3. **`test_xhci_event_poll_with_intervening_port_change`**:
   - Insert `Type 34 (Port Status Change Event)` at dequeue index 0.
   - Insert `Type 33 (Command Completion Event, Success)` at dequeue index 1.
   - Call `xhci_poll_event_filtered(..., target=Type 33)`.
   - Verify Type 34 is consumed, ERDP is updated twice, and Type 33 is successfully returned.
4. **`test_xhci_enable_slot`**:
   - Submit Enable Slot command, feed completion event with `slot_id = 1`, verify slot struct creation.
5. **`test_xhci_address_device`**:
   - Validate Input Control flags (`Drop=0, Add=3`), Slot Context speed/port mapping, EP0 Context DCS=1 and MPS.
6. **`test_xhci_ep0_control_transfer`**:
   - Enqueue Setup, Data, Status TRBs.
   - Verify immediate data layout for `GET_DESCRIPTOR`, data buffer pointer, IOC on status stage.
   - Feed Transfer Event (Type 32, success, residual=0).
   - Verify 18-byte USB descriptor unpacking.

### 4.2 QEMU E2E Integration
- Update `mk/components/run.mk` to add `-device usb-tablet,bus=xhci.0` to `RUN_QEMU_DISK`.
- Verify boot log captures:
  ```
  [xhci] port 1: connected, reset complete, speed=3
  [xhci] slot 1 allocated (port 1)
  [xhci] slot 1 addressed successfully
  [xhci] slot 1: USB device 0x0627:0x0001 (class 0x00, subclass 0x00, ep0_max=64)
  ```
- Run verification suites:
  - `make test-host`
  - `make test-static`
  - `make run`
