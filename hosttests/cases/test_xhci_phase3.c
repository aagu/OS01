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

void test_xhci_get_config_descriptor_two_stage(void)
{
    TEST_CASE("Two-stage GET_DESCRIPTOR (9B header then full 34B body) with short-tx bounds");
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));

    struct xhci_slot slot;
    memset(&slot, 0, sizeof(slot));
    slot.slot_id = 1;
    slot.speed = XHCI_PORT_SPEED_FULL;

    struct xhci_trb ep0_ring[XHCI_TRB_RING_SIZE];
    memset(ep0_ring, 0, sizeof(ep0_ring));
    slot.ep0_ring_virt = ep0_ring;
    slot.ep0_ring_phys = 0x40000;
    slot.ep0_ring_cycle = 1;
    slot.ep0_ring_enqueue = 0;

    uint8_t dma_buf[4096];
    memset(dma_buf, 0, sizeof(dma_buf));
    slot.dma_buf_virt = dma_buf;
    slot.dma_buf_phys = 0x50000;

    ctrl.slots[1] = &slot;

    struct xhci_trb event_ring[XHCI_TRB_RING_SIZE];
    memset(event_ring, 0, sizeof(event_ring));
    ctrl.event_ring_virt = event_ring;
    ctrl.event_ring_phys = 0x20000;
    ctrl.event_ring_dequeue = 0;
    ctrl.event_ring_cycle = 1;

    uint8_t rt_regs[0x100];
    memset(rt_regs, 0, sizeof(rt_regs));
    ctrl.rt_regs = rt_regs;

    uint8_t db_regs[0x1000];
    memset(db_regs, 0, sizeof(db_regs));
    ctrl.db_regs = db_regs;

    /* Mock response for Stage 1 (9B header): wTotalLength = 34 */
    struct usb_config_descriptor raw_cfg = {
        .bLength = 9,
        .bDescriptorType = USB_DESC_TYPE_CONFIGURATION,
        .wTotalLength = 34,
        .bNumInterfaces = 1,
        .bConfigurationValue = 1,
    };
    memcpy(dma_buf, &raw_cfg, 9);

    /* Event 0: Transfer Event for Stage 1 (Status TRB @ 0x40000 + 2*16) */
    event_ring[0].parameter = 0x40000 + 2 * sizeof(struct xhci_trb);
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[0].control = (1U << 24) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    /* Event 1: Transfer Event for Stage 2 (Status TRB @ 0x40000 + 5*16) */
    event_ring[1].parameter = 0x40000 + 5 * sizeof(struct xhci_trb);
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[1].control = (1U << 24) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    struct usb_config_descriptor cfg;
    int bytes_read = xhci_get_config_descriptor(&ctrl, 1, &cfg);
    assert_eq(bytes_read, 34);
    assert_eq(cfg.bLength, 9);
    assert_eq(cfg.bDescriptorType, USB_DESC_TYPE_CONFIGURATION);
    assert_eq(cfg.wTotalLength, 34);
    assert_eq(cfg.bConfigurationValue, 1);

    /* Verify Stage 1 Setup TRB wLength = 9 */
    assert_eq((ep0_ring[0].control >> 10) & 0x3F, XHCI_TRB_TYPE_SETUP_STAGE);
    assert_eq((ep0_ring[0].parameter >> 48) & 0xFFFF, 9);

    /* Verify Stage 2 Setup TRB wLength = 34 */
    assert_eq((ep0_ring[3].control >> 10) & 0x3F, XHCI_TRB_TYPE_SETUP_STAGE);
    assert_eq((ep0_ring[3].parameter >> 48) & 0xFFFF, 34);

    /* Boundary Test 1: Stage 2 SHORT_TX with residual = 4 (34 - 4 = 30 bytes) */
    event_ring[2].parameter = 0x40000 + 8 * sizeof(struct xhci_trb);
    event_ring[2].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[2].control = (1U << 24) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[3].parameter = 0x40000 + 11 * sizeof(struct xhci_trb);
    event_ring[3].status = (XHCI_COMP_SHORT_TX << 24) | 4; /* Residual = 4 */
    event_ring[3].control = (1U << 24) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    bytes_read = xhci_get_config_descriptor(&ctrl, 1, &cfg);
    assert_eq(bytes_read, 30);

    /* Boundary Test 2: Stage 2 actual < 9 bytes (e.g. residual = 28 -> actual = 6) returns -EIO */
    event_ring[4].parameter = 0x40000 + 14 * sizeof(struct xhci_trb);
    event_ring[4].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[4].control = (1U << 24) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[5].parameter = 0x40000 + 17 * sizeof(struct xhci_trb);
    event_ring[5].status = (XHCI_COMP_SHORT_TX << 24) | 28; /* Residual = 28 */
    event_ring[5].control = (1U << 24) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    bytes_read = xhci_get_config_descriptor(&ctrl, 1, &cfg);
    assert_eq(bytes_read, -EIO);

    /* Boundary Test 3: Residual underflow (residual = 50 > 34) returns -EIO */
    event_ring[6].parameter = 0x40000 + 20 * sizeof(struct xhci_trb);
    event_ring[6].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[6].control = (1U << 24) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[7].parameter = 0x40000 + 23 * sizeof(struct xhci_trb);
    event_ring[7].status = (XHCI_COMP_SUCCESS << 24) | 50;
    event_ring[7].control = (1U << 24) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    bytes_read = xhci_get_config_descriptor(&ctrl, 1, &cfg);
    assert_eq(bytes_read, -EIO);

    /* Boundary Test 4: Header wTotalLength > 4096 returns -EINVAL */
    raw_cfg.wTotalLength = 5000;
    memcpy(dma_buf, &raw_cfg, 9);
    event_ring[8].parameter = 0x40000 + 26 * sizeof(struct xhci_trb);
    event_ring[8].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[8].control = (1U << 24) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    bytes_read = xhci_get_config_descriptor(&ctrl, 1, &cfg);
    assert_eq(bytes_read, -EINVAL);
}

void test_xhci_configure_endpoint_input_context(void)
{
    TEST_CASE("Configure Endpoint: Input Control Context Add Flags and EP Context DWs");
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));

    struct xhci_slot slot;
    memset(&slot, 0, sizeof(slot));
    slot.slot_id = 1;
    slot.port_num = 1;
    slot.speed = XHCI_PORT_SPEED_FULL;
    slot.max_dci = 3;

    uint8_t input_ctx[4096];
    memset(input_ctx, 0, sizeof(input_ctx));
    slot.input_ctx_virt = input_ctx;
    slot.input_ctx_phys = 0x35000;

    slot.ep_rings = kmalloc(sizeof(struct xhci_ep_ring *) * 4);
    struct xhci_ep_ring ring3 = {
        .ring_phys = 0x48000,
        .dci = 3,
        .ep_type = XHCI_EP_TYPE_INTR_IN,
        .max_packet_size = 8,
        .interval = 6,
        .active = true,
    };
    slot.ep_rings[3] = &ring3;
    ctrl.slots[1] = &slot;

    struct xhci_trb cmd_ring[XHCI_TRB_RING_SIZE];
    memset(cmd_ring, 0, sizeof(cmd_ring));
    ctrl.cmd_ring_virt = cmd_ring;
    ctrl.cmd_ring_phys = 0x10000;
    ctrl.cmd_ring_enqueue = 0;
    ctrl.cmd_ring_cycle = 1;

    struct xhci_trb event_ring[XHCI_TRB_RING_SIZE];
    memset(event_ring, 0, sizeof(event_ring));
    ctrl.event_ring_virt = event_ring;
    ctrl.event_ring_phys = 0x20000;
    ctrl.event_ring_dequeue = 0;
    ctrl.event_ring_cycle = 1;

    uint8_t rt_regs[0x100];
    memset(rt_regs, 0, sizeof(rt_regs));
    ctrl.rt_regs = rt_regs;

    uint8_t db_regs[0x1000];
    memset(db_regs, 0, sizeof(db_regs));
    ctrl.db_regs = db_regs;

    /* Feed mock Command Completion Event for Configure Endpoint */
    event_ring[0].parameter = 0x10000;
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[0].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    int rc = xhci_configure_endpoints(&ctrl, 1);
    assert_eq(rc, 0);

    /* Verify Input Control Context: Add Flags = bit 0 (Slot) | bit 3 (EP1 IN) = 0x09 */
    uint32_t *ctrl_ctx = (uint32_t *)xhci_input_ctrl_ctx(slot.input_ctx_virt, ctrl.csz);
    assert_eq(ctrl_ctx[0], 0);             /* Drop Flags = 0 */
    assert_eq(ctrl_ctx[1], (1U << 0) | (1U << 3)); /* Add Flags */

    /* Verify Slot Context: Context Entries = 3 */
    uint32_t *slot_ctx = (uint32_t *)xhci_input_slot_ctx(slot.input_ctx_virt, ctrl.csz);
    assert_eq((slot_ctx[0] >> 27) & 0x1F, 3);

    /* Verify Endpoint 3 Context DWs */
    uint32_t *ep_ctx = (uint32_t *)xhci_input_ep_ctx(slot.input_ctx_virt, ctrl.csz, 3);
    assert_eq((ep_ctx[0] >> 16) & 0xFF, 6); /* Interval = 6 */
    assert_eq((ep_ctx[1] >> 1) & 3, 3);     /* CErr = 3 */
    assert_eq((ep_ctx[1] >> 3) & 7, 7);     /* EP Type = 7 (Interrupt IN) */
    assert_eq((ep_ctx[1] >> 16) & 0xFFFF, 8);/* Max Packet Size = 8 */
    assert_eq(ep_ctx[2] & 1U, 1U);          /* DCS = 1 */
    assert_eq(ep_ctx[2] & 0xFFFFFFF0U, 0x48000); /* Base phys lo */

    /* Verify Command Ring submission: Type 12 */
    assert_eq((cmd_ring[0].control >> 10) & 0x3F, XHCI_TRB_TYPE_CONFIG_EP);
    assert_eq((cmd_ring[0].control >> 24) & 0xFF, 1); /* Slot ID */

    kfree(slot.ep_rings);
}

int main(void)
{
    TEST_SUITE("xhci_phase3");
    test_xhci_phase3_structs_and_offsets();
    test_xhci_dynamic_ep_rings_lifecycle_and_oom_rollback();
    test_xhci_parse_config_descriptor_hierarchy();
    test_xhci_parse_corrupt_descriptor_guards();
    test_xhci_get_config_descriptor_two_stage();
    test_xhci_configure_endpoint_input_context();
    TEST_RESULTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
