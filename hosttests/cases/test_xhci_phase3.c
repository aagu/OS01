/* hosttests/cases/test_xhci_phase3.c — xHCI Host Controller Phase 3 unit tests */
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

/* The xHCI driver is compiled against the hosttest runtime mock, where every
 * MMIO store is redirected to mock_xhci_write32().  Phase 3 only touches the
 * doorbell array, so a plain 32-bit store is enough. */
void mock_xhci_write32(volatile void *addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
}

#define MAX_MOCK_PAGES 64
static void *s_mock_page_ptrs[MAX_MOCK_PAGES];
static int s_mock_page_count = 0;
static int s_inject_alloc_fail_countdown = -1;

uint64_t alloc_4k_page(void)
{
    if (s_inject_alloc_fail_countdown == 0) {
        return 0;
    }
    if (s_inject_alloc_fail_countdown > 0) {
        s_inject_alloc_fail_countdown--;
    }
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

void test_xhci_phase3_structs_and_offsets(void)
{
    TEST_CASE("Descriptor sizes and Input/Device Context endpoint offsets");
    assert_eq(sizeof(struct usb_config_descriptor), 9);
    assert_eq(sizeof(struct usb_interface_descriptor), 9);
    assert_eq(sizeof(struct usb_endpoint_descriptor), 7);
    assert_eq(sizeof(struct usb_hid_descriptor), 9);

    uint8_t buffer[4096];
    memset(buffer, 0, sizeof(buffer));

    /* CSZ = 0: 32 bytes per entry */
    /* Input Context: index 0 = Control, index 1 = Slot, index 2 = EP0 (DCI 1), index 4 = EP1 IN (DCI 3) */
    void *input_ep1_in_32 = xhci_input_ep_ctx(buffer, 0, 3);
    assert_eq((uintptr_t)input_ep1_in_32 - (uintptr_t)buffer, (1 + 3) * 32);

    /* Device Context: index 0 = Slot, index 1 = EP0, index 3 = EP1 IN (DCI 3) */
    void *dev_ep1_in_32 = xhci_dev_ep_ctx(buffer, 0, 3);
    assert_eq((uintptr_t)dev_ep1_in_32 - (uintptr_t)buffer, 3 * 32);

    /* CSZ = 1: 64 bytes per entry */
    void *input_ep1_in_64 = xhci_input_ep_ctx(buffer, 1, 3);
    assert_eq((uintptr_t)input_ep1_in_64 - (uintptr_t)buffer, (1 + 3) * 64);
}

void test_xhci_dynamic_ep_rings_lifecycle_and_oom_rollback(void)
{
    TEST_CASE("Dynamic ep_rings lifecycle and OOM rollback");
    struct xhci_slot slot;
    memset(&slot, 0, sizeof(slot));

    int base_pages = s_mock_page_count;

    /* Simulate normal dynamic allocation for max_dci = 3 */
    slot.ep_rings = kmalloc(sizeof(struct xhci_ep_ring *) * (3 + 1));
    assert_true(slot.ep_rings != NULL);
    slot.max_dci = 3;

    struct xhci_ep_ring *ring3 = kmalloc(sizeof(struct xhci_ep_ring));
    ring3->ring_phys = alloc_4k_page();
    ring3->ring_virt = (struct xhci_trb *)Phy_To_Virt(ring3->ring_phys);
    ring3->dci = 3;
    ring3->active = true;
    slot.ep_rings[3] = ring3;

    assert_eq(s_mock_page_count, base_pages + 1);

    /* Free all endpoint rings */
    xhci_free_endpoint_rings(&slot);
    assert_true(slot.ep_rings == NULL);
    assert_eq(slot.max_dci, 0);
    assert_eq(s_mock_page_count, base_pages);
}

void test_xhci_parse_config_descriptor_hierarchy(void)
{
    TEST_CASE("Parse valid USB Configuration Descriptor hierarchy (Tablet HID)");
    struct xhci_slot slot;
    memset(&slot, 0, sizeof(slot));
    slot.speed = XHCI_PORT_SPEED_FULL;

    /* Raw descriptor hierarchy for QEMU usb-tablet:
     * Config (9B): total_len=34, 1 iface, config_val=1
     * Interface (9B): iface=0, alt=0, eps=1, class=3 (HID), subclass=0, proto=0
     * HID (9B): desc_len=9, type=0x21
     * Endpoint (7B): addr=0x81 (EP1 IN -> DCI 3), attr=3 (Intr), mps=8, interval=10
     */
    uint8_t raw_desc[34] = {
        /* Config */
        9, USB_DESC_TYPE_CONFIGURATION, 34, 0, 1, 1, 0, 0x80, 50,
        /* Interface */
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, 3, 0, 0, 0,
        /* HID */
        9, USB_DESC_TYPE_HID, 0x10, 0x01, 0, 1, 0x22, 68, 0,
        /* Endpoint */
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 3, 8, 0, 10
    };

    int rc = xhci_setup_endpoint_rings(&slot, raw_desc, sizeof(raw_desc));
    assert_eq(rc, 0);
    assert_eq(slot.max_dci, 3);
    assert_true(slot.iface.active);
    assert_eq(slot.iface.class_code, 3);
    assert_eq(slot.iface.num_endpoints, 1);

    assert_true(slot.ep_rings != NULL);
    assert_true(slot.ep_rings[3] != NULL);
    assert_eq(slot.ep_rings[3]->dci, 3);
    assert_eq(slot.ep_rings[3]->ep_type, XHCI_EP_TYPE_INTR_IN);
    assert_eq(slot.ep_rings[3]->max_packet_size, 8);
    /* For Full-Speed Interrupt with bInterval=10, 2^(6-3) = 8ms <= 10ms -> interval=6 */
    assert_eq(slot.ep_rings[3]->interval, 6);
    assert_true(slot.ep_rings[3]->ring_phys != 0);

    /* Verify Link TRB initialized at index 255 */
    struct xhci_trb *link = &slot.ep_rings[3]->ring_virt[XHCI_TRB_RING_SIZE - 1];
    assert_eq((link->control >> 10) & 0x3F, XHCI_TRB_TYPE_LINK);
    assert_true((link->control & XHCI_TRB_LINK_TC) != 0);

    xhci_free_endpoint_rings(&slot);
}

void test_xhci_parse_corrupt_descriptor_guards(void)
{
    TEST_CASE("Defensive guards against corrupt descriptors");
    struct xhci_slot slot;
    memset(&slot, 0, sizeof(slot));

    /* Guard 1: Sub-descriptor len underflow (len < 7 for endpoint) */
    uint8_t underflow_desc[16] = {
        9, USB_DESC_TYPE_CONFIGURATION, 16, 0, 1, 1, 0, 0x80, 50,
        5, USB_DESC_TYPE_ENDPOINT, 0x81, 3, 8, 0, 10 /* len=5 is < 7 */
    };
    int max_dci = xhci_parse_config_descriptor(&slot, underflow_desc, sizeof(underflow_desc));
    assert_eq(max_dci, 1); /* Ignored corrupt endpoint */

    /* Guard 2: ep_num == 0 (bEndpointAddress = 0x80) */
    uint8_t ep0_desc[16] = {
        9, USB_DESC_TYPE_CONFIGURATION, 16, 0, 1, 1, 0, 0x80, 50,
        7, USB_DESC_TYPE_ENDPOINT, 0x80, 3, 8, 0, 10 /* ep_num=0 is invalid for non-EP0 */
    };
    max_dci = xhci_parse_config_descriptor(&slot, ep0_desc, sizeof(ep0_desc));
    assert_eq(max_dci, 1);

    /* Guard 3: OOM during ring allocation rolls back without leak */
    uint8_t valid_desc[34] = {
        9, USB_DESC_TYPE_CONFIGURATION, 34, 0, 1, 1, 0, 0x80, 50,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, 3, 0, 0, 0,
        9, USB_DESC_TYPE_HID, 0x10, 0x01, 0, 1, 0x22, 68, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 3, 8, 0, 10
    };
    int base_pages = s_mock_page_count;
    s_inject_alloc_fail_countdown = 0; /* Fail next alloc_4k_page */
    int rc = xhci_setup_endpoint_rings(&slot, valid_desc, sizeof(valid_desc));
    assert_eq(rc, -ENOMEM);
    assert_true(slot.ep_rings == NULL);
    assert_eq(s_mock_page_count, base_pages); /* Clean rollback */
    s_inject_alloc_fail_countdown = -1;
}

int main(void)
{
    TEST_SUITE("xhci_phase3");
    test_xhci_phase3_structs_and_offsets();
    test_xhci_dynamic_ep_rings_lifecycle_and_oom_rollback();
    test_xhci_parse_config_descriptor_hierarchy();
    test_xhci_parse_corrupt_descriptor_guards();
    TEST_RESULTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
