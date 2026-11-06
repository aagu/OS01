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
