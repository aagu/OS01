/* @os01-test
 * id: test_xhci_intr_in
 * layer: host
 * subsystem: drivers
 * timing: strict
 * tier: nightly
 * link: xhci-intr-in
 * scenarios: []
 */
#include "test_framework.h"
#include <device/device.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>
#include <driver/xhci.h>
#include <driver/xhci_hub.h>
#include <driver/xhci_hid.h>
#include <arch/pci.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

/* ── Mock MMIO & DMA Allocator ─────────────────────────────────── */

static uint64_t s_mock_now_ns = 1000000ULL;
static uint32_t s_db_total_writes = 0;
static uint32_t s_db_slot_dci_writes[33][32];
static uint64_t s_tracked_phys = 0;
static uint32_t s_tracked_free_count = 0;
static bool s_force_high_alloc = false;

enum mock_db_mode {
    MOCK_DB_NONE = 0,
    MOCK_DB_CMD_WITH_HID_FIRST,
    MOCK_DB_EP0_WITH_HID_FIRST,
    MOCK_DB_BULK_WITH_HID_FIRST,
    MOCK_DB_STOP_EP0_WITH_HID_FIRST,
    MOCK_DB_RECOVER_BULK_WITH_HID_FIRST,
    MOCK_DB_DISABLE_SLOT_FAIL,
    MOCK_DB_DISABLE_SLOT_OK,
};

static struct {
    enum mock_db_mode mode;
    struct xhci_controller *ctrl;
    uint32_t evt_write_idx;
    uint8_t evt_write_cycle;
    uint64_t hid_trb_phys;
    uint8_t hid_slot;
    uint8_t hid_dci;
    uint32_t hid_residual;
    uint32_t hid_cc;
    uint8_t hid_payload[8];
    uint8_t bulk_dci;
    uint64_t bulk_trb_phys;
    uint32_t cmd_seq;
} s_mock;

static void mock_push_event(uint64_t param, uint32_t status, uint32_t control_no_cycle)
{
    uint32_t idx = s_mock.evt_write_idx;
    struct xhci_trb *ev = &s_mock.ctrl->event_ring_virt[idx];
    ev->parameter = param;
    ev->status = status;
    uint32_t *ctrl_ptr = (uint32_t *)((uint8_t *)ev + offsetof(struct xhci_trb, control));
    __atomic_store_n(ctrl_ptr,
                     (control_no_cycle & ~XHCI_TRB_CYCLE_BIT) |
                     (s_mock.evt_write_cycle ? XHCI_TRB_CYCLE_BIT : 0U),
                     __ATOMIC_RELEASE);

    idx = (idx + 1U) % XHCI_TRB_RING_SIZE;
    if (idx == 0) {
        s_mock.evt_write_cycle ^= 1U;
    }
    s_mock.evt_write_idx = idx;
}

static void mock_push_hid_event(void)
{
    struct xhci_slot *s = s_mock.ctrl->slots[s_mock.hid_slot];
    struct xhci_ep_ring *r = s->ep_rings[s_mock.hid_dci];
    if (r && r->intr_buf_virt) {
        memcpy(r->intr_buf_virt, s_mock.hid_payload, sizeof(s_mock.hid_payload));
    }
    uint32_t st = ((s_mock.hid_cc & 0xFFU) << 24) | (s_mock.hid_residual & 0xFFFFFFU);
    uint32_t ctl = ((uint32_t)s_mock.hid_slot << 24) |
                   ((uint32_t)s_mock.hid_dci << 16) |
                   (XHCI_TRB_TYPE_TRANSFER_EVT << 10);
    mock_push_event(s_mock.hid_trb_phys, st, ctl);
}

uint64_t arch_clocksource_read_ns(void)
{
    s_mock_now_ns += 1000ULL;
    return s_mock_now_ns;
}

void mock_xhci_write32(volatile void *addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;

    struct xhci_controller *c = s_mock.ctrl;
    if (!c || !c->db_regs) return;

    uintptr_t a = (uintptr_t)addr;
    uintptr_t db_base = (uintptr_t)c->db_regs;
    if (a < db_base || a >= db_base + 33 * 4) return;

    uint32_t slot = (uint32_t)((a - db_base) / 4);
    uint32_t dci = val & 0xFFU;
    s_db_total_writes++;
    if (slot < 33 && dci < 32) {
        s_db_slot_dci_writes[slot][dci]++;
    }

    if (slot == 0) {
        /* Command doorbell */
        uint32_t cmd_idx = (c->cmd_ring_enqueue == 0) ? (XHCI_TRB_RING_SIZE - 2) : (c->cmd_ring_enqueue - 1);
        uint64_t cmd_phys = c->cmd_ring_phys + (uint64_t)cmd_idx * sizeof(struct xhci_trb);
        uint32_t cmd_type = (c->cmd_ring_virt[cmd_idx].control >> 10) & 0x3FU;

        if (s_mock.mode == MOCK_DB_CMD_WITH_HID_FIRST) {
            mock_push_hid_event();
            mock_push_event(cmd_phys, (XHCI_COMP_SUCCESS << 24),
                            (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10));
            s_mock.mode = MOCK_DB_NONE;
        } else if (s_mock.mode == MOCK_DB_STOP_EP0_WITH_HID_FIRST &&
                   cmd_type == XHCI_TRB_TYPE_STOP_EP) {
            mock_push_hid_event();
            mock_push_event(0, (XHCI_COMP_STOPPED << 24),
                            (3U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10));
            mock_push_event(cmd_phys, (XHCI_COMP_SUCCESS << 24),
                            (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10));
            s_mock.mode = MOCK_DB_NONE;
        } else if (s_mock.mode == MOCK_DB_RECOVER_BULK_WITH_HID_FIRST) {
            s_mock.cmd_seq++;
            if (cmd_type == XHCI_TRB_TYPE_STOP_EP) {
                mock_push_hid_event();
                mock_push_event(s_mock.bulk_trb_phys, (XHCI_COMP_STOPPED << 24),
                                (1U << 24) | ((uint32_t)s_mock.bulk_dci << 16) |
                                (XHCI_TRB_TYPE_TRANSFER_EVT << 10));
                mock_push_event(cmd_phys, (XHCI_COMP_SUCCESS << 24),
                                (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10));
            } else if (cmd_type == XHCI_TRB_TYPE_SET_TR_DEQ) {
                mock_push_event(cmd_phys, (XHCI_COMP_SUCCESS << 24),
                                (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10));
                s_mock.mode = MOCK_DB_NONE;
            }
        } else if (cmd_type == XHCI_TRB_TYPE_DISABLE_SLOT) {
            if (s_mock.mode == MOCK_DB_DISABLE_SLOT_FAIL) {
                mock_push_event(cmd_phys, (XHCI_COMP_SLOT_NOT_ENABLED << 24),
                                (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10));
            } else if (s_mock.mode == MOCK_DB_DISABLE_SLOT_OK) {
                mock_push_event(cmd_phys, (XHCI_COMP_SUCCESS << 24),
                                (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10));
            }
        }
    } else if (slot == 1 && dci == 1) {
        /* EP0 doorbell */
        if (s_mock.mode == MOCK_DB_EP0_WITH_HID_FIRST) {
            struct xhci_slot *s = c->slots[1];
            uint32_t status_idx = (s->ep0_ring_enqueue == 0) ? (XHCI_TRB_RING_SIZE - 2) : (s->ep0_ring_enqueue - 1);
            uint64_t status_phys = s->ep0_ring_phys + (uint64_t)status_idx * sizeof(struct xhci_trb);
            mock_push_hid_event();
            mock_push_event(status_phys, (XHCI_COMP_SUCCESS << 24),
                            (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10));
            s_mock.mode = MOCK_DB_NONE;
        }
    } else if (slot == 1 && dci == s_mock.bulk_dci) {
        /* Bulk doorbell */
        if (s_mock.mode == MOCK_DB_BULK_WITH_HID_FIRST) {
            struct xhci_slot *s = c->slots[1];
            struct xhci_ep_ring *br = s->ep_rings[dci];
            uint32_t trb_idx = (br->enqueue == 0) ? (XHCI_TRB_RING_SIZE - 2) : (br->enqueue - 1);
            uint64_t trb_phys = br->ring_phys + (uint64_t)trb_idx * sizeof(struct xhci_trb);
            mock_push_hid_event();
            mock_push_event(trb_phys, (XHCI_COMP_SUCCESS << 24),
                            (1U << 24) | ((uint32_t)dci << 16) |
                            (XHCI_TRB_TYPE_TRANSFER_EVT << 10));
            s_mock.mode = MOCK_DB_NONE;
        }
    }
}

uint64_t alloc_4k_page(void)
{
    if (s_force_high_alloc) {
        return 0x100001000ULL;
    }
    void *p = aligned_alloc(4096, 4096);
    if (p) memset(p, 0, 4096);
    return (uint64_t)(uintptr_t)p;
}

void free_4k_page(uint64_t phys)
{
    if (!phys) return;
    if (s_tracked_phys && phys == s_tracked_phys) {
        s_tracked_free_count++;
    }
    if (phys == 0x100001000ULL) return;
    free((void *)(uintptr_t)phys);
}

void *kmalloc(size_t size)
{
    return calloc(1, size);
}

size_t kfree(void *ptr)
{
    free(ptr);
    return 1;
}

/* ── Test Fixture Helpers ──────────────────────────────────────── */

struct mock_hw {
    struct xhci_controller ctrl;
    struct xhci_slot slot;
    struct xhci_slot hub_slot;
    uint8_t *cmd_ring;
    uint8_t *event_ring;
    uint8_t *ep0_ring;
    uint8_t *dma_buf;
    uint8_t *dev_ctx;
    uint8_t *in_ctx;
    uint8_t *intr_ring;
    uint8_t *bulk_ring;
    uint32_t db_regs[64];
    uint8_t rt_regs[128];
    uint8_t op_regs[128];
};

static void setup_mock_hw(struct mock_hw *hw)
{
    memset(hw, 0, sizeof(*hw));
    memset(&s_mock, 0, sizeof(s_mock));
    s_db_total_writes = 0;
    memset(s_db_slot_dci_writes, 0, sizeof(s_db_slot_dci_writes));
    s_tracked_phys = 0;
    s_tracked_free_count = 0;
    s_force_high_alloc = false;

    hw->cmd_ring = aligned_alloc(4096, 4096);
    hw->event_ring = aligned_alloc(4096, 4096);
    hw->ep0_ring = aligned_alloc(4096, 4096);
    hw->dma_buf = aligned_alloc(4096, 4096);
    hw->dev_ctx = aligned_alloc(4096, 4096);
    hw->in_ctx = aligned_alloc(4096, 4096);
    hw->intr_ring = aligned_alloc(4096, 4096);
    hw->bulk_ring = aligned_alloc(4096, 4096);
    memset(hw->cmd_ring, 0, 4096);
    memset(hw->event_ring, 0, 4096);
    memset(hw->ep0_ring, 0, 4096);
    memset(hw->dma_buf, 0, 4096);
    memset(hw->dev_ctx, 0, 4096);
    memset(hw->in_ctx, 0, 4096);
    memset(hw->intr_ring, 0, 4096);
    memset(hw->bulk_ring, 0, 4096);

    struct xhci_controller *c = &hw->ctrl;
    c->cmd_ring_virt = (struct xhci_trb *)hw->cmd_ring;
    c->cmd_ring_phys = (uint64_t)(uintptr_t)hw->cmd_ring;
    c->cmd_ring_cycle = 1;
    c->event_ring_virt = (struct xhci_trb *)hw->event_ring;
    c->event_ring_phys = (uint64_t)(uintptr_t)hw->event_ring;
    c->event_ring_cycle = 1;
    c->db_regs = (volatile uint8_t *)hw->db_regs;
    c->rt_regs = (volatile uint8_t *)hw->rt_regs;
    c->op_regs = (volatile uint8_t *)hw->op_regs;
    c->ac64 = true;
    c->ac64_valid = true;
    c->running = true;

    struct xhci_slot *s = &hw->slot;
    s->ctrl = c;
    s->slot_id = 1;
    s->active = true;
    s->configured = true;
    s->ctx_published = true;
    s->max_packet_size = 8;
    s->ep0_ring_virt = (struct xhci_trb *)hw->ep0_ring;
    s->ep0_ring_phys = (uint64_t)(uintptr_t)hw->ep0_ring;
    s->ep0_ring_cycle = 1;
    s->dma_buf_virt = hw->dma_buf;
    s->dma_buf_phys = (uint64_t)(uintptr_t)hw->dma_buf;
    s->dev_ctx_virt = hw->dev_ctx;
    s->dev_ctx_phys = (uint64_t)(uintptr_t)hw->dev_ctx;
    s->input_ctx_virt = hw->in_ctx;
    s->input_ctx_phys = (uint64_t)(uintptr_t)hw->in_ctx;
    s->max_dci = 5;
    s->ep_rings = calloc(6, sizeof(struct xhci_ep_ring *));

    struct xhci_ep_ring *intr_r = calloc(1, sizeof(*intr_r));
    intr_r->ring_virt = (struct xhci_trb *)hw->intr_ring;
    intr_r->ring_phys = (uint64_t)(uintptr_t)hw->intr_ring;
    intr_r->cycle = 1;
    intr_r->dci = 5; /* EP2 IN (0x82) */
    intr_r->ep_type = XHCI_EP_TYPE_INTR_IN;
    intr_r->max_packet_size = 8;
    intr_r->active = true;
    s->ep_rings[5] = intr_r;

    struct xhci_ep_ring *bulk_r = calloc(1, sizeof(*bulk_r));
    bulk_r->ring_virt = (struct xhci_trb *)hw->bulk_ring;
    bulk_r->ring_phys = (uint64_t)(uintptr_t)hw->bulk_ring;
    bulk_r->cycle = 1;
    bulk_r->dci = 3; /* EP1 IN (0x81) */
    bulk_r->ep_type = XHCI_EP_TYPE_BULK_IN;
    bulk_r->max_packet_size = 512;
    bulk_r->active = true;
    s->ep_rings[3] = bulk_r;

    c->slots[1] = s;

    s_mock.ctrl = c;
    s_mock.evt_write_idx = 0;
    s_mock.evt_write_cycle = 1;
    s_mock.hid_slot = 1;
    s_mock.hid_dci = 5;
    s_mock.bulk_dci = 3;
    s_mock.hid_cc = XHCI_COMP_SHORT_TX;
    s_mock.hid_residual = 5; /* MPS=8 -> actual=3 */
    s_mock.hid_payload[0] = 0x01;
    s_mock.hid_payload[1] = 0xAA;
    s_mock.hid_payload[2] = 0x55;
}

static void teardown_mock_hw(struct mock_hw *hw)
{
    if (hw->ctrl.slots[1]) {
        struct xhci_slot *s = hw->ctrl.slots[1];
        if (s->ep_rings) {
            for (int d = 2; d <= s->max_dci; d++) {
                if (s->ep_rings[d]) {
                    if (s->ep_rings[d]->ring_phys) {
                        free((void *)(uintptr_t)s->ep_rings[d]->ring_phys);
                    }
                    free(s->ep_rings[d]);
                }
            }
            free(s->ep_rings);
        }
        if (s->bulk_staging_page) free(s->bulk_staging_page);
    }
    free(hw->cmd_ring);
    free(hw->event_ring);
    free(hw->ep0_ring);
    free(hw->dma_buf);
    free(hw->dev_ctx);
    free(hw->in_ctx);
    s_mock.ctrl = NULL;
}

/* ── Test 1: Interleavings, Stale Events & Ring Wraps ──────────── */

static void test_async_interrupt_interleavings(void)
{
    struct mock_hw hw;
    setup_mock_hw(&hw);
    struct xhci_controller *c = &hw.ctrl;
    struct xhci_slot *s = &hw.slot;
    struct xhci_ep_ring *intr_r = s->ep_rings[5];

    /* 1. Prepare must NOT ring doorbell */
    assert_eq(0, xhci_intr_in_prepare_locked(c, 1, 5));
    assert_eq(0U, s_db_total_writes);

    /* 2. Submit HID TD & interleave before Command Completion */
    assert_eq(0, xhci_intr_in_submit_locked(c, 1, 5));
    assert_eq(1U, s_db_slot_dci_writes[1][5]);
    s_mock.hid_trb_phys = intr_r->ring_phys;
    s_mock.mode = MOCK_DB_CMD_WITH_HID_FIRST;

    struct xhci_trb cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.control = (XHCI_TRB_TYPE_ENABLE_SLOT << 10) | c->cmd_ring_cycle;
    struct xhci_cmd_result cres;
    assert_eq(0, xhci_submit_cmd_until_locked(c, &cmd, 0, &cres));
    assert_eq(XHCI_CMD_COMPLETED, cres.state);

    uint8_t buf[8];
    struct xhci_intr_in_result res;
    assert_eq(1, xhci_intr_in_take_locked(c, 1, 5, buf, sizeof(buf), &res));
    assert_eq(XHCI_COMP_SHORT_TX, res.cc);
    assert_eq(3, res.actual);
    assert_false(res.bad_length);
    assert_eq(0x01, buf[0]);
    assert_eq(0xAA, buf[1]);
    assert_eq(0x55, buf[2]);
    /* Second take must return 0 (consumed) */
    assert_eq(0, xhci_intr_in_take_locked(c, 1, 5, buf, sizeof(buf), &res));

    /* 3. Interleave before EP0 Status */
    assert_eq(0, xhci_intr_in_submit_locked(c, 1, 5));
    s_mock.hid_trb_phys = intr_r->ring_phys + 1 * sizeof(struct xhci_trb);
    s_mock.mode = MOCK_DB_EP0_WITH_HID_FIRST;
    assert_eq(0, xhci_control_no_data(c, 1, 0x21, 0x0B, 0, 0));
    assert_eq(1, xhci_intr_in_take_locked(c, 1, 5, buf, sizeof(buf), &res));
    assert_eq(3, res.actual);

    /* 4. Interleave before Bulk Transfer */
    assert_eq(0, xhci_intr_in_submit_locked(c, 1, 5));
    s_mock.hid_trb_phys = intr_r->ring_phys + 2 * sizeof(struct xhci_trb);
    s_mock.mode = MOCK_DB_BULK_WITH_HID_FIRST;
    uint8_t bulk_buf[64];
    size_t bulk_actual = 0;
    assert_eq(0, xhci_bulk_transfer(c, 1, 3, bulk_buf, sizeof(bulk_buf), &bulk_actual));
    assert_eq(sizeof(bulk_buf), bulk_actual);
    assert_eq(1, xhci_intr_in_take_locked(c, 1, 5, buf, sizeof(buf), &res));

    /* 5. Interleave before xhci_recover_bulk_timeout */
    assert_eq(0, xhci_intr_in_submit_locked(c, 1, 5));
    s_mock.hid_trb_phys = intr_r->ring_phys + 3 * sizeof(struct xhci_trb);
    s->pending_bulk_dci = 3;
    s->pending_bulk_trb_phys = s->ep_rings[3]->ring_phys;
    s_mock.bulk_trb_phys = s->pending_bulk_trb_phys;
    s_mock.mode = MOCK_DB_RECOVER_BULK_WITH_HID_FIRST;
    assert_eq(0, xhci_recover_bulk_timeout(c, 1, 3));
    assert_eq(1, xhci_intr_in_take_locked(c, 1, 5, buf, sizeof(buf), &res));

    /* 6. Interleave before xhci_stop_ep_and_wait_locked (via split EP0 timeout settle on slot 3) */
    assert_eq(0, xhci_intr_in_submit_locked(c, 1, 5));
    s_mock.hid_trb_phys = intr_r->ring_phys + 4 * sizeof(struct xhci_trb);
    hw.hub_slot.ctrl = c;
    hw.hub_slot.slot_id = 2;
    hw.hub_slot.active = true;
    hw.hub_slot.tt_state = XHCI_TT_READY;
    c->slots[2] = &hw.hub_slot;
    struct xhci_slot split_slot = hw.slot;
    split_slot.slot_id = 3;
    split_slot.ep_rings = NULL;
    split_slot.max_dci = 0;
    split_slot.loc.tt_hub_slot = 2;
    split_slot.loc.tt_port = 1;
    c->slots[3] = &split_slot;
    s_mock.mode = MOCK_DB_STOP_EP0_WITH_HID_FIRST;
    /* Deadline expires after 1st clocksource tick -> split EP0 settle -> stop_ep_and_wait */
    uint64_t tight_deadline = s_mock_now_ns + 1500ULL;
    (void)xhci_control_no_data_until(c, 3, &(struct xhci_setup){0x21, 0x0B, 0, 0, 0}, tight_deadline);
    assert_eq(1, xhci_intr_in_take_locked(c, 1, 5, buf, sizeof(buf), &res));
    c->slots[3] = NULL;
    c->slots[2] = NULL;

    /* 7. Stale/invalid events: ED=1, wrong ptr, wrong slot/dci, duplicate */
    assert_eq(0, xhci_intr_in_submit_locked(c, 1, 5));
    uint64_t expected_trb = intr_r->ring_phys + 5 * sizeof(struct xhci_trb);
    /* ED=1 */
    mock_push_event(expected_trb, (XHCI_COMP_SHORT_TX << 24) | 5U,
                    (1U << 24) | (5U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | (1U << 2));
    /* Wrong pointer */
    mock_push_event(expected_trb + 16U, (XHCI_COMP_SHORT_TX << 24) | 5U,
                    (1U << 24) | (5U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10));
    /* Wrong slot */
    mock_push_event(expected_trb, (XHCI_COMP_SHORT_TX << 24) | 5U,
                    (2U << 24) | (5U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10));
    /* Wrong DCI */
    mock_push_event(expected_trb, (XHCI_COMP_SHORT_TX << 24) | 5U,
                    (1U << 24) | (4U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10));
    assert_eq(4U, xhci_intr_in_pump_locked(c, 16));
    assert_eq(0, xhci_intr_in_take_locked(c, 1, 5, buf, sizeof(buf), &res));

    /* Real completion + duplicate completion */
    mock_push_event(expected_trb, (XHCI_COMP_SHORT_TX << 24) | 5U,
                    (1U << 24) | (5U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10));
    mock_push_event(expected_trb, (XHCI_COMP_SHORT_TX << 24) | 5U,
                    (1U << 24) | (5U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10));
    assert_eq(2U, xhci_intr_in_pump_locked(c, 16));
    assert_eq(1, xhci_intr_in_take_locked(c, 1, 5, buf, sizeof(buf), &res));
    assert_eq(0, xhci_intr_in_take_locked(c, 1, 5, buf, sizeof(buf), &res));

    /* 8. Event ring wrap 255 -> 0 & ERDP check */
    c->event_ring_dequeue = 255;
    c->event_ring_cycle = 1;
    s_mock.evt_write_idx = 255;
    s_mock.evt_write_cycle = 1;
    assert_eq(0, xhci_intr_in_submit_locked(c, 1, 5));
    expected_trb = intr_r->ring_phys + 6 * sizeof(struct xhci_trb);
    mock_push_event(expected_trb, (XHCI_COMP_SHORT_TX << 24) | 5U,
                    (1U << 24) | (5U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10));
    assert_eq(1U, xhci_intr_in_pump_locked(c, 8));
    assert_eq(0U, c->event_ring_dequeue);
    assert_eq(0U, c->event_ring_cycle);
    uint64_t erdp_written = *(volatile uint64_t *)(c->rt_regs + 0x20 + XHCI_INTR_ERDP);
    assert_true(erdp_written == (c->event_ring_phys | XHCI_ERDP_EHB));
    assert_eq(1, xhci_intr_in_take_locked(c, 1, 5, buf, sizeof(buf), &res));

    /* 9. Transfer ring wrap 254 -> Link(255) -> 0 */
    intr_r->enqueue = 254;
    intr_r->cycle = 1;
    assert_eq(0, xhci_intr_in_submit_locked(c, 1, 5));
    assert_eq(255U, intr_r->enqueue);
    mock_push_event(intr_r->ring_phys + 254U * sizeof(struct xhci_trb),
                    (XHCI_COMP_SHORT_TX << 24) | 5U,
                    (1U << 24) | (5U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10));
    assert_eq(1U, xhci_intr_in_pump_locked(c, 8));
    assert_eq(1, xhci_intr_in_take_locked(c, 1, 5, buf, sizeof(buf), &res));

    /* Now enqueue == 255: submit must wrap via Link TRB to index 0 with toggled cycle */
    assert_eq(0, xhci_intr_in_submit_locked(c, 1, 5));
    assert_eq(1U, intr_r->enqueue);
    assert_eq(0U, intr_r->cycle);
    assert_eq(0U, (intr_r->ring_virt[0].control & XHCI_TRB_CYCLE_BIT));
    mock_push_event(intr_r->ring_phys,
                    (XHCI_COMP_SHORT_TX << 24) | 5U,
                    (1U << 24) | (5U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10));
    assert_eq(1U, xhci_intr_in_pump_locked(c, 8));
    assert_eq(1, xhci_intr_in_take_locked(c, 1, 5, buf, sizeof(buf), &res));

    /* 10. Late event after retire_locked is discarded */
    assert_eq(0, xhci_intr_in_submit_locked(c, 1, 5));
    uint64_t late_trb = intr_r->ring_phys + 1U * sizeof(struct xhci_trb);
    xhci_intr_in_retire_locked(c, 1, 5);
    mock_push_event(late_trb, (XHCI_COMP_SHORT_TX << 24) | 5U,
                    (1U << 24) | (5U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10));
    assert_eq(1U, xhci_intr_in_pump_locked(c, 8));
    assert_true(xhci_intr_in_take_locked(c, 1, 5, buf, sizeof(buf), &res) < 0);

    teardown_mock_hw(&hw);
}

/* ── Test 2: Ownership, Bad Length, Retire, DMA & AC64 ─────────── */

static void test_interrupt_ownership(void)
{
    struct mock_hw hw;
    setup_mock_hw(&hw);
    struct xhci_controller *c = &hw.ctrl;
    struct xhci_slot *s = &hw.slot;
    struct xhci_ep_ring *intr_r = s->ep_rings[5];

    /* 1. Prepare & submit */
    assert_eq(0, xhci_intr_in_prepare_locked(c, 1, 5));
    assert_eq(0, xhci_intr_in_submit_locked(c, 1, 5));
    uint32_t db_before = s_db_slot_dci_writes[1][5];

    /* 2. Pump with no events keeps TD pending and does not ring doorbell */
    assert_eq(0U, xhci_intr_in_pump_locked(c, 64));
    assert_eq(0U, xhci_intr_in_pump_locked(c, 64));
    assert_eq(db_before, s_db_slot_dci_writes[1][5]);
    /* Cannot submit while pending */
    assert_true(xhci_intr_in_submit_locked(c, 1, 5) < 0);
    assert_eq(db_before, s_db_slot_dci_writes[1][5]);

    /* 3. Normal receive MPS=8, residual=5 -> actual=3; small cap does not consume */
    uint8_t *dma_buf = (uint8_t *)intr_r->intr_buf_virt;
    dma_buf[0] = 0x11;
    dma_buf[1] = 0x22;
    dma_buf[2] = 0x33;
    mock_push_event(intr_r->ring_phys, (XHCI_COMP_SHORT_TX << 24) | 5U,
                    (1U << 24) | (5U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10));
    assert_eq(1U, xhci_intr_in_pump_locked(c, 8));

    /* Cannot submit before take */
    assert_true(xhci_intr_in_submit_locked(c, 1, 5) < 0);
    assert_eq(db_before, s_db_slot_dci_writes[1][5]);

    uint8_t out_buf[8] = {0};
    struct xhci_intr_in_result res;
    /* Cap=2 < actual=3 -> negative and NOT consumed */
    assert_true(xhci_intr_in_take_locked(c, 1, 5, out_buf, 2, &res) < 0);
    assert_eq(1, xhci_intr_in_take_locked(c, 1, 5, out_buf, sizeof(out_buf), &res));
    assert_eq(XHCI_COMP_SHORT_TX, res.cc);
    assert_eq(3, res.actual);
    assert_false(res.bad_length);
    assert_eq(0x11, out_buf[0]);
    assert_eq(0x22, out_buf[1]);
    assert_eq(0x33, out_buf[2]);

    /* 4. residual=9 (> MPS=8) -> bad_length=true, actual=0, no copy */
    assert_eq(0, xhci_intr_in_submit_locked(c, 1, 5));
    mock_push_event(intr_r->ring_phys + sizeof(struct xhci_trb),
                    (XHCI_COMP_SHORT_TX << 24) | 9U,
                    (1U << 24) | (5U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10));
    assert_eq(1U, xhci_intr_in_pump_locked(c, 8));
    assert_eq(1, xhci_intr_in_take_locked(c, 1, 5, NULL, 0, &res));
    assert_true(res.bad_length);
    assert_eq(0, res.actual);

    /* 5. Retire skips cached result and forbids submit */
    assert_eq(0, xhci_intr_in_submit_locked(c, 1, 5));
    mock_push_event(intr_r->ring_phys + 2U * sizeof(struct xhci_trb),
                    (XHCI_COMP_SHORT_TX << 24) | 5U,
                    (1U << 24) | (5U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10));
    assert_eq(1U, xhci_intr_in_pump_locked(c, 8));
    xhci_intr_in_retire_locked(c, 1, 5);
    assert_true(xhci_intr_in_submit_locked(c, 1, 5) < 0);
    assert_true(xhci_intr_in_take_locked(c, 1, 5, out_buf, sizeof(out_buf), &res) < 0);

    /* 6. Hardware-evidenced DMA teardown:
     *    Disable Slot not confirmed -> DMA page retained.
     *    Disable Slot confirmed -> DMA page freed exactly once. */
    uint64_t intr_dma = intr_r->intr_buf_phys;
    assert_true(intr_dma != 0ULL);
    s_tracked_phys = intr_dma;
    s_tracked_free_count = 0;

    /* Allocate a heap-backed slot for xhci_free_slot */
    struct xhci_slot *dyn_slot = calloc(1, sizeof(*dyn_slot));
    *dyn_slot = hw.slot;
    dyn_slot->ep0_ring_phys = 0;
    dyn_slot->dma_buf_phys = 0;
    dyn_slot->dev_ctx_phys = 0;
    dyn_slot->input_ctx_phys = 0;
    c->slots[1] = dyn_slot;

    s_mock.mode = MOCK_DB_DISABLE_SLOT_FAIL;
    xhci_free_slot(c, 1);
    assert_eq(0U, s_tracked_free_count);
    assert_true(dyn_slot->dma_retained);

    /* Reset poison/retained flags to test confirmed Disable Slot */
    dyn_slot->poisoned = false;
    dyn_slot->dma_retained = false;
    c->poisoned = false;
    s_mock.mode = MOCK_DB_DISABLE_SLOT_OK;
    xhci_free_slot(c, 1);
    assert_eq(1U, s_tracked_free_count);
    assert_null(c->slots[1]);

    /* Cleanup remaining stack-allocated hw rings */
    free(hw.cmd_ring);
    free(hw.event_ring);
    free(hw.ep0_ring);
    free(hw.dma_buf);
    free(hw.dev_ctx);
    free(hw.in_ctx);

    /* 7. Non-AC64 >32-bit address: prepare/submit fail and NEVER ring doorbell */
    setup_mock_hw(&hw);
    c = &hw.ctrl;
    c->ac64 = false;
    c->ac64_valid = true;
    s_force_high_alloc = true;
    assert_eq(-EIO, xhci_intr_in_prepare_locked(c, 1, 5));
    s_force_high_alloc = false;

    /* Even if intr_buf_phys was >32-bit before ac64 became false, submit must not ring db */
    c->ac64 = true;
    assert_eq(0, xhci_intr_in_prepare_locked(c, 1, 5));
    c->ac64 = false;
    hw.slot.ep_rings[5]->intr_buf_phys = 0x100001000ULL;
    uint32_t db_before_ac64 = s_db_total_writes;
    assert_eq(-EIO, xhci_intr_in_submit_locked(c, 1, 5));
    assert_eq(db_before_ac64, s_db_total_writes);

    free_4k_page((uint64_t)(uintptr_t)hw.slot.ep_rings[5]->intr_buf_virt);
    teardown_mock_hw(&hw);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_async_interrupt_interleavings),
    TEST_ENTRY(test_interrupt_ownership),
TEST_LIST_END

int main(void)
{
    TEST_SUITE("xhci_intr_in");
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}
