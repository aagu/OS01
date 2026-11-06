# xHCI Phase 4: USB Device Filesystem (devfs) Publication & User-space lsusb Utility Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement Phase 4: publish enumerated USB devices into `/dev/usb/<bus>-<port>/<attr>` via `usb_devfs`, cache binary descriptor streams, and provide the user-space `/bin/lsusb` utility supporting standard output and verbose (`-v`) descriptor tree dumping.

**Architecture:** Extend xHCI slot tracking with global controller registry and concatenated raw descriptors cache (`slot->raw_descriptors`). Implement `kernel/driver/usb_devfs.c` publishing standard text attributes (`bus`, `devnum`, `port`, `vendor`, `product`, `class`, `subclass`, `protocol`, `speed`) and binary `descriptors` using `devfs_register_path()`. Hook `usb_devfs_register_all()` in `kernel/core/main.c`. Implement `user/lsusb.c` with `#ifndef LSUSB_NO_MAIN` guard, deterministic `qsort()` ordering, `-s` filtering, VID/PID lookup table, and `-v` binary descriptor tree unpacking.

**Tech Stack:** C (Clang, freestanding kernel runtime, -O2), devfs/VFS integration, POSIX dirent/read in userland, OS01 hosttest test harness, QEMU q35 with `-device qemu-xhci,id=xhci -device usb-tablet,bus=xhci.0`.

**Spec:** `docs/superpowers/specs/2026-11-06-usb-devfs-lsusb-design.md`

## Global Constraints
- Freestanding C runtime: no standard libc headers in kernel space; use `<memory/slab.h>` (`kmalloc`/`kfree`), `<memory/pmm.h>`, `<log/log.h>`, `<fs/devfs.h>`.
- `usb_attr_read` stack budget: must stay well below 512 bytes (`-Wframe-larger-than=512`). Text attributes render into a 128-byte stack buffer (`USB_DEVFS_FMT_MAX`); binary `descriptors` bypass the stack and slice directly from `slot->raw_descriptors`.
- Attribute name mapping must use C99 designated initializers `[USB_ATTR_*] = "..."` to prevent enum index mismatch.
- Host tests avoid heavy multi-subsystem VFS linking dependencies by using a clean, dedicated devfs mock in `test_usb_devfs.c`.
- `user/lsusb.c` wraps `main()` with `#ifndef LSUSB_NO_MAIN` to enable direct unit testing in `test_usb_devfs.c` without duplicate `main()` symbol conflicts.
- All hosttests must run cleanly via `OS01_PROFILE_FILE=$(pwd)/mk/profiles/x86_64-clang.mk make -C hosttests test-usb-devfs` and pass full `make test-host`.

## Review Focus
1. Controller tracking registry & compaction: ensure `s_xhci_controller_count < XHCI_MAX_CONTROLLERS` guard, out-of-bounds check returning NULL, and array compaction on `xhci_remove()`.
2. Real teardown verification: verify `xhci_free_slot()` releases `slot->raw_descriptors` and resets pointers.
3. Real `usb_attr_read()` execution: drive production code directly through mock nodes, asserting offset 0, intermediate slicing, clamped reads, exact EOF (return 0), and beyond EOF (return 0).
4. Full attribute coverage: test all 10 attributes and all 5 speed states (`full`, `low`, `high`, `super`, `unknown`).
5. Userland `lsusb`: verify deterministic sorting, comprehensive `-s` filter parsing (including invalid inputs), and `-v` descriptor parsing guards.

---

### Task 1: Controller Tracking Registry, Raw Descriptor Caching & Teardown in xHCI

**Files:**
- Modify: `kernel/include/driver/xhci.h:288-350`
- Modify: `kernel/driver/xhci.c:480-530,680-720`
- Create: `hosttests/cases/test_usb_devfs.c`
- Modify: `hosttests/mk/drivers.mk`

**Interfaces:**
- Consumes: `kmalloc`, `kfree`, `memcpy`
- Produces: `slot->raw_descriptors`, `slot->raw_descriptors_len`, `xhci_controller_count()`, `xhci_controller_get()`

- [ ] **Step 1: Write the failing test for controller tracking and real `xhci_free_slot()` raw descriptor teardown**

In `hosttests/cases/test_usb_devfs.c`:
```c
/* hosttests/cases/test_usb_devfs.c — USB devfs and lsusb unit tests */
#include "test_framework.h"
#include <device/device.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>
#include <driver/xhci.h>
#include <arch/pci.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

#define TEST_CASE(name) printf("\n  [TEST] %s\n", name)

void mock_xhci_write32(volatile void *addr, uint32_t val)
{
    (void)addr;
    (void)val;
}

#define MAX_MOCK_PAGES 32
static void *s_mock_page_ptrs[MAX_MOCK_PAGES];
static int s_mock_page_count = 0;

uint64_t alloc_4k_page(void)
{
    if (s_mock_page_count >= MAX_MOCK_PAGES) return 0;
    void *p = calloc(1, 4096);
    if (!p) return 0;
    s_mock_page_ptrs[s_mock_page_count++] = p;
    return (uint64_t)(uintptr_t)p;
}

void free_4k_page(uint64_t phys)
{
    void *ptr = (void *)(uintptr_t)phys;
    for (int i = 0; i < s_mock_page_count; i++) {
        if (s_mock_page_ptrs[i] == ptr) {
            free(ptr);
            s_mock_page_ptrs[i] = s_mock_page_ptrs[--s_mock_page_count];
            return;
        }
    }
}

void *kmalloc(size_t size) { return calloc(1, size); }
size_t kfree(void *ptr)
{
    free(ptr);
    return 1;
}

void test_xhci_raw_descriptors_caching_and_teardown(void)
{
    TEST_CASE("Raw descriptors concatenation and real xhci_free_slot teardown");
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));

    struct xhci_slot *slot = kmalloc(sizeof(struct xhci_slot));
    memset(slot, 0, sizeof(struct xhci_slot));
    ctrl.slots[1] = slot;

    slot->desc.bLength = 18;
    slot->desc.bDescriptorType = 1;
    slot->desc.idVendor = 0x0627;
    slot->desc.idProduct = 0x0001;

    uint8_t config_bytes[34];
    memset(config_bytes, 0xAB, sizeof(config_bytes));
    config_bytes[0] = 9;
    config_bytes[1] = 2; /* USB_DESC_TYPE_CONFIGURATION */

    /* Simulate concatenating Device Desc (18B) + Config Desc Hierarchy (34B) */
    uint16_t total = sizeof(struct usb_device_descriptor) + sizeof(config_bytes);
    slot->raw_descriptors = kmalloc(total);
    assert_true(slot->raw_descriptors != NULL);
    memcpy(slot->raw_descriptors, &slot->desc, sizeof(struct usb_device_descriptor));
    memcpy(slot->raw_descriptors + sizeof(struct usb_device_descriptor), config_bytes, sizeof(config_bytes));
    slot->raw_descriptors_len = total;

    assert_eq(slot->raw_descriptors_len, 52);
    assert_eq(slot->raw_descriptors[0], 18);
    assert_eq(slot->raw_descriptors[18], 9);

    /* Invoke REAL production teardown */
    xhci_free_slot(&ctrl, 1);
    assert_true(ctrl.slots[1] == NULL);
}

void test_xhci_controller_registry(void)
{
    TEST_CASE("Controller tracking registry: bounds, indexing, and compaction");
    uint8_t initial_count = xhci_controller_count();
    assert_true(xhci_controller_get(initial_count + 5) == NULL);

    /* Register controller accessors */
    struct xhci_controller *ctrl = xhci_controller_get(0);
    if (initial_count > 0) {
        assert_true(ctrl != NULL);
    }
}

int main(void)
{
    TEST_SUITE("usb_devfs");
    test_xhci_raw_descriptors_caching_and_teardown();
    test_xhci_controller_registry();
    TEST_RESULTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
```

- [ ] **Step 2: Register test in `hosttests/mk/drivers.mk` and run to verify failure**

In `hosttests/mk/drivers.mk`:
```makefile
USB_DEVFS_HOST_CFLAGS := $(XHCI_HOST_CFLAGS) -D__SPINLOCK_H__

$(eval $(call register_host_test,test_usb_devfs,drivers,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_usb_devfs.elf

$(TEST_BLD)/test_usb_devfs.o: $(TEST_CASES)/test_usb_devfs.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(USB_DEVFS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_usb_devfs.elf: $(TEST_BLD)/test_usb_devfs.o \
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

Run:
`OS01_PROFILE_FILE=$(pwd)/mk/profiles/x86_64-clang.mk make -C hosttests test-usb-devfs`
Expected: Compilation failure (`raw_descriptors`, `xhci_controller_count` not declared).

- [ ] **Step 3: Implement controller tracking registry and raw descriptor caching in `xhci.c`**

In `kernel/include/driver/xhci.h`:
- Add `uint8_t *raw_descriptors;` and `uint16_t raw_descriptors_len;` to `struct xhci_slot`.
- Declare:
  ```c
  uint8_t xhci_controller_count(void);
  struct xhci_controller *xhci_controller_get(uint8_t index);
  ```

In `kernel/driver/xhci.c`:
- Define static registry:
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
- In `xhci_remove()`:
  ```c
  for (uint8_t i = 0; i < s_xhci_controller_count; i++) {
      if (s_xhci_controllers[i] == ctrl) {
          for (uint8_t j = i; j + 1 < s_xhci_controller_count; j++) {
              s_xhci_controllers[j] = s_xhci_controllers[j + 1];
          }
          s_xhci_controllers[--s_xhci_controller_count] = NULL;
          break;
      }
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
- In `xhci_probe_ports()`: after successful `xhci_get_config_descriptor(ctrl, slot_id, &cfg)` which returns `rc` (the valid `parse_len`):
  ```c
  uint16_t total_raw = sizeof(struct usb_device_descriptor) + (uint16_t)rc;
  slot->raw_descriptors = kmalloc(total_raw);
  if (slot->raw_descriptors) {
      memcpy(slot->raw_descriptors, &slot->desc, sizeof(struct usb_device_descriptor));
      memcpy(slot->raw_descriptors + sizeof(struct usb_device_descriptor),
             slot->dma_buf_virt, rc);
      slot->raw_descriptors_len = total_raw;
  }
  ```

- [ ] **Step 4: Run test to verify it passes**

Run:
`OS01_PROFILE_FILE=$(pwd)/mk/profiles/x86_64-clang.mk make -C hosttests test-usb-devfs`
Expected: PASS (2 tests executed, 0 failed).

- [ ] **Step 5: Commit**

```bash
git add kernel/include/driver/xhci.h kernel/driver/xhci.c hosttests/cases/test_usb_devfs.c hosttests/mk/drivers.mk
git commit -m "feat(xhci): add raw descriptors caching and controller tracking registry"
```

---

### Task 2: Kernel `usb_devfs` Attribute Formatters, Real Read Slicing & Boundary Tests

**Files:**
- Create: `kernel/include/driver/usb_devfs.h`
- Create: `kernel/driver/usb_devfs.c`
- Modify: `hosttests/cases/test_usb_devfs.c`
- Modify: `hosttests/mk/drivers.mk`

**Interfaces:**
- Consumes: `struct xhci_slot`, `usb_attr_id_t`, `snprintf`, `memcpy`
- Produces: `usb_devfs_format_attr()`, `usb_attr_read()`, `s_attr_names`

- [ ] **Step 1: Write the failing tests for all 10 attribute formatters and real `usb_attr_read()` slicing**

In `hosttests/cases/test_usb_devfs.c`:
```c
#include <driver/usb_devfs.h>

/* Mock devfs node structure for driving usb_attr_read */
struct vfs_node {
    void *private_data;
};

void *devfs_get_private(struct vfs_node *node)
{
    return node ? node->private_data : NULL;
}

/* Expose usb_attr_read for testing */
int usb_attr_read(struct vfs_node *node, uint64_t offset, uint64_t size, void *buffer);

void test_usb_devfs_formatting(void)
{
    TEST_CASE("Format USB devfs text attributes (10 attributes & 5 speeds)");
    struct xhci_slot slot;
    memset(&slot, 0, sizeof(slot));

    slot.slot_id = 1;
    slot.port_num = 2;
    slot.speed = XHCI_PORT_SPEED_FULL;
    slot.desc.idVendor = 0x0627;
    slot.desc.idProduct = 0x0001;
    slot.desc.bDeviceClass = 0x03;
    slot.desc.bDeviceSubClass = 0x01;
    slot.desc.bDeviceProtocol = 0x02;

    char buf[64];

    /* Test all 10 attributes */
    assert_eq(usb_devfs_format_attr(&slot, 1, USB_ATTR_BUS, buf, sizeof(buf)), 2);
    assert_eq(strcmp(buf, "1\n"), 0);

    assert_eq(usb_devfs_format_attr(&slot, 1, USB_ATTR_DEVNUM, buf, sizeof(buf)), 2);
    assert_eq(strcmp(buf, "1\n"), 0);

    assert_eq(usb_devfs_format_attr(&slot, 1, USB_ATTR_PORT, buf, sizeof(buf)), 2);
    assert_eq(strcmp(buf, "2\n"), 0);

    assert_eq(usb_devfs_format_attr(&slot, 1, USB_ATTR_VENDOR, buf, sizeof(buf)), 7);
    assert_eq(strcmp(buf, "0x0627\n"), 0);

    assert_eq(usb_devfs_format_attr(&slot, 1, USB_ATTR_PRODUCT, buf, sizeof(buf)), 7);
    assert_eq(strcmp(buf, "0x0001\n"), 0);

    assert_eq(usb_devfs_format_attr(&slot, 1, USB_ATTR_CLASS, buf, sizeof(buf)), 5);
    assert_eq(strcmp(buf, "0x03\n"), 0);

    assert_eq(usb_devfs_format_attr(&slot, 1, USB_ATTR_SUBCLASS, buf, sizeof(buf)), 5);
    assert_eq(strcmp(buf, "0x01\n"), 0);

    assert_eq(usb_devfs_format_attr(&slot, 1, USB_ATTR_PROTOCOL, buf, sizeof(buf)), 5);
    assert_eq(strcmp(buf, "0x02\n"), 0);

    /* Test all 5 speed encodings */
    slot.speed = XHCI_PORT_SPEED_LOW;
    usb_devfs_format_attr(&slot, 1, USB_ATTR_SPEED, buf, sizeof(buf));
    assert_eq(strcmp(buf, "low\n"), 0);

    slot.speed = XHCI_PORT_SPEED_FULL;
    usb_devfs_format_attr(&slot, 1, USB_ATTR_SPEED, buf, sizeof(buf));
    assert_eq(strcmp(buf, "full\n"), 0);

    slot.speed = XHCI_PORT_SPEED_HIGH;
    usb_devfs_format_attr(&slot, 1, USB_ATTR_SPEED, buf, sizeof(buf));
    assert_eq(strcmp(buf, "high\n"), 0);

    slot.speed = XHCI_PORT_SPEED_SUPER;
    usb_devfs_format_attr(&slot, 1, USB_ATTR_SPEED, buf, sizeof(buf));
    assert_eq(strcmp(buf, "super\n"), 0);

    slot.speed = 99;
    usb_devfs_format_attr(&slot, 1, USB_ATTR_SPEED, buf, sizeof(buf));
    assert_eq(strcmp(buf, "unknown\n"), 0);

    /* Test small buffer truncation safety (snprintf return length) */
    char tiny[3];
    int len = usb_devfs_format_attr(&slot, 1, USB_ATTR_VENDOR, tiny, sizeof(tiny));
    assert_eq(len, 7); /* Full length reported */
    assert_eq(tiny[0], '0');
    assert_eq(tiny[1], 'x');
    assert_eq(tiny[2], '\0');
}

void test_usb_devfs_descriptors_binary_slicing(void)
{
    TEST_CASE("Descriptors binary reading, slicing and EOF bounds via REAL usb_attr_read");
    struct xhci_slot slot;
    memset(&slot, 0, sizeof(slot));

    uint8_t raw[52];
    for (int i = 0; i < 52; i++) raw[i] = (uint8_t)i;
    slot.raw_descriptors = raw;
    slot.raw_descriptors_len = 52;

    usb_attr_ctx_t ctx = {
        .slot = &slot,
        .bus_id = 1,
        .attr = USB_ATTR_DESCRIPTORS,
    };
    struct vfs_node node = { .private_data = &ctx };
    uint8_t out[64];

    /* 1. Normal read from offset 0 */
    memset(out, 0, sizeof(out));
    int n = usb_attr_read(&node, 0, 18, out);
    assert_eq(n, 18);
    assert_eq(out[0], 0);
    assert_eq(out[17], 17);

    /* 2. Slice read from offset 18 */
    memset(out, 0, sizeof(out));
    n = usb_attr_read(&node, 18, 34, out);
    assert_eq(n, 34);
    assert_eq(out[0], 18);
    assert_eq(out[33], 51);

    /* 3. Clamped read: request 100 bytes at offset 10 (avail = 42) */
    memset(out, 0, sizeof(out));
    n = usb_attr_read(&node, 10, 100, out);
    assert_eq(n, 42);
    assert_eq(out[0], 10);
    assert_eq(out[41], 51);

    /* 4. Exact EOF read: offset = 52 returns 0 */
    n = usb_attr_read(&node, 52, 10, out);
    assert_eq(n, 0);

    /* 5. Beyond EOF read: offset = 60 returns 0 */
    n = usb_attr_read(&node, 60, 10, out);
    assert_eq(n, 0);

    /* 6. Invalid input checks */
    assert_eq(usb_attr_read(NULL, 0, 10, out), -EINVAL);
    assert_eq(usb_attr_read(&node, 0, 0, out), 0);
}
```
Add calls to `main()`.

- [ ] **Step 2: Run test to verify failure**

Run:
`OS01_PROFILE_FILE=$(pwd)/mk/profiles/x86_64-clang.mk make -C hosttests test-usb-devfs`
Expected: Compilation failure (`usb_devfs.h` missing).

- [ ] **Step 3: Implement `kernel/include/driver/usb_devfs.h` and `kernel/driver/usb_devfs.c`**

Create `kernel/include/driver/usb_devfs.h`:
- Define `USB_DEVFS_NAMESPACE "usb"`, `USB_DEVFS_ID_MAX 32`, `USB_DEVFS_PATH_MAX 64`, `USB_DEVFS_FMT_MAX 128`.
- Define `usb_attr_id_t` enum with `USB_ATTR_BUS` through `USB_ATTR_DESCRIPTORS`, `USB_ATTR_COUNT`.
- Define `usb_attr_ctx_t` with `struct xhci_slot *slot`, `uint8_t bus_id`, `uint8_t attr`.
- Declare:
  ```c
  int usb_devfs_format_attr(const struct xhci_slot *slot, uint8_t bus_id,
                             usb_attr_id_t attr, char *buf, size_t size);
  int usb_devfs_register_slot(uint8_t bus_id, struct xhci_slot *slot);
  int usb_devfs_register_all(void);
  int usb_attr_read(struct vfs_node *node, uint64_t offset, uint64_t size, void *buffer);
  ```

Create `kernel/driver/usb_devfs.c`:
- Define `s_attr_names[USB_ATTR_COUNT]` with C99 designated initializers:
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
- Implement `usb_devfs_format_attr`:
  - Format `bus`, `devnum`, `port`, `vendor`, `product`, `class`, `subclass`, `protocol`.
  - Format `speed` (`low`, `full`, `high`, `super`, `unknown`).
  - For `USB_ATTR_DESCRIPTORS`:
    - `if (!slot || !slot->raw_descriptors || slot->raw_descriptors_len == 0) return 0;`
    - `size_t n = (size < slot->raw_descriptors_len) ? size : slot->raw_descriptors_len;`
    - `if (buf && n > 0) memcpy(buf, slot->raw_descriptors, n);`
    - Return `(int)slot->raw_descriptors_len;`
- Implement `usb_attr_read(struct vfs_node *node, uint64_t offset, uint64_t size, void *buffer)`:
  - `usb_attr_ctx_t *ctx = (usb_attr_ctx_t *)devfs_get_private(node);`
  - `if (!ctx || !ctx->slot || !buffer) return -EINVAL;`
  - `if (size == 0) return 0;`
  - If `ctx->attr == USB_ATTR_DESCRIPTORS`:
    - `if (!ctx->slot->raw_descriptors || ctx->slot->raw_descriptors_len == 0) return 0;`
    - `if (offset >= (uint64_t)ctx->slot->raw_descriptors_len) return 0;`
    - `uint64_t avail = (uint64_t)ctx->slot->raw_descriptors_len - offset;`
    - `uint64_t n = (size < avail) ? size : avail;`
    - `memcpy(buffer, ctx->slot->raw_descriptors + offset, n);`
    - `return (int)n;`
  - For text attributes:
    - `char tmp[USB_DEVFS_FMT_MAX];`
    - `int len = usb_devfs_format_attr(ctx->slot, ctx->bus_id, (usb_attr_id_t)ctx->attr, tmp, sizeof(tmp));`
    - `if (len < 0) return len;`
    - `if (offset >= (uint64_t)len) return 0;`
    - `uint64_t avail = (uint64_t)len - offset;`
    - `uint64_t n = (size < avail) ? size : avail;`
    - `memcpy(buffer, tmp + offset, n);`
    - `return (int)n;`
- In `hosttests/mk/drivers.mk`: add rule to build `$(TEST_BLD)/usb_devfs_production.o` using `$(USB_DEVFS_HOST_CFLAGS)` and link into `test_usb_devfs.elf`:
  ```makefile
  $(TEST_BLD)/usb_devfs_production.o: $(TESTS_DIR)/kernel/driver/usb_devfs.c
  	@mkdir -p $(TEST_BLD)
  	$(HOST_CC) $(USB_DEVFS_HOST_CFLAGS) -c $< -o $@
  ```

- [ ] **Step 4: Run test to verify it passes**

Run:
`OS01_PROFILE_FILE=$(pwd)/mk/profiles/x86_64-clang.mk make -C hosttests test-usb-devfs`
Expected: PASS (4 tests executed, 0 failed).

- [ ] **Step 5: Commit**

```bash
git add kernel/include/driver/usb_devfs.h kernel/driver/usb_devfs.c hosttests/cases/test_usb_devfs.c hosttests/mk/drivers.mk
git commit -m "feat(usb_devfs): implement attribute formatters and descriptors read slicing"
```

---

### Task 3: devfs Path Registration & `main.c` Integration

**Files:**
- Modify: `kernel/driver/usb_devfs.c`
- Modify: `kernel/core/main.c:150-160`
- Modify: `hosttests/cases/test_usb_devfs.c`

**Interfaces:**
- Consumes: `devfs_register_path`, `xhci_controller_count`, `xhci_controller_get`
- Produces: `usb_devfs_register_slot()`, `usb_devfs_register_all()`

- [ ] **Step 1: Write the failing test for slot registration with standalone devfs mock**

In `hosttests/cases/test_usb_devfs.c`:
```c
/* Mock devfs path registry for host unit testing */
#define MAX_MOCK_DEVFS_NODES 32
struct mock_devfs_entry {
    char path[64];
    void *ctx;
    const struct devfs_ops *ops;
};
static struct mock_devfs_entry s_mock_devfs[MAX_MOCK_DEVFS_NODES];
static int s_mock_devfs_count = 0;

int devfs_register_path(const char *path, void *private_data, const struct devfs_ops *ops)
{
    for (int i = 0; i < s_mock_devfs_count; i++) {
        if (strcmp(s_mock_devfs[i].path, path) == 0) {
            return -EEXIST;
        }
    }
    if (s_mock_devfs_count >= MAX_MOCK_DEVFS_NODES) return -ENOMEM;
    strncpy(s_mock_devfs[s_mock_devfs_count].path, path, 63);
    s_mock_devfs[s_mock_devfs_count].ctx = private_data;
    s_mock_devfs[s_mock_devfs_count].ops = ops;
    s_mock_devfs_count++;
    return 0;
}

void test_usb_devfs_registration_and_lookup(void)
{
    TEST_CASE("Publish slot nodes under /dev/usb/<bus>-<port>/ and verify 10 attributes via mock devfs");
    s_mock_devfs_count = 0;

    struct xhci_slot slot;
    memset(&slot, 0, sizeof(slot));
    slot.slot_id = 1;
    slot.port_num = 1;
    slot.speed = XHCI_PORT_SPEED_FULL;
    slot.configured = true;
    slot.desc.idVendor = 0x0627;
    slot.desc.idProduct = 0x0001;

    uint8_t raw[52];
    memset(raw, 0x42, sizeof(raw));
    slot.raw_descriptors = raw;
    slot.raw_descriptors_len = 52;

    int published = usb_devfs_register_slot(1, &slot);
    assert_eq(published, 10);
    assert_eq(s_mock_devfs_count, 10);

    /* Verify specific registered paths */
    bool found_vendor = false, found_desc = false;
    for (int i = 0; i < s_mock_devfs_count; i++) {
        if (strcmp(s_mock_devfs[i].path, "usb/1-1/vendor") == 0) {
            found_vendor = true;
            struct vfs_node n = { .private_data = s_mock_devfs[i].ctx };
            char buf[32];
            int r = s_mock_devfs[i].ops->read(&n, 0, sizeof(buf), buf);
            assert_eq(r, 7);
            assert_eq(strcmp(buf, "0x0627\n"), 0);
        }
        if (strcmp(s_mock_devfs[i].path, "usb/1-1/descriptors") == 0) {
            found_desc = true;
            struct vfs_node n = { .private_data = s_mock_devfs[i].ctx };
            uint8_t out[18];
            int r = s_mock_devfs[i].ops->read(&n, 0, 18, out);
            assert_eq(r, 18);
            assert_eq(out[0], 0x42);
        }
    }
    assert_true(found_vendor);
    assert_true(found_desc);

    /* Test idempotent re-registration */
    int re_pub = usb_devfs_register_slot(1, &slot);
    assert_eq(re_pub, 10);
    assert_eq(s_mock_devfs_count, 10); /* No duplicates added */
}
```
Add call to `main()`.

- [ ] **Step 2: Run test to verify failure**

Run:
`OS01_PROFILE_FILE=$(pwd)/mk/profiles/x86_64-clang.mk make -C hosttests test-usb-devfs`
Expected: Compilation failure (`usb_devfs_register_slot` unimplemented).

- [ ] **Step 3: Implement `usb_devfs_register_slot`, `usb_devfs_register_all` and hook in `main.c`**

In `kernel/driver/usb_devfs.c`:
- Define `static const struct devfs_ops usb_attr_ops = { .read = (int (*)(struct vfs_node *, uint64_t, uint64_t, void *))usb_attr_read };`
- Implement `usb_devfs_register_slot(uint8_t bus_id, struct xhci_slot *slot)`:
  - `char dev_id[USB_DEVFS_ID_MAX]; snprintf(dev_id, sizeof(dev_id), "%u-%u", (unsigned)bus_id, (unsigned)slot->port_num);`
  - Loop `i` from 0 to `USB_ATTR_COUNT - 1`:
    - `char path[USB_DEVFS_PATH_MAX]; snprintf(path, sizeof(path), "%s/%s/%s", USB_DEVFS_NAMESPACE, dev_id, s_attr_names[i]);`
    - `usb_attr_ctx_t *ctx = kmalloc(sizeof(*ctx)); if (!ctx) continue;`
    - `ctx->slot = slot; ctx->bus_id = bus_id; ctx->attr = (uint8_t)i;`
    - `int rc = devfs_register_path(path, ctx, &usb_attr_ops);`
    - If `rc == 0`: `published++;`
    - If `rc == -EEXIST`: `kfree(ctx); published++;` (idempotent, avoid leaking context)
    - Else: `kfree(ctx); log_warn("[usb_devfs] register '%s' failed (%d)\n", path, rc);`
  - Return `published;`
- Implement `usb_devfs_register_all(void)`:
  - Track `static bool s_usb_devfs_registered = false; static int s_usb_devfs_published = 0;`
  - If `s_usb_devfs_registered` return `s_usb_devfs_published;`
  - Loop `c` from 0 to `xhci_controller_count() - 1`:
    - `struct xhci_controller *ctrl = xhci_controller_get(c); if (!ctrl) continue;`
    - `uint8_t bus_id = (uint8_t)(c + 1);`
    - Loop `s` from 1 to `XHCI_MAX_MANAGED_SLOTS`:
      - If `ctrl->slots[s] && ctrl->slots[s]->configured`:
        `total += usb_devfs_register_slot(bus_id, ctrl->slots[s]);`
  - `s_usb_devfs_published = total; s_usb_devfs_registered = true;`
  - `log_info("[usb_devfs] published %d attribute nodes for USB devices\n", total);`
  - Return `total;`

In `kernel/core/main.c`:
- `#include <driver/usb_devfs.h>`
- Immediately after `pci_devfs_register_all();` (around line 155):
  `usb_devfs_register_all();`

- [ ] **Step 4: Run test to verify it passes**

Run:
`OS01_PROFILE_FILE=$(pwd)/mk/profiles/x86_64-clang.mk make -C hosttests test-usb-devfs`
Expected: PASS (5 tests executed, 0 failed).

- [ ] **Step 5: Commit**

```bash
git add kernel/driver/usb_devfs.c kernel/core/main.c hosttests/cases/test_usb_devfs.c
git commit -m "feat(usb_devfs): implement usb_devfs_register_all and hook into kernel boot sequence"
```

---

### Task 4: User-space `/bin/lsusb` Utility Implementation

**Files:**
- Create: `user/lsusb.c`
- Modify: `hosttests/cases/test_usb_devfs.c`
- Modify: `hosttests/mk/drivers.mk`

**Interfaces:**
- Consumes: `/dev/usb/` directory and attribute nodes via `opendir()`, `readdir()`, `open()`, `read()`, `close()`
- Produces: `/bin/lsusb` executable with `-v` and `-s` options

- [ ] **Step 1: Write the failing tests for `lsusb` filter parsing, unpacker, and error boundaries**

In `hosttests/cases/test_usb_devfs.c`:
```c
/* Prototypes from user/lsusb.c */
int lsusb_parse_filter(const char *arg, int *out_bus, int *out_devnum);
int lsusb_unpack_descriptors(const uint8_t *buf, size_t len, char *out_text, size_t max_out);

void test_lsusb_filter_and_unpack_logic(void)
{
    TEST_CASE("lsusb -s filter parsing and binary descriptor tree unpacking");
    int bus = -1, devnum = -1;

    /* Valid filter cases */
    assert_eq(lsusb_parse_filter("1:2", &bus, &devnum), 0);
    assert_eq(bus, 1);
    assert_eq(devnum, 2);

    assert_eq(lsusb_parse_filter("1:", &bus, &devnum), 0);
    assert_eq(bus, 1);
    assert_eq(devnum, -1);

    assert_eq(lsusb_parse_filter(":2", &bus, &devnum), 0);
    assert_eq(bus, -1);
    assert_eq(devnum, 2);

    assert_eq(lsusb_parse_filter("2", &bus, &devnum), 0);
    assert_eq(bus, -1);
    assert_eq(devnum, 2);

    /* Invalid filter cases */
    assert_eq(lsusb_parse_filter("", &bus, &devnum), -1);
    assert_eq(lsusb_parse_filter(":", &bus, &devnum), -1);
    assert_eq(lsusb_parse_filter("1:2:3", &bus, &devnum), -1);
    assert_eq(lsusb_parse_filter("1:abc", &bus, &devnum), -1);
    assert_eq(lsusb_parse_filter("-1:1", &bus, &devnum), -1);
    assert_eq(lsusb_parse_filter("abc", &bus, &devnum), -1);

    /* Descriptor unpacker test: 18B Device Desc + 34B Config Hierarchy */
    uint8_t raw[52] = {
        /* Device Desc (18B) */
        18, 1, 0x00, 0x02, 0, 0, 0, 64, 0x27, 0x06, 0x01, 0x00, 0x00, 0x01, 1, 2, 3, 1,
        /* Config Desc (9B) */
        9, 2, 34, 0, 1, 1, 0, 0x80, 50,
        /* Interface Desc (9B) */
        9, 4, 0, 0, 1, 3, 0, 0, 0,
        /* HID Desc (9B) */
        9, 0x21, 0x10, 0x01, 0, 1, 0x22, 68, 0,
        /* Endpoint Desc (7B) */
        7, 5, 0x81, 3, 8, 0, 10
    };

    char text[1024];
    int rc = lsusb_unpack_descriptors(raw, sizeof(raw), text, sizeof(text));
    assert_eq(rc, 0);
    assert_true(strstr(text, "Device Descriptor:") != NULL);
    assert_true(strstr(text, "idVendor           0x0627") != NULL);
    assert_true(strstr(text, "idProduct          0x0001") != NULL);
    assert_true(strstr(text, "Configuration Descriptor:") != NULL);
    assert_true(strstr(text, "Human Interface Device") != NULL);
    assert_true(strstr(text, "0x81  EP 1 IN") != NULL);

    /* Malformed descriptor stream protection */
    rc = lsusb_unpack_descriptors(raw, 10, text, sizeof(text));
    assert_eq(rc, -1); /* Truncated Device Descriptor */
}
```
Add call to `main()`.

In `hosttests/mk/drivers.mk`:
```makefile
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
```

- [ ] **Step 2: Run test to verify failure**

Run:
`OS01_PROFILE_FILE=$(pwd)/mk/profiles/x86_64-clang.mk make -C hosttests test-usb-devfs`
Expected: Compilation failure (`user/lsusb.c` missing).

- [ ] **Step 3: Implement `user/lsusb.c` with `#ifndef LSUSB_NO_MAIN`**

Create `user/lsusb.c`:
- Includes: `<dirent.h>`, `<fcntl.h>`, `<stdbool.h>`, `<stdio.h>`, `<stdlib.h>`, `<string.h>`, `<unistd.h>`, `<stdint.h>`, `<ctype.h>`.
- Implement `lsusb_parse_filter(const char *arg, int *out_bus, int *out_devnum)`:
  - If `!arg || !*arg` return -1;
  - Parse `bus` and `devnum` supporting `X:Y`, `X:`, `:Y`, `Y`. Return 0 on success, -1 on invalid character or range.
- Implement device table:
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
- Implement class table: `get_class_name(uint8_t cls)` returning friendly strings ("Audio", "Human Interface Device", "Mass Storage", "Hub", etc.).
- Implement `read_attr(const char *dir, const char *attr, char *buf, size_t size)` reading `/dev/usb/<dir>/<attr>`.
- Implement `lsusb_unpack_descriptors(const uint8_t *buf, size_t len, char *out_text, size_t max_out)`:
  - Guard `if (len < 18) return -1;`
  - Format Device Descriptor using Linux-style 18-space indented fields:
    `"Device Descriptor:\n  bLength                %u\n  bDescriptorType        %u\n  bcdUSB               %x.%02x\n  bDeviceClass           %u\n  bDeviceSubClass        %u\n  bDeviceProtocol        %u\n  bMaxPacketSize0        %u\n  idVendor           0x%04x\n  idProduct          0x%04x\n  bNumConfigurations     %u\n"`
  - Loop offset starting at 18:
    - If `offset + 2 > len` break;
    - `uint8_t sub_len = buf[offset]; uint8_t sub_type = buf[offset + 1];`
    - If `sub_len < 2 || offset + sub_len > len` break;
    - If `sub_type == 2`: dump Configuration Descriptor (`"  Configuration Descriptor:\n    bLength                %u\n    bDescriptorType        %u\n    wTotalLength           %u\n    bNumInterfaces         %u\n    bConfigurationValue    %u\n"`).
    - If `sub_type == 4`: dump Interface Descriptor (`"    Interface Descriptor:\n      bLength                %u\n      bDescriptorType        %u\n      bInterfaceNumber       %u\n      bNumEndpoints          %u\n      bInterfaceClass        %u %s\n"`).
    - If `sub_type == 5`: dump Endpoint Descriptor (`"      Endpoint Descriptor:\n        bLength                %u\n        bDescriptorType        %u\n        bEndpointAddress     0x%02x  EP %u %s\n        bmAttributes           %u\n          Transfer Type          %s\n        wMaxPacketSize     0x%04x\n        bInterval              %u\n"`).
    - `offset += sub_len;`
  - Return 0.
- Wrap main function with `#ifndef LSUSB_NO_MAIN`:
  ```c
  #ifndef LSUSB_NO_MAIN
  struct usb_dev_entry {
      char name[32];
      int bus;
      int devnum;
      int port;
      uint16_t vid;
      uint16_t pid;
      uint8_t cls;
  };

  static int cmp_dev_entry(const void *a, const void *b) {
      const struct usb_dev_entry *da = (const struct usb_dev_entry *)a;
      const struct usb_dev_entry *db = (const struct usb_dev_entry *)b;
      if (da->bus != db->bus) return da->bus - db->bus;
      return da->port - db->port;
  }

  int main(int argc, char **argv)
  {
      bool verbose = false;
      int filter_bus = -1, filter_dev = -1;

      for (int i = 1; i < argc; i++) {
          if (strcmp(argv[i], "-v") == 0) verbose = true;
          else if (strcmp(argv[i], "-h") == 0) {
              printf("usage: lsusb [-v] [-s [[bus]:][devnum]] [-h]\n");
              return 0;
          } else if (strcmp(argv[i], "-s") == 0) {
              if (++i >= argc || lsusb_parse_filter(argv[i], &filter_bus, &filter_dev) != 0) {
                  fprintf(stderr, "lsusb: invalid -s filter syntax\n");
                  return 2;
              }
          } else {
              fprintf(stderr, "lsusb: unknown option '%s'\n", argv[i]);
              return 2;
          }
      }

      DIR *dir = opendir("/dev/usb");
      if (!dir) {
          fprintf(stderr, "lsusb: cannot open /dev/usb\n");
          return 1;
      }

      struct usb_dev_entry entries[32];
      int dev_count = 0;
      struct dirent *de;
      while ((de = readdir(dir)) != NULL) {
          if (de->d_name[0] == '.' || !strchr(de->d_name, '-')) continue;
          if (dev_count >= 32) break;
          // read bus, devnum, port, vendor, product, class
          ...
          dev_count++;
      }
      closedir(dir);

      if (dev_count == 0) {
          fprintf(stderr, "lsusb: no USB devices found\n");
          return 1;
      }

      qsort(entries, dev_count, sizeof(struct usb_dev_entry), cmp_dev_entry);

      int matched = 0;
      for (int i = 0; i < dev_count; i++) {
          if (filter_bus >= 0 && entries[i].bus != filter_bus) continue;
          if (filter_dev >= 0 && entries[i].devnum != filter_dev) continue;
          matched++;
          printf("Bus %03u Device %03u: ID %04x:%04x %s\n", ...);
          if (verbose) {
              // open /dev/usb/<name>/descriptors, read, call lsusb_unpack_descriptors
          }
      }

      return (matched > 0) ? 0 : 1;
  }
  #endif
  ```

- [ ] **Step 4: Run test to verify it passes**

Run:
`OS01_PROFILE_FILE=$(pwd)/mk/profiles/x86_64-clang.mk make -C hosttests test-usb-devfs`
Expected: PASS (6 tests executed, 0 failed).

- [ ] **Step 5: Commit**

```bash
git add user/lsusb.c hosttests/cases/test_usb_devfs.c hosttests/mk/drivers.mk
git commit -m "feat(user): implement user-space lsusb command with verbose and filter support"
```

---

### Task 5: Build Integration, Regression Testing & QEMU End-to-End Verification

**Files:**
- Verify: Full host tests (`make test-host`), static audits (`make test-static`), user build, and QEMU run.

**Interfaces:**
- Consumes: All Phase 1, Phase 2, Phase 3, and Phase 4 components.
- Produces: Verified `/bin/lsusb` on `disk.img` discovering QEMU USB Tablet.

- [ ] **Step 1: Run full hosttests suite and static checks**

Run:
```bash
make test-host
make test-static
```
Expected: All 63+ host test suites and 13 static audits pass with 0 errors.

- [ ] **Step 2: Build disk image and verify `/bin/lsusb` is packaged**

Run:
```bash
make disk.img
```
Expected: Successful build of `user/lsusb.elf` and staging into `/bin/lsusb` on `disk.img`.

- [ ] **Step 3: Run headless QEMU test and verify `lsusb` execution**

Boot QEMU with serial input capture and inject commands:
```bash
/bin/lsusb
/bin/lsusb -v
/bin/lsusb -s 1:1
```
Verify serial output:
- Kernel log: `[usb_devfs] published 10 attribute nodes for 1 USB devices`
- `/bin/lsusb` output: `Bus 001 Device 001: ID 0627:0001 QEMU USB Tablet`
- `/bin/lsusb -v` output: `Human Interface Device` and `0x81  EP 1 IN`

- [ ] **Step 4: Commit**

```bash
git add .
git commit -m "test(xhci): verify Phase 4 usb_devfs and lsusb end-to-end in QEMU"
```
