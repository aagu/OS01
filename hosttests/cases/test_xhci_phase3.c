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
static volatile uint32_t *s_mock_portsc_ptr = NULL;
static bool s_port_pr_seen = false;
static bool s_port_wpr_seen = false;
static struct xhci_controller *s_mock_db_ctrl = NULL;
static int s_mock_ep0_db_count = 0;
static int s_mock_evt_write_idx = 0;

void mock_xhci_write32(volatile void *addr, uint32_t val)
{
    if (s_mock_portsc_ptr && addr == s_mock_portsc_ptr) {
        uint32_t cur = *s_mock_portsc_ptr;
        uint32_t ro = XHCI_PORTSC_CCS | XHCI_PORTSC_SPEED_MASK;
        uint32_t rw1c_cleared = cur & ~(val & XHCI_PORTSC_RW1C_MASK);
        uint32_t updated = (cur & ro) |
                           (val & ~XHCI_PORTSC_RW1C_MASK & ~ro) |
                           (rw1c_cleared & XHCI_PORTSC_RW1C_MASK);
        if (val & XHCI_PORTSC_PR) {
            s_port_pr_seen = true;
            updated &= ~XHCI_PORTSC_PR;
            updated |= XHCI_PORTSC_PRC | XHCI_PORTSC_PED;
            updated = (updated & ~XHCI_PORTSC_PLS_MASK) | (XHCI_PLS_U0 << 5);
        }
        if (val & XHCI_PORTSC_WPR) {
            s_port_wpr_seen = true;
            updated &= ~XHCI_PORTSC_WPR;
            updated |= XHCI_PORTSC_WRC | XHCI_PORTSC_PED;
            updated = (updated & ~XHCI_PORTSC_PLS_MASK) | (XHCI_PLS_U0 << 5);
        }
        *s_mock_portsc_ptr = updated;
        return;
    }
    if (s_mock_db_ctrl && addr == (volatile void *)(s_mock_db_ctrl->db_regs + 4) && val == 1) {
        s_mock_ep0_db_count++;
        struct xhci_slot *s = s_mock_db_ctrl->slots[1];
        if (s && s->ep0_ring_phys) {
            uint32_t enq = s->ep0_ring_enqueue;
            if (s_mock_ep0_db_count == 1) {
                /* GET_DESCRIPTOR(DEVICE): Data @ enq-2, Status @ enq-1 */
                s_mock_db_ctrl->event_ring_virt[s_mock_evt_write_idx].parameter =
                    s->ep0_ring_phys + (enq - 2) * sizeof(struct xhci_trb);
                s_mock_db_ctrl->event_ring_virt[s_mock_evt_write_idx].status = (XHCI_COMP_SUCCESS << 24);
                s_mock_db_ctrl->event_ring_virt[s_mock_evt_write_idx++].control =
                    (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
                s_mock_db_ctrl->event_ring_virt[s_mock_evt_write_idx].parameter =
                    s->ep0_ring_phys + (enq - 1) * sizeof(struct xhci_trb);
                s_mock_db_ctrl->event_ring_virt[s_mock_evt_write_idx].status = (XHCI_COMP_SUCCESS << 24);
                s_mock_db_ctrl->event_ring_virt[s_mock_evt_write_idx++].control =
                    (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
            } else if (s_mock_ep0_db_count == 2 || s_mock_ep0_db_count == 3) {
                if (s->dma_buf_virt) {
                    struct usb_config_descriptor cfg = {
                        .bLength = 9,
                        .bDescriptorType = USB_DESC_TYPE_CONFIGURATION,
                        .wTotalLength = 100,
                        .bNumInterfaces = 1,
                        .bConfigurationValue = 1,
                    };
                    memcpy(s->dma_buf_virt, &cfg, sizeof(cfg));
                }
                uint32_t data_st = (s_mock_ep0_db_count == 3)
                    ? ((XHCI_COMP_SHORT_TX << 24) | 68) : (XHCI_COMP_SUCCESS << 24);
                s_mock_db_ctrl->event_ring_virt[s_mock_evt_write_idx].parameter =
                    s->ep0_ring_phys + (enq - 2) * sizeof(struct xhci_trb);
                s_mock_db_ctrl->event_ring_virt[s_mock_evt_write_idx].status = data_st;
                s_mock_db_ctrl->event_ring_virt[s_mock_evt_write_idx++].control =
                    (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
                s_mock_db_ctrl->event_ring_virt[s_mock_evt_write_idx].parameter =
                    s->ep0_ring_phys + (enq - 1) * sizeof(struct xhci_trb);
                s_mock_db_ctrl->event_ring_virt[s_mock_evt_write_idx].status = (XHCI_COMP_SUCCESS << 24);
                s_mock_db_ctrl->event_ring_virt[s_mock_evt_write_idx++].control =
                    (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
            } else if (s_mock_ep0_db_count == 4) {
                /* SET_CONFIGURATION: Status @ enq-1 (skip index 8 used by Configure Endpoint) */
                if (s_mock_evt_write_idx == 8) s_mock_evt_write_idx = 9;
                s_mock_db_ctrl->event_ring_virt[s_mock_evt_write_idx].parameter =
                    s->ep0_ring_phys + (enq - 1) * sizeof(struct xhci_trb);
                s_mock_db_ctrl->event_ring_virt[s_mock_evt_write_idx].status = (XHCI_COMP_SUCCESS << 24);
                s_mock_db_ctrl->event_ring_virt[s_mock_evt_write_idx++].control =
                    (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
            }
        }
    }
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

    /* Guard 4: a duplicated endpoint descriptor must not create a second ring */
    uint8_t dup_desc[41] = {
        9, USB_DESC_TYPE_CONFIGURATION, 41, 0, 1, 1, 0, 0x80, 50,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, 3, 0, 0, 0,
        9, USB_DESC_TYPE_HID, 0x10, 0x01, 0, 1, 0x22, 68, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 3, 8, 0, 10,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 3, 8, 0, 10
    };
    rc = xhci_setup_endpoint_rings(&slot, dup_desc, sizeof(dup_desc));
    assert_eq(rc, 0);
    assert_eq(slot.max_dci, 3);
    assert_true(slot.ep_rings != NULL);
    assert_true(slot.ep_rings[3] != NULL);
    assert_true(slot.ep_rings[2] == NULL);
    xhci_free_endpoint_rings(&slot);

    /* Guard 5: an isochronous endpoint is left unconfigured */
    uint8_t isoch_desc[34] = {
        9, USB_DESC_TYPE_CONFIGURATION, 34, 0, 1, 1, 0, 0x80, 50,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, 3, 0, 0, 0,
        9, USB_DESC_TYPE_HID, 0x10, 0x01, 0, 1, 0x22, 68, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 1, 8, 0, 10 /* attr = 1 (isochronous) */
    };
    rc = xhci_setup_endpoint_rings(&slot, isoch_desc, sizeof(isoch_desc));
    assert_eq(rc, 0);
    assert_true(slot.ep_rings != NULL);
    assert_true(slot.ep_rings[3] == NULL); /* no ring for the isoch endpoint */
    xhci_free_endpoint_rings(&slot);
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

    /* Event 0+1: Transfer Events for Stage 1 (Data TRB @ +1*16, Status TRB @ +2*16) */
    event_ring[0].parameter = 0x40000 + 1 * sizeof(struct xhci_trb);
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[0].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[1].parameter = 0x40000 + 2 * sizeof(struct xhci_trb);
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[1].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    /* Event 2+3: Transfer Events for Stage 2 (Data TRB @ +4*16, Status TRB @ +5*16) */
    event_ring[2].parameter = 0x40000 + 4 * sizeof(struct xhci_trb);
    event_ring[2].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[2].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[3].parameter = 0x40000 + 5 * sizeof(struct xhci_trb);
    event_ring[3].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[3].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

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
    /* The Setup Stage carries the 8-byte setup packet as immediate data. */
    assert_true((ep0_ring[0].control & (1U << 6)) != 0); /* IDT = 1 */
    assert_eq(ep0_ring[0].status & 0x1FFFF, 8);
    /* The Data Stage names the DMA buffer and its length. */
    assert_eq((ep0_ring[1].control >> 10) & 0x3F, XHCI_TRB_TYPE_DATA_STAGE);
    assert_true((ep0_ring[1].control & (1U << 16)) != 0); /* DIR = 1 (IN) */
    assert_eq(ep0_ring[1].parameter, 0x50000);
    assert_eq(ep0_ring[1].status & 0x1FFFF, 9);

    /* Verify Stage 2 Setup TRB wLength = 34 */
    assert_eq((ep0_ring[3].control >> 10) & 0x3F, XHCI_TRB_TYPE_SETUP_STAGE);
    assert_eq((ep0_ring[3].parameter >> 48) & 0xFFFF, 34);
    assert_eq(ep0_ring[4].status & 0x1FFFF, 34);

    /* Boundary Test 1: Stage 2 SHORT_TX with residual = 4 (34 - 4 = 30 bytes) */
    event_ring[4].parameter = 0x40000 + 7 * sizeof(struct xhci_trb);
    event_ring[4].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[4].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    event_ring[5].parameter = 0x40000 + 8 * sizeof(struct xhci_trb);
    event_ring[5].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[5].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[6].parameter = 0x40000 + 10 * sizeof(struct xhci_trb);
    event_ring[6].status = (XHCI_COMP_SHORT_TX << 24) | 4; /* Residual = 4 */
    event_ring[6].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    event_ring[7].parameter = 0x40000 + 11 * sizeof(struct xhci_trb);
    event_ring[7].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[7].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    bytes_read = xhci_get_config_descriptor(&ctrl, 1, &cfg);
    assert_eq(bytes_read, 30);

    /* Boundary Test 2: Stage 2 actual < 9 bytes (e.g. residual = 28 -> actual = 6) returns -EIO */
    event_ring[8].parameter = 0x40000 + 13 * sizeof(struct xhci_trb);
    event_ring[8].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[8].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    event_ring[9].parameter = 0x40000 + 14 * sizeof(struct xhci_trb);
    event_ring[9].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[9].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[10].parameter = 0x40000 + 16 * sizeof(struct xhci_trb);
    event_ring[10].status = (XHCI_COMP_SHORT_TX << 24) | 28; /* Residual = 28 */
    event_ring[10].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    event_ring[11].parameter = 0x40000 + 17 * sizeof(struct xhci_trb);
    event_ring[11].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[11].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    bytes_read = xhci_get_config_descriptor(&ctrl, 1, &cfg);
    assert_eq(bytes_read, -EIO);

    /* Boundary Test 3: Residual underflow (residual = 50 > 34) returns -EIO */
    event_ring[12].parameter = 0x40000 + 19 * sizeof(struct xhci_trb);
    event_ring[12].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[12].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    event_ring[13].parameter = 0x40000 + 20 * sizeof(struct xhci_trb);
    event_ring[13].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[13].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[14].parameter = 0x40000 + 22 * sizeof(struct xhci_trb);
    event_ring[14].status = (XHCI_COMP_SUCCESS << 24) | 50;
    event_ring[14].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    bytes_read = xhci_get_config_descriptor(&ctrl, 1, &cfg);
    assert_eq(bytes_read, -EIO);

    /* Boundary Test 4: Header wTotalLength > 4096 returns -EINVAL */
    raw_cfg.wTotalLength = 5000;
    memcpy(dma_buf, &raw_cfg, 9);
    event_ring[15].parameter = 0x40000 + 25 * sizeof(struct xhci_trb);
    event_ring[15].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[15].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    event_ring[16].parameter = 0x40000 + 26 * sizeof(struct xhci_trb);
    event_ring[16].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[16].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

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
    assert_eq((slot_ctx[0] >> 20) & 0xF, XHCI_PORT_SPEED_FULL); /* Speed */
    assert_eq((slot_ctx[1] >> 16) & 0xFF, 1);                    /* Root Hub Port Number */

    /* Verify Endpoint 3 Context DWs */
    uint32_t *ep_ctx = (uint32_t *)xhci_input_ep_ctx(slot.input_ctx_virt, ctrl.csz, 3);
    assert_eq((ep_ctx[0] >> 16) & 0xFF, 6); /* Interval = 6 */
    assert_eq((ep_ctx[1] >> 1) & 3, 3);     /* CErr = 3 */
    assert_eq((ep_ctx[1] >> 3) & 7, 7);     /* EP Type = 7 (Interrupt IN) */
    assert_eq((ep_ctx[1] >> 16) & 0xFFFF, 8);/* Max Packet Size = 8 */
    assert_eq(ep_ctx[2] & 1U, 1U);          /* DCS = 1 */
    assert_eq(ep_ctx[2] & 0xFFFFFFF0U, 0x48000); /* Base phys lo */
    assert_eq(ep_ctx[3], 0);                /* TR Dequeue Hi */
    assert_eq(ep_ctx[4], 8);                /* Average TRB Length */

    /* Verify Command Ring submission: Type 12 */
    assert_eq((cmd_ring[0].control >> 10) & 0x3F, XHCI_TRB_TYPE_CONFIG_EP);
    assert_eq((cmd_ring[0].control >> 24) & 0xFF, 1); /* Slot ID */

    kfree(slot.ep_rings);

    /* An EP0-only device still gets the Configure Endpoint command: it is
     * the command that moves the slot from Addressed to Configured. */
    struct xhci_slot ep0_slot;
    memset(&ep0_slot, 0, sizeof(ep0_slot));
    ep0_slot.slot_id = 2;
    ep0_slot.port_num = 1;
    ep0_slot.speed = XHCI_PORT_SPEED_FULL;
    ep0_slot.max_dci = 0;

    uint8_t ep0_input[4096];
    memset(ep0_input, 0, sizeof(ep0_input));
    ep0_slot.input_ctx_virt = ep0_input;
    ep0_slot.input_ctx_phys = 0x36000;
    ctrl.slots[2] = &ep0_slot;

    /* Command completion event for the second command (TRB at index 1) */
    event_ring[1].parameter = 0x10000 + sizeof(struct xhci_trb);
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[1].control = (2U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    rc = xhci_configure_endpoints(&ctrl, 2);
    assert_eq(rc, 0);
    assert_eq((cmd_ring[1].control >> 10) & 0x3F, XHCI_TRB_TYPE_CONFIG_EP);

    uint32_t *ep0_ctrl_ctx = (uint32_t *)xhci_input_ctrl_ctx(ep0_input, ctrl.csz);
    assert_eq(ep0_ctrl_ctx[1], 1U << 0); /* Add Flags: Slot Context only */
    uint32_t *ep0_slot_ctx = (uint32_t *)xhci_input_slot_ctx(ep0_input, ctrl.csz);
    assert_eq((ep0_slot_ctx[0] >> 27) & 0x1F, 1); /* Context Entries = 1 */
}

void test_xhci_set_configuration_2_stage_transfer(void)
{
    TEST_CASE("SET_CONFIGURATION 2-stage zero-length control transfer (Setup TRT=0, Status DIR=1)");
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));

    struct xhci_slot slot;
    memset(&slot, 0, sizeof(slot));
    slot.slot_id = 1;

    struct xhci_trb ep0_ring[XHCI_TRB_RING_SIZE];
    memset(ep0_ring, 0, sizeof(ep0_ring));
    slot.ep0_ring_virt = ep0_ring;
    slot.ep0_ring_phys = 0x40000;
    slot.ep0_ring_cycle = 1;
    slot.ep0_ring_enqueue = 0;

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

    /* Feed mock Transfer Event for Status TRB @ 0x40000 + 1*16 (DCI=1) */
    event_ring[0].parameter = 0x40000 + 1 * sizeof(struct xhci_trb);
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[0].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    int rc = xhci_set_configuration(&ctrl, 1, 1);
    assert_eq(rc, 0);
    assert_true(slot.configured);

    /* Verify Setup Stage TRB: TRT = 0 (No Data), IDT = 1, bRequest = 9, wValue = 1 */
    assert_eq((ep0_ring[0].control >> 10) & 0x3F, XHCI_TRB_TYPE_SETUP_STAGE);
    assert_eq((ep0_ring[0].control >> 16) & 3, 0);        /* TRT = 0 */
    assert_true((ep0_ring[0].control & (1U << 6)) != 0); /* IDT = 1 */
    assert_eq(ep0_ring[0].parameter & 0xFFFF, (0x09U << 8)); /* bRequest=9, bmRequestType=0 */
    assert_eq((ep0_ring[0].parameter >> 16) & 0xFFFF, 1);    /* wValue=1 */

    /* Verify Status Stage TRB: DIR = 1 (IN), IOC = 1 */
    assert_eq((ep0_ring[1].control >> 10) & 0x3F, XHCI_TRB_TYPE_STATUS_STAGE);
    assert_true((ep0_ring[1].control & (1U << 16)) != 0); /* DIR = 1 */
    assert_true((ep0_ring[1].control & (1U << 5)) != 0);  /* IOC = 1 */
}

static void setup_mock_ep0_ctrl(struct xhci_controller *ctrl, struct xhci_slot *slot,
                                struct xhci_trb *ep0_ring, struct xhci_trb *event_ring,
                                uint8_t *rt_regs, uint8_t *db_regs)
{
    memset(ctrl, 0, sizeof(*ctrl));
    memset(slot, 0, sizeof(*slot));
    memset(ep0_ring, 0, sizeof(*ep0_ring) * XHCI_TRB_RING_SIZE);
    memset(event_ring, 0, sizeof(*event_ring) * XHCI_TRB_RING_SIZE);
    memset(rt_regs, 0, 0x100);
    memset(db_regs, 0, 0x1000);

    slot->slot_id = 1;
    slot->speed = XHCI_PORT_SPEED_HIGH;
    slot->ep0_ring_virt = ep0_ring;
    slot->ep0_ring_phys = 0x40000;
    slot->ep0_ring_cycle = 1;
    slot->ep0_ring_enqueue = 0;

    ep0_ring[XHCI_TRB_RING_SIZE - 1].parameter = 0x40000;
    ep0_ring[XHCI_TRB_RING_SIZE - 1].control = (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_LINK_TC;

    ctrl->slots[1] = slot;
    ctrl->event_ring_virt = event_ring;
    ctrl->event_ring_phys = 0x20000;
    ctrl->event_ring_dequeue = 0;
    ctrl->event_ring_cycle = 1;
    ctrl->rt_regs = rt_regs;
    ctrl->db_regs = db_regs;
}

void test_xhci_msc_alt_zero_pair(void)
{
    TEST_CASE("msc_alt_zero_pair: active alt 0 MSC bulk pair, guards, and SET_CONFIGURATION gating");
    struct xhci_controller ctrl;
    struct xhci_slot slot;
    struct xhci_trb ep0_ring[XHCI_TRB_RING_SIZE];
    struct xhci_trb event_ring[XHCI_TRB_RING_SIZE];
    uint8_t rt_regs[0x100];
    uint8_t db_regs[0x1000];

    /* Case 1: Nonzero interface number (iface 0 = HID, iface 1 alt 0 = MSC 08/06/50)
     * plus inactive alt 1 reusing DCI 3 (0x81) with MPS=64 instead of 512. */
    setup_mock_ep0_ctrl(&ctrl, &slot, ep0_ring, event_ring, rt_regs, db_regs);
    uint8_t desc1[] = {
        /* Config (9B): total=64, 2 ifaces */
        9, USB_DESC_TYPE_CONFIGURATION, 64, 0, 2, 1, 0, 0x80, 50,
        /* Interface 0 alt 0: HID */
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, 3, 0, 0, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x83, 3, 8, 0, 10,
        /* Interface 1 alt 0: MSC 08/06/50, Bulk IN 0x81 (MPS 512), Bulk OUT 0x02 (MPS 512) */
        9, USB_DESC_TYPE_INTERFACE, 1, 0, 2, 0x08, 0x06, 0x50, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 2, 0x00, 0x02, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x02, 2, 0x00, 0x02, 0,
        /* Interface 1 alt 1 (inactive): reuses 0x81 with MPS=64 */
        9, USB_DESC_TYPE_INTERFACE, 1, 1, 1, 0x08, 0x06, 0x50, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 2, 64, 0, 0,
    };
    int rc = xhci_setup_endpoint_rings(&slot, desc1, sizeof(desc1));
    assert_eq(rc, 0);
    /* Not published before SET_CONFIGURATION */
    assert_false(slot.msc_iface.active);
    /* Inactive alt 1 must NOT overwrite active DCI 3 MPS (512) */
    assert_true(slot.ep_rings[3] != NULL);
    assert_eq(slot.ep_rings[3]->max_packet_size, 512);

    /* Failed SET_CONFIGURATION (STALL) must leave msc_iface inactive */
    event_ring[0].parameter = 0x40000 + 1 * sizeof(struct xhci_trb);
    event_ring[0].status = (XHCI_COMP_STALL << 24);
    event_ring[0].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    rc = xhci_set_configuration(&ctrl, 1, 1);
    assert_true(rc != 0);
    assert_false(slot.msc_iface.active);
    assert_false(slot.configured);

    /* Successful SET_CONFIGURATION publishes msc_iface */
    event_ring[1].parameter = 0x40000 + 3 * sizeof(struct xhci_trb);
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[1].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    rc = xhci_set_configuration(&ctrl, 1, 1);
    assert_eq(rc, 0);
    assert_true(slot.msc_iface.active);
    assert_eq(slot.msc_iface.interface_num, 1);
    assert_eq(slot.msc_iface.in_addr, 0x81);
    assert_eq(slot.msc_iface.in_dci, 3);
    assert_eq(slot.msc_iface.out_addr, 0x02);
    assert_eq(slot.msc_iface.out_dci, 4);
    xhci_free_endpoint_rings(&slot);
    assert_false(slot.msc_iface.active);

    /* Case 2: Two complete MSC interfaces -> first complete wins */
    setup_mock_ep0_ctrl(&ctrl, &slot, ep0_ring, event_ring, rt_regs, db_regs);
    uint8_t desc2[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 55, 0, 2, 1, 0, 0x80, 50,
        9, USB_DESC_TYPE_INTERFACE, 1, 0, 2, 0x08, 0x06, 0x50, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 2, 0x00, 0x02, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x02, 2, 0x00, 0x02, 0,
        9, USB_DESC_TYPE_INTERFACE, 2, 0, 2, 0x08, 0x06, 0x50, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x83, 2, 0x00, 0x02, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x04, 2, 0x00, 0x02, 0,
    };
    assert_eq(xhci_setup_endpoint_rings(&slot, desc2, sizeof(desc2)), 0);
    event_ring[0].parameter = 0x40000 + 1 * sizeof(struct xhci_trb);
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[0].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    assert_eq(xhci_set_configuration(&ctrl, 1, 1), 0);
    assert_true(slot.msc_iface.active);
    assert_eq(slot.msc_iface.interface_num, 1);
    assert_eq(slot.msc_iface.in_addr, 0x81);
    assert_eq(slot.msc_iface.in_dci, 3);
    assert_eq(slot.msc_iface.out_addr, 0x02);
    assert_eq(slot.msc_iface.out_dci, 4);
    xhci_free_endpoint_rings(&slot);

    /* Case 3: Incomplete first MSC interface + complete second -> second wins */
    setup_mock_ep0_ctrl(&ctrl, &slot, ep0_ring, event_ring, rt_regs, db_regs);
    uint8_t desc3[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 48, 0, 2, 1, 0, 0x80, 50,
        /* Interface 0: MSC with only IN */
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, 0x08, 0x06, 0x50, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x84, 2, 0x00, 0x02, 0,
        /* Interface 2: complete MSC */
        9, USB_DESC_TYPE_INTERFACE, 2, 0, 2, 0x08, 0x06, 0x50, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 2, 0x00, 0x02, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x02, 2, 0x00, 0x02, 0,
    };
    assert_eq(xhci_setup_endpoint_rings(&slot, desc3, sizeof(desc3)), 0);
    event_ring[0].parameter = 0x40000 + 1 * sizeof(struct xhci_trb);
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[0].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    assert_eq(xhci_set_configuration(&ctrl, 1, 1), 0);
    assert_true(slot.msc_iface.active);
    assert_eq(slot.msc_iface.interface_num, 2);
    assert_eq(slot.msc_iface.in_addr, 0x81);
    assert_eq(slot.msc_iface.in_dci, 3);
    assert_eq(slot.msc_iface.out_addr, 0x02);
    assert_eq(slot.msc_iface.out_dci, 4);
    xhci_free_endpoint_rings(&slot);

    /* Case 4: IN/OUT split across two interfaces -> no cross-interface pair */
    setup_mock_ep0_ctrl(&ctrl, &slot, ep0_ring, event_ring, rt_regs, db_regs);
    uint8_t desc4[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 41, 0, 2, 1, 0, 0x80, 50,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, 0x08, 0x06, 0x50, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 2, 0x00, 0x02, 0,
        9, USB_DESC_TYPE_INTERFACE, 1, 0, 1, 0x08, 0x06, 0x50, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x02, 2, 0x00, 0x02, 0,
    };
    assert_eq(xhci_setup_endpoint_rings(&slot, desc4, sizeof(desc4)), 0);
    event_ring[0].parameter = 0x40000 + 1 * sizeof(struct xhci_trb);
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[0].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    assert_eq(xhci_set_configuration(&ctrl, 1, 1), 0);
    assert_false(slot.msc_iface.active);
    xhci_free_endpoint_rings(&slot);

    /* Case 5: SuperSpeed missing companion -> ineligible; with valid companion -> eligible */
    setup_mock_ep0_ctrl(&ctrl, &slot, ep0_ring, event_ring, rt_regs, db_regs);
    slot.speed = XHCI_PORT_SPEED_SUPER;
    uint8_t desc5_no_comp[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 32, 0, 1, 1, 0, 0x80, 50,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 2, 0x08, 0x06, 0x50, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 2, 0x00, 0x04, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x02, 2, 0x00, 0x04, 0,
    };
    assert_eq(xhci_setup_endpoint_rings(&slot, desc5_no_comp, sizeof(desc5_no_comp)), 0);
    event_ring[0].parameter = 0x40000 + 1 * sizeof(struct xhci_trb);
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[0].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    assert_eq(xhci_set_configuration(&ctrl, 1, 1), 0);
    assert_false(slot.msc_iface.active);
    xhci_free_endpoint_rings(&slot);

    setup_mock_ep0_ctrl(&ctrl, &slot, ep0_ring, event_ring, rt_regs, db_regs);
    slot.speed = XHCI_PORT_SPEED_SUPER;
    uint8_t desc5_comp[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 44, 0, 1, 1, 0, 0x80, 50,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 2, 0x08, 0x06, 0x50, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 2, 0x00, 0x04, 0,
        6, USB_DESC_TYPE_SS_EP_COMPANION, 3, 0, 0, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x02, 2, 0x00, 0x04, 0,
        6, USB_DESC_TYPE_SS_EP_COMPANION, 3, 0, 0, 0,
    };
    assert_eq(xhci_setup_endpoint_rings(&slot, desc5_comp, sizeof(desc5_comp)), 0);
    event_ring[0].parameter = 0x40000 + 1 * sizeof(struct xhci_trb);
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[0].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    assert_eq(xhci_set_configuration(&ctrl, 1, 1), 0);
    assert_true(slot.msc_iface.active);
    assert_eq(slot.ep_rings[3]->max_burst, 3);
    assert_eq(slot.ep_rings[4]->max_burst, 3);
    xhci_free_endpoint_rings(&slot);

    /* Case 6: Truncated config descriptor reply in xhci_probe_ports bounds raw_descriptors_len */
    uint8_t op_regs[0x500];
    memset(op_regs, 0, sizeof(op_regs));
    struct xhci_trb cmd_ring[XHCI_TRB_RING_SIZE];
    memset(cmd_ring, 0, sizeof(cmd_ring));
    uint64_t dcbaa[64];
    memset(dcbaa, 0, sizeof(dcbaa));

    memset(&ctrl, 0, sizeof(ctrl));
    memset(event_ring, 0, sizeof(event_ring));
    memset(rt_regs, 0, sizeof(rt_regs));
    memset(db_regs, 0, sizeof(db_regs));

    ctrl.op_regs = op_regs;
    ctrl.max_ports = 1;
    ctrl.cmd_ring_virt = cmd_ring;
    ctrl.cmd_ring_phys = 0x10000;
    ctrl.cmd_ring_enqueue = 0;
    ctrl.cmd_ring_cycle = 1;
    ctrl.event_ring_virt = event_ring;
    ctrl.event_ring_phys = 0x20000;
    ctrl.event_ring_dequeue = 0;
    ctrl.event_ring_cycle = 1;
    ctrl.rt_regs = rt_regs;
    ctrl.db_regs = db_regs;
    ctrl.dcbaa_virt = dcbaa;
    ctrl.dcbaa_phys = 0x30000;

    volatile uint32_t *port1 = (volatile uint32_t *)(op_regs + 0x400);
    *port1 = XHCI_PORTSC_CCS | XHCI_PORTSC_PP | (XHCI_PORT_SPEED_HIGH << 10);
    s_mock_portsc_ptr = port1;
    s_mock_db_ctrl = &ctrl;
    s_mock_ep0_db_count = 0;
    s_mock_evt_write_idx = 2;

    /* Event 0: Enable Slot -> slot_id = 1 */
    event_ring[0].parameter = 0x10000;
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[0].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;
    /* Event 1: Address Device */
    event_ring[1].parameter = 0x10000 + sizeof(struct xhci_trb);
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[1].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;
    /* Events 2..7 are filled dynamically on EP0 doorbell writes; Event 8 is Configure Endpoint */
    event_ring[8].parameter = 0x10000 + 2 * sizeof(struct xhci_trb);
    event_ring[8].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[8].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;
    s_mock_evt_write_idx = 2;
    /* Note: when db_count==3 finishes, evt_write_idx is 8, so put Configure Endpoint at 8 and
     * advance s_mock_evt_write_idx past 8 in mock, OR simply put Configure Endpoint at 8 and
     * have SET_CONFIGURATION (db_count==4) write to 9! */
    xhci_probe_ports(&ctrl);
    s_mock_portsc_ptr = NULL;
    s_mock_db_ctrl = NULL;

    assert_true(ctrl.slots[1] != NULL);
    assert_eq(ctrl.slots[1]->raw_descriptors_len, sizeof(struct usb_device_descriptor) + 32);
    xhci_free_slot(&ctrl, 1);
}

void test_xhci_bulk_event_identity(void)
{
    TEST_CASE("bulk_event_identity: slot/DCI/TRB match, 64K short IN, invalid residual, short OUT, 65537/unaligned");
    struct xhci_controller ctrl;
    struct xhci_slot slot;
    struct xhci_trb ep0_ring[XHCI_TRB_RING_SIZE];
    struct xhci_trb event_ring[XHCI_TRB_RING_SIZE];
    uint8_t rt_regs[0x100];
    uint8_t db_regs[0x1000];
    setup_mock_ep0_ctrl(&ctrl, &slot, ep0_ring, event_ring, rt_regs, db_regs);
    slot.active = true;
    slot.configured = true;

    struct xhci_trb bulk_in_ring_trbs[XHCI_TRB_RING_SIZE];
    struct xhci_trb bulk_out_ring_trbs[XHCI_TRB_RING_SIZE];
    memset(bulk_in_ring_trbs, 0, sizeof(bulk_in_ring_trbs));
    memset(bulk_out_ring_trbs, 0, sizeof(bulk_out_ring_trbs));

    struct xhci_ep_ring in_ring = {
        .ring_phys = 0x60000, .ring_virt = bulk_in_ring_trbs,
        .enqueue = 0, .cycle = 1, .dci = 3, .ep_type = XHCI_EP_TYPE_BULK_IN,
        .max_packet_size = 512, .active = true,
    };
    struct xhci_ep_ring out_ring = {
        .ring_phys = 0x70000, .ring_virt = bulk_out_ring_trbs,
        .enqueue = 0, .cycle = 1, .dci = 4, .ep_type = XHCI_EP_TYPE_BULK_OUT,
        .max_packet_size = 512, .active = true,
    };
    struct xhci_ep_ring *rings[5] = { NULL, NULL, NULL, &in_ring, &out_ring };
    slot.ep_rings = rings;
    slot.max_dci = 4;

    void *staging = aligned_alloc(65536, 65536);
    assert_true(staging != NULL);
    memset(staging, 0xA5, 65536);
    slot.bulk_staging_virt = staging;
    slot.bulk_staging_phys = (uint64_t)(uintptr_t)staging;

    uint8_t *cpu_buf = malloc(65537);
    assert_true(cpu_buf != NULL);

    /* 1. Wrong slot (2), wrong DCI (4), wrong TRB ptr, then true event: len=65536, residual=512 -> 65024 */
    event_ring[0].parameter = 0x60000;
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[0].control = (2U << 24) | (3U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[1].parameter = 0x60000;
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[1].control = (1U << 24) | (4U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[2].parameter = 0xDEADBEEFULL;
    event_ring[2].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[2].control = (1U << 24) | (3U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[3].parameter = 0x60000;
    event_ring[3].status = (XHCI_COMP_SHORT_TX << 24) | 512;
    event_ring[3].control = (1U << 24) | (3U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    size_t actual = 1234;
    int rc = xhci_bulk_transfer(&ctrl, 1, 3, cpu_buf, 65536, &actual);
    assert_eq(rc, 0);
    assert_eq(actual, 65024);
    assert_eq(ctrl.event_ring_dequeue, 4);

    /* 2. Invalid residual (> len) rejected */
    event_ring[4].parameter = 0x60000 + 1 * sizeof(struct xhci_trb);
    event_ring[4].status = (XHCI_COMP_SHORT_TX << 24) | 1000;
    event_ring[4].control = (1U << 24) | (3U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    rc = xhci_bulk_transfer(&ctrl, 1, 3, cpu_buf, 512, &actual);
    assert_true(rc < 0);
    assert_eq(actual, 0);

    /* 3. Short OUT rejected */
    event_ring[5].parameter = 0x70000;
    event_ring[5].status = (XHCI_COMP_SUCCESS << 24) | 16;
    event_ring[5].control = (1U << 24) | (4U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    rc = xhci_bulk_transfer(&ctrl, 1, 4, cpu_buf, 512, &actual);
    assert_true(rc < 0);
    assert_eq(actual, 0);

    /* 4. len = 65537 rejected */
    rc = xhci_bulk_transfer(&ctrl, 1, 3, cpu_buf, 65537, &actual);
    assert_eq(rc, -EINVAL);

    /* 5. Unaligned DMA staging window rejected */
    slot.bulk_staging_phys += 4096;
    rc = xhci_bulk_transfer(&ctrl, 1, 3, cpu_buf, 512, &actual);
    assert_true(rc < 0);
    slot.bulk_staging_phys -= 4096;

    free(cpu_buf);
    free(staging);
}

void test_xhci_bulk_wrap_and_boundary(void)
{
    TEST_CASE("bulk_wrap_and_boundary: index 254 -> Link/C=1 -> index 0/C=0");
    struct xhci_controller ctrl;
    struct xhci_slot slot;
    struct xhci_trb ep0_ring[XHCI_TRB_RING_SIZE];
    struct xhci_trb event_ring[XHCI_TRB_RING_SIZE];
    uint8_t rt_regs[0x100];
    uint8_t db_regs[0x1000];
    setup_mock_ep0_ctrl(&ctrl, &slot, ep0_ring, event_ring, rt_regs, db_regs);
    slot.active = true;
    slot.configured = true;

    struct xhci_trb bulk_ring_trbs[XHCI_TRB_RING_SIZE];
    memset(bulk_ring_trbs, 0, sizeof(bulk_ring_trbs));
    bulk_ring_trbs[255].parameter = 0x60000;
    bulk_ring_trbs[255].control = (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_LINK_TC;

    struct xhci_ep_ring in_ring = {
        .ring_phys = 0x60000, .ring_virt = bulk_ring_trbs,
        .enqueue = 254, .cycle = 1, .dci = 3, .ep_type = XHCI_EP_TYPE_BULK_IN,
        .max_packet_size = 512, .active = true,
    };
    struct xhci_ep_ring *rings[4] = { NULL, NULL, NULL, &in_ring };
    slot.ep_rings = rings;
    slot.max_dci = 3;

    void *staging = aligned_alloc(65536, 65536);
    assert_true(staging != NULL);
    slot.bulk_staging_virt = staging;
    slot.bulk_staging_phys = (uint64_t)(uintptr_t)staging;

    uint8_t buf[512];
    event_ring[0].parameter = 0x60000 + 254 * sizeof(struct xhci_trb);
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[0].control = (1U << 24) | (3U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[1].parameter = 0x60000 + 0 * sizeof(struct xhci_trb);
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[1].control = (1U << 24) | (3U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    size_t actual = 0;
    assert_eq(xhci_bulk_transfer(&ctrl, 1, 3, buf, 512, &actual), 0);
    assert_eq(in_ring.enqueue, 255);
    assert_eq(in_ring.cycle, 1);
    assert_eq(bulk_ring_trbs[254].control & XHCI_TRB_CYCLE_BIT, 1);

    assert_eq(xhci_bulk_transfer(&ctrl, 1, 3, buf, 512, &actual), 0);
    assert_eq(bulk_ring_trbs[255].control & XHCI_TRB_CYCLE_BIT, 1);
    assert_eq(in_ring.cycle, 0);
    assert_eq(in_ring.enqueue, 1);
    assert_eq(bulk_ring_trbs[0].control & XHCI_TRB_CYCLE_BIT, 0);

    free(staging);
}

void test_xhci_clear_halt_state_drives_command_sequence(void)
{
    TEST_CASE("clear_halt_state_drives_command_sequence: Halted vs Running/Stopped, wrapped DCS=0, errors");
    struct xhci_controller ctrl;
    struct xhci_slot slot;
    struct xhci_trb ep0_ring[XHCI_TRB_RING_SIZE];
    struct xhci_trb event_ring[XHCI_TRB_RING_SIZE];
    uint8_t rt_regs[0x100];
    uint8_t db_regs[0x1000];
    struct xhci_trb cmd_ring[XHCI_TRB_RING_SIZE];
    uint8_t input_ctx[4096];
    uint8_t dev_ctx[4096];

    void *staging = aligned_alloc(65536, 65536);
    assert_true(staging != NULL);

    /* Case 1: Halted pipe (DCI 3, ep_addr 0x81) with wrapped producer (enqueue=255, cycle=1 -> next TRB 0, DCS=0) */
    setup_mock_ep0_ctrl(&ctrl, &slot, ep0_ring, event_ring, rt_regs, db_regs);
    memset(cmd_ring, 0, sizeof(cmd_ring));
    memset(input_ctx, 0, sizeof(input_ctx));
    memset(dev_ctx, 0, sizeof(dev_ctx));
    ctrl.cmd_ring_virt = cmd_ring;
    ctrl.cmd_ring_phys = 0x10000;
    ctrl.cmd_ring_enqueue = 0;
    ctrl.cmd_ring_cycle = 1;
    slot.active = true;
    slot.configured = true;
    slot.speed = XHCI_PORT_SPEED_HIGH;
    slot.port_num = 1;
    slot.input_ctx_virt = input_ctx;
    slot.input_ctx_phys = 0x35000;
    slot.dev_ctx_virt = dev_ctx;
    slot.dev_ctx_phys = 0x36000;
    slot.bulk_staging_virt = staging;
    slot.bulk_staging_phys = (uint64_t)(uintptr_t)staging;

    struct xhci_trb bulk_in_trbs[XHCI_TRB_RING_SIZE];
    struct xhci_trb bulk_out_trbs[XHCI_TRB_RING_SIZE];
    memset(bulk_in_trbs, 0, sizeof(bulk_in_trbs));
    memset(bulk_out_trbs, 0, sizeof(bulk_out_trbs));
    bulk_in_trbs[255].parameter = 0x60000;
    bulk_in_trbs[255].control = (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_LINK_TC;

    struct xhci_ep_ring in_ring = {
        .ring_phys = 0x60000, .ring_virt = bulk_in_trbs,
        .enqueue = 255, .cycle = 1, .dci = 3, .ep_type = XHCI_EP_TYPE_BULK_IN,
        .max_packet_size = 512, .active = true,
    };
    struct xhci_ep_ring out_ring = {
        .ring_phys = 0x70000, .ring_virt = bulk_out_trbs,
        .enqueue = 0, .cycle = 1, .dci = 4, .ep_type = XHCI_EP_TYPE_BULK_OUT,
        .max_packet_size = 512, .active = true,
    };
    struct xhci_ep_ring *rings[5] = { NULL, NULL, NULL, &in_ring, &out_ring };
    slot.ep_rings = rings;
    slot.max_dci = 4;

    /* Output Slot Context: Context Entries = 4; Output EP3 Context: State = Halted (2) */
    uint32_t *out_slot_ctx = (uint32_t *)xhci_dev_slot_ctx(dev_ctx, ctrl.csz);
    out_slot_ctx[0] = (XHCI_PORT_SPEED_HIGH << 20) | (4U << 27);
    out_slot_ctx[1] = (1U << 16);
    uint32_t *out_ep3_ctx = (uint32_t *)xhci_dev_ep_ctx(dev_ctx, ctrl.csz, 3);
    out_ep3_ctx[0] = XHCI_EP_STATE_HALTED;

    /* Events: Cmd0 (Reset EP @ 0x10000), EP0 Status (@ 0x40000 + 1*16), Cmd1 (Set TR Deq @ 0x10010) */
    event_ring[0].parameter = 0x10000;
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[0].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    event_ring[1].parameter = 0x40000 + 1 * sizeof(struct xhci_trb);
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[1].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[2].parameter = 0x10000 + sizeof(struct xhci_trb);
    event_ring[2].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[2].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    int rc = xhci_clear_halt(&ctrl, 1, 0x81);
    assert_eq(rc, 0);

    /* Verify Reset Endpoint (type 14, TSP=0, slot=1, ep_id=3) */
    assert_eq((cmd_ring[0].control >> 10) & 0x3F, XHCI_TRB_TYPE_RESET_EP);
    assert_eq((cmd_ring[0].control >> 9) & 1, 0); /* TSP=0 */
    assert_eq((cmd_ring[0].control >> 16) & 0x1F, 3);
    assert_eq((cmd_ring[0].control >> 24) & 0xFF, 1);

    /* Verify EP0 CLEAR_FEATURE(ENDPOINT_HALT): 02/01/0/0x81 */
    assert_eq(ep0_ring[0].parameter, ((uint64_t)0x81 << 32) | (0x01ULL << 8) | 0x02ULL);

    /* Verify Set TR Dequeue Pointer (type 16, slot=1, ep_id=3, param=0x60000 | DCS=0) */
    assert_eq((cmd_ring[1].control >> 10) & 0x3F, XHCI_TRB_TYPE_SET_TR_DEQ);
    assert_eq((cmd_ring[1].control >> 16) & 0x1F, 3);
    assert_eq((cmd_ring[1].control >> 24) & 0xFF, 1);
    assert_eq(cmd_ring[1].parameter, 0x60000ULL); /* DCS=0 */
    assert_eq(in_ring.enqueue, 0);
    assert_eq(in_ring.cycle, 0);

    /* Case 2: Idle Running pipe (DCI 4, ep_addr 0x02) -> EP0 clear + Configure Endpoint (no Reset EP) */
    uint32_t *out_ep4_ctx = (uint32_t *)xhci_dev_ep_ctx(dev_ctx, ctrl.csz, 4);
    out_ep4_ctx[0] = XHCI_EP_STATE_RUNNING;

    /* Event 3: EP0 Status (@ 0x40000 + 3*16), Event 4: Configure Endpoint (@ 0x10020) */
    event_ring[3].parameter = 0x40000 + 3 * sizeof(struct xhci_trb);
    event_ring[3].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[3].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[4].parameter = 0x10000 + 2 * sizeof(struct xhci_trb);
    event_ring[4].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[4].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    rc = xhci_clear_halt(&ctrl, 1, 0x02);
    assert_eq(rc, 0);
    assert_eq((cmd_ring[2].control >> 10) & 0x3F, XHCI_TRB_TYPE_CONFIG_EP);
    uint32_t *in_ctrl_ctx = (uint32_t *)xhci_input_ctrl_ctx(input_ctx, ctrl.csz);
    assert_eq(in_ctrl_ctx[0], (1U << 4));              /* D4=1, D0=0, D1=0 */
    assert_eq(in_ctrl_ctx[1], (1U << 0) | (1U << 4));  /* A0=1, A4=1, A1=0 */
    uint32_t *in_slot_ctx = (uint32_t *)xhci_input_slot_ctx(input_ctx, ctrl.csz);
    assert_eq((in_slot_ctx[0] >> 27) & 0x1F, 4);       /* Retained Context Entries */

    /* Case 3: Stopped pipe (DCI 4, ep_addr 0x02) also uses EP0 clear + Configure Endpoint */
    out_ep4_ctx[0] = XHCI_EP_STATE_STOPPED;
    event_ring[5].parameter = 0x40000 + 5 * sizeof(struct xhci_trb);
    event_ring[5].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[5].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[6].parameter = 0x10000 + 3 * sizeof(struct xhci_trb);
    event_ring[6].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[6].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    rc = xhci_clear_halt(&ctrl, 1, 0x02);
    assert_eq(rc, 0);
    assert_eq((cmd_ring[3].control >> 10) & 0x3F, XHCI_TRB_TYPE_CONFIG_EP);

    /* Case 4: EP0 STALL during clear_halt quarantines slot */
    event_ring[7].parameter = 0x40000 + 7 * sizeof(struct xhci_trb);
    event_ring[7].status = (XHCI_COMP_STALL << 24);
    event_ring[7].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    rc = xhci_clear_halt(&ctrl, 1, 0x02);
    assert_eq(rc, -EPIPE);
    assert_true(slot.poisoned);
    uint8_t dummy[64];
    size_t actual = 0;
    *(volatile uint32_t *)(db_regs + 4) = 0;
    assert_true(xhci_bulk_transfer(&ctrl, 1, 3, dummy, sizeof(dummy), &actual) != 0);
    assert_eq(*(volatile uint32_t *)(db_regs + 4), 0);

    free(staging);
}

void test_xhci_stop_endpoint_recovery(void)
{
    TEST_CASE("stop_endpoint_recovery: Stopped->SetDeq, FSE after late normal, ContextStateErr+Halted, missing FSE");
    struct xhci_controller ctrl;
    struct xhci_slot slot;
    struct xhci_trb ep0_ring[XHCI_TRB_RING_SIZE];
    struct xhci_trb event_ring[XHCI_TRB_RING_SIZE];
    uint8_t rt_regs[0x100];
    uint8_t db_regs[0x1000];
    struct xhci_trb cmd_ring[XHCI_TRB_RING_SIZE];
    uint8_t input_ctx[4096];
    uint8_t dev_ctx[4096];

    void *staging = aligned_alloc(65536, 65536);
    assert_true(staging != NULL);

    struct xhci_trb bulk_in_trbs[XHCI_TRB_RING_SIZE];
    struct xhci_ep_ring in_ring;
    struct xhci_ep_ring *rings[4] = { NULL, NULL, NULL, &in_ring };
    uint8_t buf[64];
    size_t actual = 0;

    /* Helper macro-like setup for each sub-case */
    #define INIT_RECOVERY_CASE() do { \
        setup_mock_ep0_ctrl(&ctrl, &slot, ep0_ring, event_ring, rt_regs, db_regs); \
        memset(cmd_ring, 0, sizeof(cmd_ring)); \
        memset(input_ctx, 0, sizeof(input_ctx)); \
        memset(dev_ctx, 0, sizeof(dev_ctx)); \
        memset(bulk_in_trbs, 0, sizeof(bulk_in_trbs)); \
        ctrl.cmd_ring_virt = cmd_ring; \
        ctrl.cmd_ring_phys = 0x10000; \
        ctrl.cmd_ring_enqueue = 0; \
        ctrl.cmd_ring_cycle = 1; \
        slot.active = true; \
        slot.configured = true; \
        slot.speed = XHCI_PORT_SPEED_HIGH; \
        slot.port_num = 1; \
        slot.input_ctx_virt = input_ctx; \
        slot.input_ctx_phys = 0x35000; \
        slot.dev_ctx_virt = dev_ctx; \
        slot.dev_ctx_phys = 0x36000; \
        slot.bulk_staging_virt = staging; \
        slot.bulk_staging_phys = (uint64_t)(uintptr_t)staging; \
        memset(&in_ring, 0, sizeof(in_ring)); \
        in_ring.ring_phys = 0x60000; \
        in_ring.ring_virt = bulk_in_trbs; \
        in_ring.enqueue = 0; \
        in_ring.cycle = 1; \
        in_ring.dci = 3; \
        in_ring.ep_type = XHCI_EP_TYPE_BULK_IN; \
        in_ring.max_packet_size = 512; \
        in_ring.active = true; \
        slot.ep_rings = rings; \
        slot.max_dci = 3; \
        uint32_t *out_ep3 = (uint32_t *)xhci_dev_ep_ctx(dev_ctx, ctrl.csz, 3); \
        out_ep3[0] = XHCI_EP_STATE_RUNNING; \
    } while (0)

    /* Case 1: Bulk timeout -> Stopped Transfer Event -> Stop EP Success -> Set TR Dequeue (no Reset) */
    INIT_RECOVERY_CASE();
    assert_eq(xhci_bulk_transfer(&ctrl, 1, 3, buf, sizeof(buf), &actual), -ETIMEDOUT);
    assert_true(slot.poisoned);
    assert_true(slot.dma_retained);
    assert_eq(slot.pending_bulk_dci, 3);

    /* Ordinary I/O cannot run against pending DMA */
    *(volatile uint32_t *)(db_regs + 4) = 0;
    assert_true(xhci_bulk_transfer(&ctrl, 1, 3, buf, sizeof(buf), &actual) != 0);
    assert_eq(*(volatile uint32_t *)(db_regs + 4), 0);

    /* Inject Stopped Transfer Event (DCI 3) then Stop EP Cmd Completion (cmd[0] @ 0x10000),
     * then Set TR Deq Cmd Completion (cmd[1] @ 0x10010) */
    event_ring[0].parameter = 0x60000;
    event_ring[0].status = (XHCI_COMP_STOPPED << 24);
    event_ring[0].control = (1U << 24) | (3U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[1].parameter = 0x10000;
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[1].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    event_ring[2].parameter = 0x10010;
    event_ring[2].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[2].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    assert_eq(xhci_recover_bulk_timeout(&ctrl, 1, 3), 0);
    assert_false(slot.poisoned);
    assert_eq(slot.pending_bulk_dci, 0);
    assert_eq((cmd_ring[0].control >> 10) & 0x3F, XHCI_TRB_TYPE_STOP_EP);
    assert_eq((cmd_ring[0].control >> 23) & 1, 0); /* SP=0 */
    assert_eq((cmd_ring[1].control >> 10) & 0x3F, XHCI_TRB_TYPE_SET_TR_DEQ);
    assert_eq(cmd_ring[1].parameter, 0x60010ULL | 1ULL);

    /* Case 2: Old normal completion (SUCCESS) then FSE (Stopped @ next deq) -> Stop EP Success -> Set TR Deq */
    INIT_RECOVERY_CASE();
    assert_eq(xhci_bulk_transfer(&ctrl, 1, 3, buf, sizeof(buf), &actual), -ETIMEDOUT);

    event_ring[0].parameter = 0x60000;
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[0].control = (1U << 24) | (3U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[1].parameter = 0x60010; /* FSE pointing to next dequeue */
    event_ring[1].status = (XHCI_COMP_STOPPED_LENGTH_INVALID << 24);
    event_ring[1].control = (1U << 24) | (3U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[2].parameter = 0x10000;
    event_ring[2].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[2].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    event_ring[3].parameter = 0x10010;
    event_ring[3].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[3].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    assert_eq(xhci_recover_bulk_timeout(&ctrl, 1, 3), 0);
    assert_eq((cmd_ring[0].control >> 10) & 0x3F, XHCI_TRB_TYPE_STOP_EP);
    assert_eq((cmd_ring[1].control >> 10) & 0x3F, XHCI_TRB_TYPE_SET_TR_DEQ);

    /* Case 3: Stop returns Context State Error + Halted output context -> Reset(TSP=0) -> ClearFeature -> Set Deq */
    INIT_RECOVERY_CASE();
    assert_eq(xhci_bulk_transfer(&ctrl, 1, 3, buf, sizeof(buf), &actual), -ETIMEDOUT);
    uint32_t *out_ep3 = (uint32_t *)xhci_dev_ep_ctx(dev_ctx, ctrl.csz, 3);
    out_ep3[0] = XHCI_EP_STATE_HALTED;

    /* Event 0: Stop EP Context State Error (@ 0x10000)
     * Event 1: Reset EP Success (@ 0x10010)
     * Event 2: EP0 CLEAR_FEATURE Status (@ 0x40000 + 1*16)
     * Event 3: Set TR Deq Success (@ 0x10020) */
    event_ring[0].parameter = 0x10000;
    event_ring[0].status = (XHCI_COMP_CONTEXT_STATE_ERR << 24);
    event_ring[0].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    event_ring[1].parameter = 0x10010;
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[1].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    event_ring[2].parameter = 0x40000 + 1 * sizeof(struct xhci_trb);
    event_ring[2].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[2].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[3].parameter = 0x10020;
    event_ring[3].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[3].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    assert_eq(xhci_recover_bulk_timeout(&ctrl, 1, 3), 0);
    assert_eq((cmd_ring[0].control >> 10) & 0x3F, XHCI_TRB_TYPE_STOP_EP);
    assert_eq((cmd_ring[1].control >> 10) & 0x3F, XHCI_TRB_TYPE_RESET_EP);
    assert_eq((cmd_ring[1].control >> 9) & 1, 0); /* TSP=0 */
    assert_eq(ep0_ring[0].parameter, ((uint64_t)0x81 << 32) | (0x01ULL << 8) | 0x02ULL);
    assert_eq((cmd_ring[2].control >> 10) & 0x3F, XHCI_TRB_TYPE_SET_TR_DEQ);

    /* Case 4: Missing FSE (only normal completion or unrelated DCI Stopped before Stop EP Success) -> stays poisoned */
    INIT_RECOVERY_CASE();
    assert_eq(xhci_bulk_transfer(&ctrl, 1, 3, buf, sizeof(buf), &actual), -ETIMEDOUT);

    event_ring[0].parameter = 0x60000;
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24); /* normal completion, NOT FSE */
    event_ring[0].control = (1U << 24) | (3U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[1].parameter = 0x70000;
    event_ring[1].status = (XHCI_COMP_STOPPED << 24); /* unrelated DCI 4 */
    event_ring[1].control = (1U << 24) | (4U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[2].parameter = 0x10000;
    event_ring[2].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[2].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    assert_true(xhci_recover_bulk_timeout(&ctrl, 1, 3) < 0);
    assert_true(slot.poisoned);
    assert_true(slot.dma_retained);
    assert_eq((cmd_ring[1].control >> 10) & 0x3F, 0); /* no Set TR Deq */
    *(volatile uint32_t *)(db_regs + 4) = 0;
    assert_true(xhci_bulk_transfer(&ctrl, 1, 3, buf, sizeof(buf), &actual) != 0);
    assert_eq(*(volatile uint32_t *)(db_regs + 4), 0);

    /* Case 5: Context State Error + EP Error state (not Halted) -> stays poisoned */
    INIT_RECOVERY_CASE();
    assert_eq(xhci_bulk_transfer(&ctrl, 1, 3, buf, sizeof(buf), &actual), -ETIMEDOUT);
    out_ep3 = (uint32_t *)xhci_dev_ep_ctx(dev_ctx, ctrl.csz, 3);
    out_ep3[0] = XHCI_EP_STATE_ERROR;

    event_ring[0].parameter = 0x10000;
    event_ring[0].status = (XHCI_COMP_CONTEXT_STATE_ERR << 24);
    event_ring[0].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    assert_true(xhci_recover_bulk_timeout(&ctrl, 1, 3) < 0);
    assert_true(slot.poisoned);
    assert_true(slot.dma_retained);
    *(volatile uint32_t *)(db_regs + 4) = 0;
    assert_true(xhci_bulk_transfer(&ctrl, 1, 3, buf, sizeof(buf), &actual) != 0);
    assert_eq(*(volatile uint32_t *)(db_regs + 4), 0);

    #undef INIT_RECOVERY_CASE
    free(staging);
}

void test_xhci_port_protocol_reset(void)
{
    TEST_CASE("xhci_port_protocol_reset: USB2 PR, USB3 U0 no-reset, USB3 Inactive WPR, unknown speed, SS bMaxBurst");
    uint8_t mmio[0x1000];
    struct xhci_controller ctrl;
    uint8_t speed = 0;

    /* 1. Supported Protocol parsing: port 1 = USB2, port 2 = USB3 */
    memset(mmio, 0, sizeof(mmio));
    mmio[XHCI_CAP_CAPLENGTH] = 0x20;
    *(uint32_t *)(mmio + XHCI_CAP_HCSPARAMS1) = (2U << 24) | (1U << 8) | 16U;
    *(uint32_t *)(mmio + XHCI_CAP_HCCPARAMS1) = ((0x800U >> 2) << 16) | 1U;
    *(uint32_t *)(mmio + XHCI_CAP_DBOFF) = 0x400;
    *(uint32_t *)(mmio + XHCI_CAP_RTSOFF) = 0x600;
    /* Cap 1 @ 0x800: USB 2.0, next=4 dwords (0x810), port_off=1, port_cnt=1 */
    *(uint32_t *)(mmio + 0x800) = XHCI_EXT_CAP_PROTOCOL | (4U << 8) | (2U << 24);
    *(uint32_t *)(mmio + 0x808) = (1U << 8) | 1U;
    /* Cap 2 @ 0x810: USB 3.0, next=0, port_off=2, port_cnt=1 */
    *(uint32_t *)(mmio + 0x810) = XHCI_EXT_CAP_PROTOCOL | (3U << 24);
    *(uint32_t *)(mmio + 0x818) = (1U << 8) | 2U;

    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.mmio_virt = mmio;
    ctrl.mmio_len = sizeof(mmio);
    assert_eq(xhci_parse_capabilities(&ctrl), 0);
    assert_eq(ctrl.port_proto[1], 2);
    assert_eq(ctrl.port_proto[2], 3);

    /* 1a. USB 2 port 1 uses PR */
    volatile uint32_t *port1 = (volatile uint32_t *)(ctrl.op_regs + 0x400);
    *port1 = XHCI_PORTSC_CCS | XHCI_PORTSC_PP | (XHCI_PORT_SPEED_HIGH << 10);
    s_mock_portsc_ptr = port1;
    s_port_pr_seen = false;
    s_port_wpr_seen = false;
    assert_eq(xhci_port_reset(&ctrl, 1, &speed), 0);
    assert_true(s_port_pr_seen);
    assert_false(s_port_wpr_seen);
    assert_eq(speed, XHCI_PORT_SPEED_HIGH);

    /* 2. Enabled USB 3 port 2 in U0 requires NO reset */
    volatile uint32_t *port2 = (volatile uint32_t *)(ctrl.op_regs + 0x410);
    *port2 = XHCI_PORTSC_CCS | XHCI_PORTSC_PED | XHCI_PORTSC_PP |
             (XHCI_PLS_U0 << 5) | (XHCI_PORT_SPEED_SUPER << 10);
    s_mock_portsc_ptr = port2;
    s_port_pr_seen = false;
    s_port_wpr_seen = false;
    assert_eq(xhci_port_reset(&ctrl, 2, &speed), 0);
    assert_false(s_port_pr_seen);
    assert_false(s_port_wpr_seen);
    assert_eq(speed, XHCI_PORT_SPEED_SUPER);

    /* 3. Inactive USB 3 port 2 uses WPR (not PR) */
    *port2 = XHCI_PORTSC_CCS | XHCI_PORTSC_PP |
             (XHCI_PLS_INACTIVE << 5) | (XHCI_PORT_SPEED_SUPER << 10);
    s_port_pr_seen = false;
    s_port_wpr_seen = false;
    assert_eq(xhci_port_reset(&ctrl, 2, &speed), 0);
    assert_false(s_port_pr_seen);
    assert_true(s_port_wpr_seen);
    assert_eq(speed, XHCI_PORT_SPEED_SUPER);

    /* 4. Unknown speed ID (e.g. 5) is rejected */
    *port2 = XHCI_PORTSC_CCS | XHCI_PORTSC_PED | XHCI_PORTSC_PP |
             (XHCI_PLS_U0 << 5) | (5U << 10);
    assert_true(xhci_port_reset(&ctrl, 2, &speed) < 0);
    s_mock_portsc_ptr = NULL;

    /* 5. SuperSpeed MSC bulk endpoints get bMaxBurst from SS Companion with no streams */
    uint8_t ss_cfg[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 44, 0, 1, 1, 0, 0x80, 50,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 2, 0x08, 0x06, 0x50, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 0x02, 0x00, 0x04, 0,
        6, USB_DESC_TYPE_SS_EP_COMPANION, 7, 0, 0, 0, /* bMaxBurst=7, no streams */
        7, USB_DESC_TYPE_ENDPOINT, 0x02, 0x02, 0x00, 0x04, 0,
        6, USB_DESC_TYPE_SS_EP_COMPANION, 5, 0, 0, 0, /* bMaxBurst=5, no streams */
    };
    struct xhci_slot slot;
    memset(&slot, 0, sizeof(slot));
    slot.slot_id = 1;
    slot.speed = XHCI_PORT_SPEED_SUPER;
    slot.active = true;
    assert_eq(xhci_setup_endpoint_rings(&slot, ss_cfg, sizeof(ss_cfg)), 0);
    assert_true(slot.msc_candidate.active);
    assert_eq(slot.ep_rings[3]->max_burst, 7);
    assert_eq(slot.ep_rings[4]->max_burst, 5);
    xhci_free_endpoint_rings(&slot);
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
    test_xhci_set_configuration_2_stage_transfer();
    test_xhci_msc_alt_zero_pair();
    test_xhci_bulk_event_identity();
    test_xhci_bulk_wrap_and_boundary();
    test_xhci_clear_halt_state_drives_command_sequence();
    test_xhci_stop_endpoint_recovery();
    test_xhci_port_protocol_reset();
    TEST_RESULTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
