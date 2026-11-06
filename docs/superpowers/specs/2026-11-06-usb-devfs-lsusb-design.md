# xHCI Phase 4: USB Device Filesystem (devfs) Publication & User-space lsusb Utility

**Author**: Antigravity  
**Date**: 2026-11-06  
**Target Milestone**: Phase 4 (USB devfs Publication & User-space `lsusb` Utility — End-to-End Closure)  
**Status**: Revised Specification (Incorporating Review Feedback)  

---

## 1. Executive Summary & Goals

Phases 1 through 3 brought xHCI up from bare MMIO and ring initialisation to full USB device configuration:
- Phase 1: Controller skeleton, 5-step reset handshake, DMA rings, Run state.
- Phase 2: Root port reset (with RW1C mask protection, PED validation, $T_{RSTRCY}$ delay), slot allocation, addressing, EP0 Transfer Ring, and 18-byte Device Descriptor retrieval.
- Phase 3: Dynamic endpoint rings (`struct xhci_ep_ring **ep_rings`), 2-stage configuration descriptor retrieval, malformed descriptor defense, `Configure Endpoint` command (TRB Type 12) with accurate Interval calculation, and 2-stage zero-length `SET_CONFIGURATION` control transfer.

Phase 4 merges the kernel devfs exposure and the user-space `lsusb` utility into a unified, end-to-end increment:
1. **Raw Descriptors Caching**:
   - Cache the full raw binary descriptor stream in `struct xhci_slot`: 18 bytes of Device Descriptor concatenated with `wTotalLength` bytes of Configuration Descriptor hierarchy (Configuration, Interface, HID, Endpoint descriptors).
   - Clean up dynamically on slot teardown (`xhci_free_slot`).
2. **Kernel `/dev/usb/` Filesystem Namespace (`usb_devfs`)**:
   - Following the proven pattern of `kernel/bus/pci/pci_devfs.c` (ARCH-11/12), publish each configured USB device as `/dev/usb/<bus>-<port>/<attr>` using `devfs_register_path()`.
   - Provide standard text attribute nodes:
     - `bus`: `%u\n` (e.g. `1\n`)
     - `devnum`: `%u\n` (e.g. `1\n`)
     - `port`: `%u\n` (e.g. `1\n`)
     - `vendor`: `0x%04x\n` (e.g. `0x0627\n`)
     - `product`: `0x%04x\n` (e.g. `0x0001\n`)
     - `class`: `0x%02x\n` (e.g. `0x00\n`)
     - `subclass`: `0x%02x\n` (e.g. `0x00\n`)
     - `protocol`: `0x%02x\n` (e.g. `0x00\n`)
     - `speed`: `full\n`, `low\n`, `high\n`, or `super\n`
   - Provide binary attribute node:
     - `descriptors`: Complete binary stream of Device Descriptor + Configuration Hierarchy (identical to Linux `/sys/bus/usb/devices/<bus>-<port>/descriptors`).
   - Slicing and offset support in the read callback, returning 0 at or beyond EOF.
3. **User-space `/bin/lsusb` Utility (`user/lsusb.c`)**:
   - Enumerate `/dev/usb/` using standard POSIX `opendir()` / `readdir()`.
   - Collect and sort entries using `qsort()` by `bus` ascending, then `port`/`devnum` ascending for deterministic output.
   - Standard output mode (aligned with Linux `lsusb`):
     ```text
     Bus 001 Device 001: ID 0627:0001 QEMU USB Tablet
     ```
     With built-in device name lookup table (QEMU Tablet, Mouse, Keyboard, Hubs, etc.) and fallback to class description.
   - Verbose mode (`-v`):
     - Open `/dev/usb/<dev>/descriptors` binary node.
     - Unpack and formatted dump of Device Descriptor (VID, PID, USB release, MaxPacketSize0, etc.).
     - Unpack and formatted dump of Configuration Descriptor, Interface Descriptors (Interface class name, protocol), and Endpoint Descriptors (Address, Direction, Transfer Type, MaxPacketSize, Interval).
   - Options `-s [[bus]:][devnum]` to filter by bus and device number, and `-h` for usage.
   - Standard exit codes: 0 = at least one device listed/matched, 1 = no devices found / none matched, 2 = usage error.
4. **Testing & Integration**:
   - Host unit tests (`hosttests/cases/test_usb_devfs.c`): Validate attribute formatting, buffer slicing, offset handling, binary descriptors dumping, and `lsusb` directory parsing.
   - Packaging: Automatically compiled via `user/apps.mk` and staged into `/bin/lsusb` on `disk.img`.
   - QEMU E2E test: Boot into OS01 shell, execute `lsusb` and `lsusb -v`, asserting expected device output for QEMU USB Tablet.

---

## 2. Architecture & Data Structures

### 2.1 Raw Descriptors Caching in `struct xhci_slot`
In `kernel/include/driver/xhci.h`:
```c
struct xhci_slot {
    /* ... existing Phase 1-3 fields ... */

    /* Cached Raw Binary Descriptors (Device Descriptor + Config Hierarchy) */
    uint8_t *raw_descriptors;
    uint16_t raw_descriptors_len;
};
```
During probe orchestration in `xhci_probe_ports()`:
- After `xhci_get_config_descriptor()` returns `parse_len`:
  ```c
  uint16_t total_raw = sizeof(struct usb_device_descriptor) + parse_len;
  slot->raw_descriptors = kmalloc(total_raw);
  if (slot->raw_descriptors) {
      memcpy(slot->raw_descriptors, &slot->desc, sizeof(struct usb_device_descriptor));
      memcpy(slot->raw_descriptors + sizeof(struct usb_device_descriptor),
             slot->dma_buf_virt, parse_len);
      slot->raw_descriptors_len = total_raw;
  }
  ```
- In `xhci_free_slot()`:
  ```c
  if (slot->raw_descriptors) {
      kfree(slot->raw_descriptors);
      slot->raw_descriptors = NULL;
      slot->raw_descriptors_len = 0;
  }
  ```

### 2.2 `usb_devfs` Namespace and Attribute IDs
New header: `kernel/include/driver/usb_devfs.h`:
```c
#ifndef _DRIVER_USB_DEVFS_H
#define _DRIVER_USB_DEVFS_H

#include <stdint.h>
#include <stddef.h>
#include <driver/xhci.h>

#define USB_DEVFS_NAMESPACE   "usb"
#define USB_DEVFS_ID_MAX      32    /* "1-1" + NUL */
#define USB_DEVFS_PATH_MAX    64    /* "usb/1-1/descriptors" + NUL */
#define USB_DEVFS_FMT_MAX     128   /* Stack buffer for text attributes */

typedef enum {
    USB_ATTR_BUS = 0,
    USB_ATTR_DEVNUM,
    USB_ATTR_PORT,
    USB_ATTR_VENDOR,
    USB_ATTR_PRODUCT,
    USB_ATTR_CLASS,
    USB_ATTR_SUBCLASS,
    USB_ATTR_PROTOCOL,
    USB_ATTR_SPEED,
    USB_ATTR_DESCRIPTORS,
    USB_ATTR_COUNT
} usb_attr_id_t;

/* Attribute context stored in devfs node private_data */
typedef struct usb_attr_ctx {
    struct xhci_slot *slot;
    uint8_t bus_id;
    uint8_t attr;
} usb_attr_ctx_t;

struct vfs_node;

int usb_devfs_format_attr(const struct xhci_slot *slot, uint8_t bus_id,
                           usb_attr_id_t attr, char *buf, size_t size);
int usb_devfs_register_slot(uint8_t bus_id, struct xhci_slot *slot);
int usb_devfs_register_all(void);
int usb_attr_read(struct vfs_node *node, uint64_t offset, uint64_t size, void *buffer);

#endif /* _DRIVER_USB_DEVFS_H */
```

### 2.3 Global xHCI Controller Tracking
To allow `usb_devfs_register_all()` (called from `kernel/core/main.c` after `fs_boot_prepare()`) to discover configured devices across controllers:
- In `kernel/driver/xhci.c`:
  ```c
  #define XHCI_MAX_CONTROLLERS 4
  static struct xhci_controller *s_xhci_controllers[XHCI_MAX_CONTROLLERS];
  static uint8_t s_xhci_controller_count = 0;

  uint8_t xhci_controller_count(void)
  {
      return s_xhci_controller_count;
  }

  struct xhci_controller *xhci_controller_get(uint8_t index)
  {
      if (index >= s_xhci_controller_count) return NULL;
      return s_xhci_controllers[index];
  }
  ```
  - In `xhci_probe()`:
    ```c
    if (s_xhci_controller_count < XHCI_MAX_CONTROLLERS) {
        s_xhci_controllers[s_xhci_controller_count++] = ctrl;
    } else {
        log_warn("[xhci] controller limit (%u) reached\n", XHCI_MAX_CONTROLLERS);
    }
    ```
  - In `xhci_remove()`: deregister controller from array and compact.
  - Export prototypes in `kernel/include/driver/xhci.h`.

---

## 3. Detailed Workflows & Node Operations

### 3.1 Kernel devfs Registration & Reading (`kernel/driver/usb_devfs.c`)

1. **Attribute Name Mapping**:
   C99 designated initializers guarantee perfect synchronization with `usb_attr_id_t`:
   ```c
   static const char *const s_attr_names[USB_ATTR_COUNT] = {
       [USB_ATTR_BUS]         = "bus",
       [USB_ATTR_DEVNUM]      = "devnum",
       [USB_ATTR_PORT]        = "port",
       [USB_ATTR_VENDOR]      = "vendor",
       [USB_ATTR_PRODUCT]     = "product",
       [USB_ATTR_CLASS]       = "class",
       [USB_ATTR_SUBCLASS]    = "subclass",
       [USB_ATTR_PROTOCOL]    = "protocol",
       [USB_ATTR_SPEED]       = "speed",
       [USB_ATTR_DESCRIPTORS] = "descriptors",
   };
   ```

2. **Attribute Formatting (`usb_devfs_format_attr`)**:
   - `USB_ATTR_BUS`: `snprintf(buf, size, "%u\n", (unsigned)bus_id)`
   - `USB_ATTR_DEVNUM`: `snprintf(buf, size, "%u\n", (unsigned)slot->slot_id)`
   - `USB_ATTR_PORT`: `snprintf(buf, size, "%u\n", (unsigned)slot->port_num)`
   - `USB_ATTR_VENDOR`: `snprintf(buf, size, "0x%04x\n", (unsigned)slot->desc.idVendor)`
   - `USB_ATTR_PRODUCT`: `snprintf(buf, size, "0x%04x\n", (unsigned)slot->desc.idProduct)`
   - `USB_ATTR_CLASS`: `snprintf(buf, size, "0x%02x\n", (unsigned)slot->desc.bDeviceClass)`
   - `USB_ATTR_SUBCLASS`: `snprintf(buf, size, "0x%02x\n", (unsigned)slot->desc.bDeviceSubClass)`
   - `USB_ATTR_PROTOCOL`: `snprintf(buf, size, "0x%02x\n", (unsigned)slot->desc.bDeviceProtocol)`
   - `USB_ATTR_SPEED`:
     - If `speed == XHCI_PORT_SPEED_LOW`: `"low\n"`
     - If `speed == XHCI_PORT_SPEED_FULL`: `"full\n"`
     - If `speed == XHCI_PORT_SPEED_HIGH`: `"high\n"`
     - If `speed == XHCI_PORT_SPEED_SUPER`: `"super\n"`
     - Else: `"unknown\n"`
   - `USB_ATTR_DESCRIPTORS`:
     - If `!slot->raw_descriptors || slot->raw_descriptors_len == 0`: return 0;
     - `size_t n = (size < slot->raw_descriptors_len) ? size : slot->raw_descriptors_len;`
     - `if (buf && n > 0) memcpy(buf, slot->raw_descriptors, n);`
     - Return `(int)slot->raw_descriptors_len;` (snprintf semantics, consistent with `PCI_ATTR_CONFIG`).

3. **devfs Node Slicing (`usb_attr_read`)**:
   ```c
   int usb_attr_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
   {
       usb_attr_ctx_t *ctx = (usb_attr_ctx_t *)devfs_get_private(node);
       if (!ctx || !ctx->slot || !buffer) return -EINVAL;
       if (size == 0) return 0;

       if (ctx->attr == USB_ATTR_DESCRIPTORS) {
           if (!ctx->slot->raw_descriptors || ctx->slot->raw_descriptors_len == 0) return 0;
           if (offset >= (uint64_t)ctx->slot->raw_descriptors_len) return 0;
           uint64_t avail = (uint64_t)ctx->slot->raw_descriptors_len - offset;
           uint64_t n = (size < avail) ? size : avail;
           memcpy(buffer, ctx->slot->raw_descriptors + offset, n);
           return (int)n;
       }

       char tmp[USB_DEVFS_FMT_MAX];
       int len = usb_devfs_format_attr(ctx->slot, ctx->bus_id, (usb_attr_id_t)ctx->attr,
                                       tmp, sizeof(tmp));
       if (len < 0) return len;
       if (offset >= (uint64_t)len) return 0;

       uint64_t avail = (uint64_t)len - offset;
       uint64_t n = (size < avail) ? size : avail;
       memcpy(buffer, tmp + offset, n);
       return (int)n;
   }
   ```

4. **Slot Registration (`usb_devfs_register_slot`)**:
   - `char dev_id[USB_DEVFS_ID_MAX]; snprintf(dev_id, sizeof(dev_id), "%u-%u", bus_id, slot->port_num);`
   - Iterate `i` from 0 to `USB_ATTR_COUNT - 1`:
     - Path: `"usb/%s/%s", dev_id, s_attr_names[i]`.
     - Allocate `usb_attr_ctx_t *ctx = kmalloc(sizeof(*ctx));`
     - Set `ctx->slot = slot; ctx->bus_id = bus_id; ctx->attr = i;`
     - `int rc = devfs_register_path(path, ctx, &usb_attr_ops);`
     - If `rc == -EEXIST` or `rc != 0`: `kfree(ctx);` (prevent memory leaks on duplicate or failed nodes).

5. **Bus Publication (`usb_devfs_register_all`)**:
   - Called from `kernel/core/main.c` immediately after `pci_devfs_register_all()`.
   - Iterates `c` from 0 to `xhci_controller_count() - 1`:
     - `struct xhci_controller *ctrl = xhci_controller_get(c);`
     - `uint8_t bus_id = (uint8_t)(c + 1);` (1-based bus ID).
     - Loops `s` from 1 to `XHCI_MAX_MANAGED_SLOTS`:
       - If `ctrl->slots[s] && ctrl->slots[s]->configured`:
         `usb_devfs_register_slot(bus_id, ctrl->slots[s]);`
   - Logs: `[usb_devfs] published %d attribute nodes for %u USB devices\n`.

---

### 3.2 User-space `lsusb` Architecture (`user/lsusb.c`)

1. **Option Parsing & Filter Specifications**:
   - Syntax: `lsusb [-v] [-s [[bus]:][devnum]] [-h]`
   - Options:
     - `-v`: Verbose descriptor dump.
     - `-s [[bus]:][devnum]`: Filter devices by bus and/or device number.
       - `"1:2"`: bus == 1 && devnum == 2
       - `"1:"`: bus == 1
       - `":2"` or `"2"`: devnum == 2
     - `-h`: Print usage and exit with 0.
   - If unknown option or invalid syntax: print usage to stderr and exit with code 2.

2. **Directory Traversal & Deterministic Sorting**:
   - Open `/dev/usb` using `opendir()`. If open fails, print error to stderr and exit with code 1.
   - Collect valid entries into an array of structs:
     ```c
     struct usb_dev_entry {
         char name[32];      /* e.g. "1-1" */
         int bus;
         int devnum;
         int port;
         uint16_t vid;
         uint16_t pid;
         uint8_t cls;
     };
     ```
   - Sort array with `qsort()` using comparison:
     - Primary key: `bus` ascending.
     - Secondary key: `port` ascending (or `devnum` ascending).

3. **Vendor/Product Name Resolution**:
   - Lookup table:
     ```c
     static const struct { uint16_t vid, pid; const char *name; } kKnownDevices[] = {
         { 0x0627, 0x0001, "QEMU USB Tablet" },
         { 0x0627, 0x0002, "QEMU USB Mouse" },
         { 0x0627, 0x0003, "QEMU USB Keyboard" },
         { 0x05e3, 0x0608, "Genesys Logic, Inc. Hub" },
         { 0x1b1c, 0x0000, "Corsair" },
         { 0x046d, 0x0000, "Logitech, Inc." },
     };
     ```
   - If not found in table, look up class code:
     - `0x03` -> "Human Interface Device"
     - `0x08` -> "Mass Storage"
     - `0x09` -> "Hub"
     - Fallback: "Device %04x:%04x".

4. **Default Output Format**:
   ```text
   Bus 001 Device 001: ID 0627:0001 QEMU USB Tablet
   ```

5. **Verbose Output (`-v`) via `/dev/usb/<dev>/descriptors`**:
   - Open `/dev/usb/<name>/descriptors`.
   - Read entire binary descriptor stream into local buffer (`read()`).
   - Parse and dump Device Descriptor:
     - `bLength`, `bDescriptorType`, `bcdUSB`, `bDeviceClass`, `bDeviceSubClass`, `bDeviceProtocol`, `bMaxPacketSize0`, `idVendor`, `idProduct`, `bcdDevice`, `bNumConfigurations`.
   - Walk remaining buffer starting at offset 18:
     - Parse Configuration Descriptor: `bLength`, `bDescriptorType`, `wTotalLength`, `bNumInterfaces`, `bConfigurationValue`.
     - Parse Interface Descriptor: `bInterfaceNumber`, `bAlternateSetting`, `bNumEndpoints`, `bInterfaceClass` (pretty-print name), `bInterfaceSubClass`, `bInterfaceProtocol`.
     - Parse Endpoint Descriptor: `bEndpointAddress` (e.g. `0x81 EP 1 IN`), `bmAttributes` (`Interrupt` / `Bulk` / `Isochronous`), `wMaxPacketSize`, `bInterval`.

6. **Exit Codes**:
   - `0`: At least one device listed (or matched filter `-s`).
   - `1`: `/dev/usb` does not exist, or 0 devices found / matched.
   - `2`: Command line usage/argument syntax error.

---

## 4. Testing & Verification Plan

### 4.1 Host Unit Tests (`hosttests/cases/test_usb_devfs.c`)
1. **`test_usb_devfs_formatting`**:
   - Test `usb_devfs_format_attr()` for all 10 attribute IDs.
   - Verify `vendor` -> `"0x0627\n"`, `speed` -> `"full\n"`, `port` -> `"1\n"`, `devnum` -> `"1\n"`, `bus` -> `"1\n"`.
2. **`test_usb_devfs_descriptors_binary_slicing`**:
   - Allocate 52-byte descriptor stream (18B Device + 34B Config Hierarchy).
   - Test offset reading:
     - Read offset 0, size 18: exactly matches Device Descriptor.
     - Read offset 18, size 34: exactly matches Config Hierarchy.
     - Read offset 52: returns 0 (EOF).
     - Read offset 10, size 100: returns 42 (clamped).
3. **`test_usb_devfs_registration_and_lookup`**:
   - Set up mock devfs tree and call `usb_devfs_register_slot(1, &slot)`.
   - Verify paths `"usb/1-1/vendor"`, `"usb/1-1/descriptors"`, etc. are created and accessible via VFS.
4. **`test_lsusb_filter_and_unpack_logic`**:
   - Test `-s` parsing: `"1:1"`, `"1:"`, `":1"`, `"1"`, and malformed inputs.
   - Test binary descriptor unpacker functions for Device, Config, Interface, and Endpoint descriptors.

### 4.2 Build & Link Dependencies
In `hosttests/mk/drivers.mk`:
```makefile
$(eval $(call register_host_test,test_usb_devfs,drivers,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_usb_devfs.elf

USB_DEVFS_HOST_CFLAGS := $(XHCI_HOST_CFLAGS) -D__SPINLOCK_H__

$(TEST_BLD)/test_usb_devfs.o: $(TEST_CASES)/test_usb_devfs.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(USB_DEVFS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/usb_devfs_production.o: $(TESTS_DIR)/kernel/driver/usb_devfs.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(USB_DEVFS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/lsusb_production.o: $(TESTS_DIR)/user/lsusb.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -DLSUSB_NO_MAIN $(FRAMEWORK_INC) $(LIBC_INC) $(KERNEL_INC) -c $< -o $@

$(TEST_BLD)/test_usb_devfs.elf: $(TEST_BLD)/test_usb_devfs.o \
    $(TEST_BLD)/usb_devfs_production.o \
    $(TEST_BLD)/lsusb_production.o \
    $(TEST_BLD)/xhci_production.o \
    $(TEST_BLD)/device_core_production.o \
    $(TEST_BLD)/pci_core_production.o \
    $(TEST_BLD)/pci_match_production.o \
    $(TEST_BLD)/pci_platform_production.o \
    $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

.PHONY: test-usb-devfs
test-usb-devfs: $(TEST_BLD)/test_usb_devfs.elf
	$<
```

### 4.3 QEMU E2E Verification
- Boot QEMU with `-device qemu-xhci,id=xhci -device usb-tablet,bus=xhci.0`.
- Verify in serial output:
  ```text
  /bin/lsusb
  Bus 001 Device 001: ID 0627:0001 QEMU USB Tablet
  ```
- Run `/bin/lsusb -v` and verify:
  ```text
  bInterfaceClass 3 Human Interface Device
  bEndpointAddress 0x81 EP 1 IN
  ```

---

## 5. Implementation Scope & File Plan
- `kernel/include/driver/xhci.h`: Add `raw_descriptors`, `raw_descriptors_len` to `struct xhci_slot`; export `xhci_controller_count()` and `xhci_controller_get()`.
- `kernel/driver/xhci.c`: Implement controller tracking, cache `raw_descriptors` during probe, and clean up in `xhci_free_slot()`.
- `kernel/include/driver/usb_devfs.h`: Declare public API, attribute IDs, and constants.
- `kernel/driver/usb_devfs.c`: Implement `usb_devfs_format_attr()`, read callback with slicing, `usb_devfs_register_slot()`, and `usb_devfs_register_all()`.
- `kernel/core/main.c`: Invoke `usb_devfs_register_all()` after `pci_devfs_register_all()`.
- `user/lsusb.c`: Complete userland `lsusb` utility supporting standard mode, `-v`, and `-s` filtering.
- `hosttests/cases/test_usb_devfs.c`: Unit tests.
- `hosttests/mk/drivers.mk`: Build rules and dependencies for `test_usb_devfs`.
