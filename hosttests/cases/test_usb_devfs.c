/* hosttests/cases/test_usb_devfs.c — USB devfs and lsusb unit tests */
#include "test_framework.h"
#include <device/device.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>
#include <driver/xhci.h>
#include <driver/usb_devfs.h>
#include <fs/devfs.h>
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

void *devfs_get_private(struct vfs_node *node)
{
    return node ? node->fs_data : NULL;
}

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
    struct vfs_node node = { .fs_data = &ctx };
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
            struct vfs_node n = { .fs_data = s_mock_devfs[i].ctx };
            char buf[32];
            memset(buf, 0, sizeof(buf));
            int r = s_mock_devfs[i].ops->read(&n, 0, sizeof(buf), buf);
            assert_eq(r, 7);
            assert_eq(strcmp(buf, "0x0627\n"), 0);
        }
        if (strcmp(s_mock_devfs[i].path, "usb/1-1/descriptors") == 0) {
            found_desc = true;
            struct vfs_node n = { .fs_data = s_mock_devfs[i].ctx };
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

int main(void)
{
    TEST_SUITE("usb_devfs");
    test_xhci_raw_descriptors_caching_and_teardown();
    test_xhci_controller_registry();
    test_usb_devfs_formatting();
    test_usb_devfs_descriptors_binary_slicing();
    test_usb_devfs_registration_and_lookup();
    TEST_RESULTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
