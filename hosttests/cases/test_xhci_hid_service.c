/* @os01-test
 * id: test_xhci_hid_service
 * layer: host
 * subsystem: drivers
 * timing: strict
 * tier: nightly
 * link: xhci-hid-service
 * scenarios: []
 */
#include "test_framework.h"
#include <device/device.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>
#include <driver/xhci.h>
#include <driver/xhci_hub.h>
#include <driver/xhci_hid.h>
#include <driver/input_route.h>
#include <driver/usb_hid_report.h>
#include <arch/pci.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

/* Test-only hooks in xhci_hid.c */
int xhci_hid__test_init(void);
void xhci_hid__test_reset(void);

/* ── Mock Route & Lock Assertions ──────────────────────────────── */

static struct xhci_controller *s_active_ctrls[4];
static uint8_t s_active_ctrl_count = 0;

static uint32_t s_kbd_deliveries = 0;
static input_ps2_frame_t s_last_kbd_frames[32];
static uint32_t s_last_kbd_count = 0;
static uint32_t s_kbd_release_deliveries = 0;

static uint32_t s_mouse_deliveries = 0;
static mouse_event_t s_last_mouse_ev;
static uint32_t s_mouse_release_deliveries = 0;

static void assert_no_io_lock_held(void)
{
    for (uint8_t i = 0; i < s_active_ctrl_count; i++) {
        if (s_active_ctrls[i]) {
            assert_eq(0, s_active_ctrls[i]->io_lock.owner);
        }
    }
}

void input_route_keyboard_usb_frames(const input_ps2_frame_t *frames, size_t count)
{
    assert_no_io_lock_held();
    s_kbd_deliveries++;
    s_last_kbd_count = (uint32_t)(count < 32 ? count : 32);
    if (frames && s_last_kbd_count > 0) {
        memcpy(s_last_kbd_frames, frames, s_last_kbd_count * sizeof(input_ps2_frame_t));
        bool all_break = true;
        for (size_t i = 0; i < count; i++) {
            if (frames[i].is_make != 0) {
                all_break = false;
                break;
            }
        }
        if (all_break) {
            s_kbd_release_deliveries++;
        }
    }
}

void input_route_mouse_usb_event(const mouse_event_t *event)
{
    assert_no_io_lock_held();
    s_mouse_deliveries++;
    if (event) {
        s_last_mouse_ev = *event;
        if (event->dx == 0 && event->dy == 0 && event->wheel == 0 && event->buttons == 0) {
            s_mouse_release_deliveries++;
        }
    }
}

/* ── Mock Kernel / xHCI Environment ────────────────────────────── */

uint8_t xhci_controller_count(void)
{
    return s_active_ctrl_count;
}

struct xhci_controller *xhci_controller_get(uint8_t index)
{
    if (index >= s_active_ctrl_count) return NULL;
    return s_active_ctrls[index];
}

static uint64_t s_mock_now_ns = 10000000ULL;
uint64_t arch_clocksource_read_ns(void)
{
    s_mock_now_ns += 1000ULL;
    return s_mock_now_ns;
}

static bool s_fail_next_alloc = false;

uint64_t alloc_4k_page(void)
{
    if (s_fail_next_alloc) {
        s_fail_next_alloc = false;
        return 0;
    }
    void *p = aligned_alloc(4096, 4096);
    if (p) memset(p, 0, 4096);
    return (uint64_t)(uintptr_t)p;
}

void free_4k_page(uint64_t phys)
{
    if (!phys) return;
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

static int s_kthread_creates = 0;
int kernel_thread_on_cpu(uint64_t (*fn)(uint64_t), uint64_t arg, uint32_t cpu)
{
    (void)fn;
    (void)arg;
    (void)cpu;
    s_kthread_creates++;
    return 42;
}

/* ── Mock Doorbell & Setup Recording ───────────────────────────── */

struct recorded_setup {
    uint8_t slot;
    uint8_t type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
};

static struct recorded_setup s_setups[32];
static uint32_t s_setup_count = 0;
static uint8_t s_stall_slot = 0;
static uint16_t s_stall_iface = 0xFFFFU;
static uint32_t s_hub_ep0_writes = 0;
static uint8_t s_hub_slot_id = 0;
static uint32_t s_db_writes[33][32];
static bool s_auto_complete_ep0 = true;

struct mock_hw {
    struct pci_device pdev;
    struct xhci_controller ctrl;
    struct xhci_slot slots[6];
    struct xhci_ep_ring *ep_rings_storage[6][16];
    struct xhci_ep_ring rings[6][16];
    uint8_t *cmd_ring;
    uint8_t *event_ring;
    uint8_t *ep0_rings[6];
    uint8_t *dma_bufs[6];
    uint8_t *dev_ctxs[6];
    uint8_t *in_ctxs[6];
    uint8_t *intr_rings[6][16];
    uint32_t db_regs[64];
    uint8_t rt_regs[128];
    uint8_t op_regs[0x500];
    uint32_t evt_write_idx;
    uint8_t evt_write_cycle;
};

static struct mock_hw *s_hw_list[4];
static uint8_t s_hw_count = 0;

static void hw_push_event(struct mock_hw *hw, uint64_t param, uint32_t status, uint32_t ctl_no_cycle)
{
    uint32_t idx = hw->evt_write_idx;
    struct xhci_trb *ev = &hw->ctrl.event_ring_virt[idx];
    ev->parameter = param;
    ev->status = status;
    uint32_t *ctrl_ptr = (uint32_t *)((uint8_t *)ev + offsetof(struct xhci_trb, control));
    __atomic_store_n(ctrl_ptr,
                     (ctl_no_cycle & ~XHCI_TRB_CYCLE_BIT) |
                     (hw->evt_write_cycle ? XHCI_TRB_CYCLE_BIT : 0U),
                     __ATOMIC_RELEASE);
    idx = (idx + 1U) % XHCI_TRB_RING_SIZE;
    if (idx == 0) hw->evt_write_cycle ^= 1U;
    hw->evt_write_idx = idx;
}

static void hw_push_intr_event(struct mock_hw *hw, uint8_t slot, uint8_t dci,
                               const uint8_t *data, uint16_t len, uint32_t cc)
{
    struct xhci_slot *s = hw->ctrl.slots[slot];
    struct xhci_ep_ring *r = s->ep_rings[dci];
    if (data && len > 0 && r->intr_buf_virt) {
        memcpy(r->intr_buf_virt, data, len);
    }
    uint32_t residual = (r->max_packet_size >= len) ? (r->max_packet_size - len) : 0U;
    uint32_t st = ((cc & 0xFFU) << 24) | (residual & 0xFFFFFFU);
    uint32_t ctl = ((uint32_t)slot << 24) | ((uint32_t)dci << 16) |
                   (XHCI_TRB_TYPE_TRANSFER_EVT << 10);
    hw_push_event(hw, r->intr_pending_trb_phys, st, ctl);
}

void mock_xhci_write32(volatile void *addr, uint32_t val)
{
    for (uint8_t h = 0; h < s_hw_count; h++) {
        struct mock_hw *hw = s_hw_list[h];
        struct xhci_controller *c = &hw->ctrl;
        uintptr_t a = (uintptr_t)addr;
        if (c->op_regs) {
            uintptr_t pbase = (uintptr_t)(c->op_regs + 0x400);
            if (a >= pbase && a < pbase + 8 * 0x10 && ((a - pbase) % 0x10) == 0) {
                uint32_t cur = *(volatile uint32_t *)addr;
                /* RW1C bits: writing 1 clears, writing 0 preserves */
                cur &= ~(val & XHCI_PORTSC_RW1C_MASK);
                if (val & XHCI_PORTSC_PR) {
                    /* Simulate reset completion: PR self-clears, PRC & PED set */
                    cur &= ~XHCI_PORTSC_PR;
                    cur |= XHCI_PORTSC_PRC | XHCI_PORTSC_PED;
                }
                *(volatile uint32_t *)addr = cur;
                return;
            }
        }
    }

    *(volatile uint32_t *)addr = val;

    for (uint8_t h = 0; h < s_hw_count; h++) {
        struct mock_hw *hw = s_hw_list[h];
        struct xhci_controller *c = &hw->ctrl;
        if (!c->db_regs) continue;
        uintptr_t a = (uintptr_t)addr;
        uintptr_t db_base = (uintptr_t)c->db_regs;
        if (a < db_base || a >= db_base + 33 * 4) continue;

        uint32_t slot = (uint32_t)((a - db_base) / 4);
        uint32_t dci = val & 0xFFU;
        if (slot < 33 && dci < 32) {
            s_db_writes[slot][dci]++;
        }

        if (slot >= 1 && slot <= 5 && dci == 1 && s_auto_complete_ep0) {
            if (slot == s_hub_slot_id) {
                s_hub_ep0_writes++;
            }
            struct xhci_slot *s = c->slots[slot];
            if (s && s->ep0_ring_virt) {
                uint32_t status_idx = (s->ep0_ring_enqueue == 0) ? (XHCI_TRB_RING_SIZE - 2) : (s->ep0_ring_enqueue - 1);
                uint32_t setup_idx = (status_idx == 0) ? (XHCI_TRB_RING_SIZE - 2) : (status_idx - 1);
                struct xhci_trb *setup_trb = &s->ep0_ring_virt[setup_idx];
                uint64_t p = setup_trb->parameter;
                uint8_t rtype = (uint8_t)(p & 0xFFU);
                uint8_t req = (uint8_t)((p >> 8) & 0xFFU);
                uint16_t wval = (uint16_t)((p >> 16) & 0xFFFFU);
                uint16_t widx = (uint16_t)((p >> 32) & 0xFFFFU);
                uint16_t wlen = (uint16_t)((p >> 48) & 0xFFFFU);

                if (s_setup_count < 32) {
                    s_setups[s_setup_count++] = (struct recorded_setup){
                        .slot = (uint8_t)slot,
                        .type = rtype,
                        .request = req,
                        .value = wval,
                        .index = widx,
                        .length = wlen
                    };
                }

                uint32_t cc = XHCI_COMP_SUCCESS;
                if (slot == s_stall_slot && widx == s_stall_iface) {
                    cc = XHCI_COMP_STALL;
                }
                uint64_t status_phys = s->ep0_ring_phys + (uint64_t)status_idx * sizeof(struct xhci_trb);
                hw_push_event(hw, status_phys, (cc << 24),
                              (slot << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10));
            }
        }
        break;
    }
}

static void reset_test_globals(void)
{
    s_active_ctrl_count = 0;
    s_hw_count = 0;
    s_kbd_deliveries = 0;
    s_last_kbd_count = 0;
    s_kbd_release_deliveries = 0;
    s_mouse_deliveries = 0;
    memset(&s_last_mouse_ev, 0, sizeof(s_last_mouse_ev));
    s_mouse_release_deliveries = 0;
    s_setup_count = 0;
    s_stall_slot = 0;
    s_stall_iface = 0xFFFFU;
    s_hub_ep0_writes = 0;
    s_hub_slot_id = 0;
    memset(s_db_writes, 0, sizeof(s_db_writes));
    s_fail_next_alloc = false;
    s_kthread_creates = 0;
    s_auto_complete_ep0 = true;
    xhci_hid__test_reset();
}

static void init_mock_hw(struct mock_hw *hw, uint8_t bus, uint8_t dev, uint8_t fn)
{
    memset(hw, 0, sizeof(*hw));
    hw->pdev.bus = bus;
    hw->pdev.slot = dev;
    hw->pdev.fn = fn;
    hw->pdev.driver_data = &hw->ctrl;

    hw->cmd_ring = aligned_alloc(4096, 4096);
    hw->event_ring = aligned_alloc(4096, 4096);
    memset(hw->cmd_ring, 0, 4096);
    memset(hw->event_ring, 0, 4096);

    struct xhci_controller *c = &hw->ctrl;
    c->pdev = &hw->pdev;
    c->cmd_ring_virt = (struct xhci_trb *)hw->cmd_ring;
    c->cmd_ring_phys = (uint64_t)(uintptr_t)hw->cmd_ring;
    c->cmd_ring_cycle = 1;
    c->event_ring_virt = (struct xhci_trb *)hw->event_ring;
    c->event_ring_phys = (uint64_t)(uintptr_t)hw->event_ring;
    c->event_ring_cycle = 1;
    c->db_regs = (volatile uint8_t *)hw->db_regs;
    c->rt_regs = (volatile uint8_t *)hw->rt_regs;
    c->op_regs = (volatile uint8_t *)hw->op_regs;
    c->max_ports = 8;
    c->max_slots = 8;
    c->ac64 = true;
    c->ac64_valid = true;
    c->running = true;
    c->io_lock_init = true;
    mutex_init(&c->io_lock);

    for (uint32_t p = 1; p <= 8; p++) {
        volatile uint32_t *portsc = (volatile uint32_t *)(hw->op_regs + 0x400 + (p - 1) * 0x10);
        *portsc = XHCI_PORTSC_PP | XHCI_PORTSC_CCS | XHCI_PORTSC_PED;
    }
    *(volatile uint32_t *)(hw->op_regs + XHCI_OP_USBSTS) = XHCI_STS_HCH;

    hw->evt_write_cycle = 1;

    s_hw_list[s_hw_count++] = hw;
    s_active_ctrls[s_active_ctrl_count++] = c;
}

static struct xhci_slot *setup_mock_slot(struct mock_hw *hw, uint8_t slot_id, uint8_t root_port)
{
    struct xhci_slot *s = &hw->slots[slot_id];
    memset(s, 0, sizeof(*s));
    hw->ep0_rings[slot_id] = aligned_alloc(4096, 4096);
    hw->dma_bufs[slot_id] = aligned_alloc(4096, 4096);
    hw->dev_ctxs[slot_id] = aligned_alloc(4096, 4096);
    hw->in_ctxs[slot_id] = aligned_alloc(4096, 4096);
    memset(hw->ep0_rings[slot_id], 0, 4096);
    memset(hw->dma_bufs[slot_id], 0, 4096);
    memset(hw->dev_ctxs[slot_id], 0, 4096);
    memset(hw->in_ctxs[slot_id], 0, 4096);

    s->ctrl = &hw->ctrl;
    s->slot_id = slot_id;
    s->port_num = root_port;
    s->loc.root_port = root_port;
    s->active = true;
    s->configured = true;
    s->ctx_published = true;
    s->max_packet_size = 8;
    s->ep0_ring_virt = (struct xhci_trb *)hw->ep0_rings[slot_id];
    s->ep0_ring_phys = (uint64_t)(uintptr_t)hw->ep0_rings[slot_id];
    s->ep0_ring_cycle = 1;
    s->dma_buf_virt = hw->dma_bufs[slot_id];
    s->dma_buf_phys = (uint64_t)(uintptr_t)hw->dma_bufs[slot_id];
    s->dev_ctx_virt = hw->dev_ctxs[slot_id];
    s->dev_ctx_phys = (uint64_t)(uintptr_t)hw->dev_ctxs[slot_id];
    s->input_ctx_virt = hw->in_ctxs[slot_id];
    s->input_ctx_phys = (uint64_t)(uintptr_t)hw->in_ctxs[slot_id];
    s->ep_rings = hw->ep_rings_storage[slot_id];
    s->max_dci = 15;
    hw->ctrl.slots[slot_id] = s;
    return s;
}

static void setup_intr_ep(struct mock_hw *hw, uint8_t slot_id, uint8_t dci, uint16_t mps)
{
    struct xhci_slot *s = hw->ctrl.slots[slot_id];
    struct xhci_ep_ring *r = &hw->rings[slot_id][dci];
    memset(r, 0, sizeof(*r));
    hw->intr_rings[slot_id][dci] = aligned_alloc(4096, 4096);
    memset(hw->intr_rings[slot_id][dci], 0, 4096);
    r->ring_virt = (struct xhci_trb *)hw->intr_rings[slot_id][dci];
    r->ring_phys = (uint64_t)(uintptr_t)hw->intr_rings[slot_id][dci];
    r->cycle = 1;
    r->dci = dci;
    r->ep_type = XHCI_EP_TYPE_INTR_IN;
    r->max_packet_size = mps;
    r->active = true;
    s->ep_rings[dci] = r;
}

static void free_mock_hw(struct mock_hw *hw)
{
    free(hw->cmd_ring);
    free(hw->event_ring);
    for (int s = 1; s < 6; s++) {
        free(hw->ep0_rings[s]);
        free(hw->dma_bufs[s]);
        free(hw->dev_ctxs[s]);
        free(hw->in_ctxs[s]);
        for (int d = 0; d < 16; d++) {
            free(hw->intr_rings[s][d]);
            if (hw->rings[s][d].intr_buf_phys) {
                free_4k_page(hw->rings[s][d].intr_buf_phys);
                hw->rings[s][d].intr_buf_phys = 0;
            }
        }
    }
}

/* ── Test Cases ────────────────────────────────────────────────── */

static void test_bind_and_fail_closed(void)
{
    reset_test_globals();
    struct mock_hw hw;
    init_mock_hw(&hw, 0, 0x14, 0);

    /* Case 1: Empty controller -> no kthread created, mouse not bound */
    assert_eq(0, xhci_hid__test_init());
    assert_false(xhci_hid_mouse_bound());
    assert_eq(0, arch_usb_hid_start_worker());
    assert_eq(0, s_kthread_creates);
    free_mock_hw(&hw);

    /* Case 2: Slot 1 composite (iface=3 kbd ACKs, iface=8 mouse STALLs) -> slot 1 isolated,
     * neither published; Slot 2 (mouse) still binds! */
    reset_test_globals();
    init_mock_hw(&hw, 0, 0x14, 0);

    struct xhci_slot *s1 = setup_mock_slot(&hw, 1, 1);
    s1->hid_candidates[XHCI_HID_KEYBOARD] = (struct xhci_hid_candidate){
        .valid = true, .iface = 3, .dci = 3, .ep_addr = 0x81, .interval = 10, .mps = 8
    };
    s1->hid_candidates[XHCI_HID_MOUSE] = (struct xhci_hid_candidate){
        .valid = true, .iface = 8, .dci = 5, .ep_addr = 0x82, .interval = 10, .mps = 4
    };
    setup_intr_ep(&hw, 1, 3, 8);
    setup_intr_ep(&hw, 1, 5, 4);
    s_stall_slot = 1;
    s_stall_iface = 8;

    /* Slot 2: mouse on iface 1, dci 3 */
    struct xhci_slot *s2 = setup_mock_slot(&hw, 2, 2);
    s2->hid_candidates[XHCI_HID_MOUSE] = (struct xhci_hid_candidate){
        .valid = true, .iface = 1, .dci = 3, .ep_addr = 0x81, .interval = 10, .mps = 4
    };
    setup_intr_ep(&hw, 2, 3, 4);

    /* Slot 3: unconfigured */
    struct xhci_slot *s3 = setup_mock_slot(&hw, 3, 3);
    s3->configured = false;
    s3->hid_candidates[XHCI_HID_KEYBOARD] = (struct xhci_hid_candidate){
        .valid = true, .iface = 0, .dci = 3, .ep_addr = 0x81, .interval = 10, .mps = 8
    };
    setup_intr_ep(&hw, 3, 3, 8);

    /* Slot 4: MSC mixed */
    struct xhci_slot *s4 = setup_mock_slot(&hw, 4, 4);
    s4->msc_iface.active = true;
    s4->hid_candidates[XHCI_HID_KEYBOARD] = (struct xhci_hid_candidate){
        .valid = true, .iface = 0, .dci = 3, .ep_addr = 0x81, .interval = 10, .mps = 8
    };
    setup_intr_ep(&hw, 4, 3, 8);

    /* Slot 5: depth > 1 */
    struct xhci_slot *s5 = setup_mock_slot(&hw, 5, 5);
    s5->loc.depth = 2;
    s5->hid_candidates[XHCI_HID_KEYBOARD] = (struct xhci_hid_candidate){
        .valid = true, .iface = 0, .dci = 3, .ep_addr = 0x81, .interval = 10, .mps = 8
    };
    setup_intr_ep(&hw, 5, 3, 8);

    assert_eq(0, xhci_hid__test_init());

    bool saw_s1_iface3 = false;
    for (uint32_t i = 0; i < s_setup_count; i++) {
        assert_true(s_setups[i].request != 0x0AU);
        if (s_setups[i].slot == 1 && s_setups[i].index == 3) {
            assert_eq(0x21U, s_setups[i].type);
            assert_eq(0x0BU, s_setups[i].request);
            assert_eq(0U, s_setups[i].value);
            assert_eq(0U, s_setups[i].length);
            saw_s1_iface3 = true;
        }
        assert_true(s_setups[i].slot != 3);
        assert_true(s_setups[i].slot != 4);
        assert_true(s_setups[i].slot != 5);
    }
    assert_true(saw_s1_iface3);

    assert_false(hw.rings[1][3].intr_bound);
    assert_false(hw.rings[1][5].intr_bound);
    assert_true(hw.rings[2][3].intr_bound);
    assert_true(xhci_hid_mouse_bound());

    /* Boot stage must NOT submit any permanent HID TD */
    assert_eq(0U, s_db_writes[1][3]);
    assert_eq(0U, s_db_writes[2][3]);

    /* Runtime worker starts and submits Slot 2 DCI 3 */
    assert_eq(0, arch_usb_hid_start_worker());
    assert_eq(1, s_kthread_creates);
    xhci_hid_service_once(20000000ULL);
    assert_eq(0U, s_db_writes[1][3]);
    assert_eq(1U, s_db_writes[2][3]);
    free_mock_hw(&hw);

    /* Allocation failure */
    reset_test_globals();
    init_mock_hw(&hw, 0, 0x14, 0);
    s1 = setup_mock_slot(&hw, 1, 1);
    s1->hid_candidates[XHCI_HID_MOUSE] = (struct xhci_hid_candidate){
        .valid = true, .iface = 0, .dci = 3, .ep_addr = 0x81, .interval = 10, .mps = 4
    };
    setup_intr_ep(&hw, 1, 3, 4);
    s_fail_next_alloc = true;
    assert_eq(0, xhci_hid__test_init());
    assert_false(xhci_hid_mouse_bound());
    free_mock_hw(&hw);

    /* Mismatched ring */
    reset_test_globals();
    init_mock_hw(&hw, 0, 0x14, 0);
    s1 = setup_mock_slot(&hw, 1, 1);
    s1->hid_candidates[XHCI_HID_MOUSE] = (struct xhci_hid_candidate){
        .valid = true, .iface = 0, .dci = 3, .ep_addr = 0x81, .interval = 10, .mps = 4
    };
    assert_eq(0, xhci_hid__test_init());
    assert_false(xhci_hid_mouse_bound());
    free_mock_hw(&hw);
}

static void test_monitor_precedes_payload(void)
{
    reset_test_globals();
    struct mock_hw hw;
    init_mock_hw(&hw, 0, 0x14, 0);

    s_hub_slot_id = 1;
    struct xhci_slot *hub = setup_mock_slot(&hw, 1, 1);
    hub->is_hub = true;
    hub->hub_ss = false;
    hub->hub_nports = 4;
    hub->hub_status_dci = 3;
    setup_intr_ep(&hw, 1, 3, 2);

    struct xhci_slot *dev2 = setup_mock_slot(&hw, 2, 1);
    dev2->loc.depth = 1;
    dev2->loc.parent_slot = 1;
    dev2->loc.parent_port = 2;
    dev2->loc.route = 2;
    dev2->hid_candidates[XHCI_HID_KEYBOARD] = (struct xhci_hid_candidate){
        .valid = true, .iface = 0, .dci = 3, .ep_addr = 0x81, .interval = 10, .mps = 8
    };
    dev2->hid_candidates[XHCI_HID_MOUSE] = (struct xhci_hid_candidate){
        .valid = true, .iface = 1, .dci = 5, .ep_addr = 0x82, .interval = 10, .mps = 4
    };
    setup_intr_ep(&hw, 2, 3, 8);
    setup_intr_ep(&hw, 2, 5, 4);

    assert_eq(0, xhci_hid__test_init());
    assert_true(xhci_hid_mouse_bound());
    s_hub_ep0_writes = 0;

    uint64_t t0 = 100000000ULL;
    xhci_hid_service_once(t0);
    assert_eq(1U, s_db_writes[1][3]);
    assert_eq(1U, s_db_writes[2][3]);
    assert_eq(1U, s_db_writes[2][5]);

    uint8_t kbd_press[8] = { 0, 0, 0x04, 0, 0, 0, 0, 0 };
    uint8_t mouse_press[4] = { 0x01, 2, 3, 0 };
    hw_push_intr_event(&hw, 2, 3, kbd_press, 8, XHCI_COMP_SUCCESS);
    hw_push_intr_event(&hw, 2, 5, mouse_press, 4, XHCI_COMP_SUCCESS);
    xhci_hid_service_once(t0 + 10000000ULL);
    assert_eq(1U, s_kbd_deliveries);
    assert_eq(1U, s_mouse_deliveries);

    /* Irrelevant port bit (port 4 = 0x10): no release, no Hub EP0, 100ms throttle */
    uint8_t irrel_bitmap[1] = { 0x10 };
    hw_push_intr_event(&hw, 1, 3, irrel_bitmap, 1, XHCI_COMP_SHORT_TX);
    uint64_t t_irrel = t0 + 20000000ULL;
    xhci_hid_service_once(t_irrel);
    assert_eq(0U, s_hub_ep0_writes);
    assert_eq(0U, s_kbd_release_deliveries);
    assert_eq(0U, s_mouse_release_deliveries);
    assert_eq(1U, s_db_writes[1][3]);

    xhci_hid_service_once(t_irrel + 50000000ULL);
    assert_eq(1U, s_db_writes[1][3]);

    xhci_hid_service_once(t_irrel + 100000000ULL);
    assert_eq(2U, s_db_writes[1][3]);

    /* 64-event saturation: 1 HID event + 63 dummy events BEFORE Hub change (port 2 = 1<<2) */
    uint8_t kbd_b[8] = { 0, 0, 0x05, 0, 0, 0, 0, 0 };
    hw_push_intr_event(&hw, 2, 3, kbd_b, 8, XHCI_COMP_SUCCESS);
    for (int i = 0; i < 63; i++) {
        hw_push_event(&hw, 0, (XHCI_COMP_SUCCESS << 24),
                      (1U << 24) | (XHCI_TRB_TYPE_PORT_STATUS_EVT << 10));
    }
    uint8_t port2_chg[1] = { (uint8_t)(1U << 2) };
    hw_push_intr_event(&hw, 1, 3, port2_chg, 1, XHCI_COMP_SHORT_TX);

    uint32_t kbd_before_sat = s_kbd_deliveries;
    xhci_hid_service_once(t_irrel + 110000000ULL);
    assert_eq(kbd_before_sat, s_kbd_deliveries);

    xhci_hid_service_once(t_irrel + 120000000ULL);
    assert_eq(1U, s_kbd_release_deliveries);
    assert_eq(1U, s_mouse_release_deliveries);
    assert_eq(kbd_before_sat + 1U, s_kbd_deliveries);
    assert_eq(0U, s_hub_ep0_writes);

    free_mock_hw(&hw);
}

static void test_transport_loss_and_teardown(void)
{
    reset_test_globals();
    struct mock_hw hw;
    init_mock_hw(&hw, 0, 0x14, 0);

    struct xhci_slot *s1 = setup_mock_slot(&hw, 1, 1);
    s1->hid_candidates[XHCI_HID_KEYBOARD] = (struct xhci_hid_candidate){
        .valid = true, .iface = 0, .dci = 3, .ep_addr = 0x81, .interval = 10, .mps = 8
    };
    setup_intr_ep(&hw, 1, 3, 8);

    assert_eq(0, xhci_hid__test_init());

    xhci_hid_service_once(10000000ULL);
    uint8_t kbd_press[8] = { 0, 0, 0x04, 0, 0, 0, 0, 0 };
    hw_push_intr_event(&hw, 1, 3, kbd_press, 8, XHCI_COMP_SUCCESS);
    xhci_hid_service_once(20000000ULL);
    assert_eq(1U, s_kbd_deliveries);

    /* Busy controller (io_lock held): service_once skips */
    hw.ctrl.io_lock.owner = 1;
    uint32_t db_before = s_db_writes[1][3];
    xhci_hid_service_once(30000000ULL);
    assert_eq(db_before, s_db_writes[1][3]);
    hw.ctrl.io_lock.owner = 0;

    /* Root port CSC set while HID completion is also pending: retire & no payload */
    uint8_t kbd_c[8] = { 0, 0, 0x06, 0, 0, 0, 0, 0 };
    hw_push_intr_event(&hw, 1, 3, kbd_c, 8, XHCI_COMP_SUCCESS);
    volatile uint32_t *portsc1 = (volatile uint32_t *)(hw.op_regs + 0x400);
    *portsc1 |= XHCI_PORTSC_CSC;

    xhci_hid_service_once(40000000ULL);
    assert_eq(1U, s_kbd_release_deliveries);
    assert_eq(2U, s_kbd_deliveries);

    xhci_hid_service_once(50000000ULL);
    xhci_hid_detach_controller(&hw.ctrl);
    assert_eq(1U, s_kbd_release_deliveries);
    free_mock_hw(&hw);

    /* Controller detach during held mouse button -> single release, no UAF */
    reset_test_globals();
    init_mock_hw(&hw, 0, 0x14, 0);
    s1 = setup_mock_slot(&hw, 1, 1);
    s1->hid_candidates[XHCI_HID_MOUSE] = (struct xhci_hid_candidate){
        .valid = true, .iface = 0, .dci = 3, .ep_addr = 0x81, .interval = 10, .mps = 4
    };
    setup_intr_ep(&hw, 1, 3, 4);
    assert_eq(0, xhci_hid__test_init());
    xhci_hid_service_once(10000000ULL);
    uint8_t mouse_press[4] = { 0x01, 0, 0, 0 };
    hw_push_intr_event(&hw, 1, 3, mouse_press, 4, XHCI_COMP_SUCCESS);
    xhci_hid_service_once(20000000ULL);
    assert_eq(1U, s_mouse_deliveries);

    xhci_hid_detach_controller(&hw.ctrl);
    assert_eq(1U, s_mouse_release_deliveries);
    free_mock_hw(&hw);
    s_active_ctrl_count = 0;
    s_hw_count = 0;
    xhci_hid_service_once(30000000ULL);
    assert_eq(1U, s_mouse_release_deliveries);

    /* Hub monitor bit0 fail-closed scoped to Hub only; root port device survives */
    reset_test_globals();
    init_mock_hw(&hw, 0, 0x14, 0);
    s_hub_slot_id = 1;
    struct xhci_slot *hub = setup_mock_slot(&hw, 1, 1);
    hub->is_hub = true;
    hub->hub_ss = false;
    hub->hub_nports = 8; /* min_bytes = ceil(9/8) = 2 */
    hub->hub_status_dci = 3;
    setup_intr_ep(&hw, 1, 3, 2);

    struct xhci_slot *hub_kbd = setup_mock_slot(&hw, 2, 1);
    hub_kbd->loc.depth = 1;
    hub_kbd->loc.parent_slot = 1;
    hub_kbd->loc.parent_port = 1;
    hub_kbd->hid_candidates[XHCI_HID_KEYBOARD] = (struct xhci_hid_candidate){
        .valid = true, .iface = 0, .dci = 3, .ep_addr = 0x81, .interval = 10, .mps = 8
    };
    setup_intr_ep(&hw, 2, 3, 8);

    struct xhci_slot *root_mouse = setup_mock_slot(&hw, 3, 2);
    root_mouse->hid_candidates[XHCI_HID_MOUSE] = (struct xhci_hid_candidate){
        .valid = true, .iface = 0, .dci = 3, .ep_addr = 0x81, .interval = 10, .mps = 4
    };
    setup_intr_ep(&hw, 3, 3, 4);

    assert_eq(0, xhci_hid__test_init());
    xhci_hid_service_once(10000000ULL);
    hw_push_intr_event(&hw, 2, 3, kbd_press, 8, XHCI_COMP_SUCCESS);
    hw_push_intr_event(&hw, 3, 3, mouse_press, 4, XHCI_COMP_SUCCESS);
    xhci_hid_service_once(20000000ULL);
    assert_eq(1U, s_kbd_deliveries);
    assert_eq(1U, s_mouse_deliveries);

    /* 1-byte short bitmap for 8-port Hub (min_bytes=2) -> fail-closed for hub_kbd, NOT root_mouse */
    uint8_t short_bitmap[1] = { 0x00 };
    hw_push_intr_event(&hw, 1, 3, short_bitmap, 1, XHCI_COMP_SHORT_TX);
    xhci_hid_service_once(30000000ULL);
    assert_eq(1U, s_kbd_release_deliveries);
    assert_eq(0U, s_mouse_release_deliveries);

    /* Root mouse still works after Hub fail-closed */
    uint8_t mouse_move[4] = { 0x01, 5, 0, 0 };
    hw_push_intr_event(&hw, 3, 3, mouse_move, 4, XHCI_COMP_SUCCESS);
    xhci_hid_service_once(40000000ULL);
    assert_eq(2U, s_mouse_deliveries);
    assert_eq(5, s_last_mouse_ev.dx);
    free_mock_hw(&hw);

    /* Root port initial CSC cleared before reset without writing PED */
    reset_test_globals();
    init_mock_hw(&hw, 0, 0x14, 0);
    volatile uint32_t *portsc = (volatile uint32_t *)(hw.op_regs + 0x400);
    *portsc = XHCI_PORTSC_PP | XHCI_PORTSC_CCS | XHCI_PORTSC_PED | XHCI_PORTSC_CSC | XHCI_PORTSC_PRC | (3U << 10);
    uint8_t spd = 0;
    assert_eq(0, xhci_port_reset(&hw.ctrl, 1, &spd));
    assert_true((*portsc & XHCI_PORTSC_PED) != 0);
    free_mock_hw(&hw);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_bind_and_fail_closed),
    TEST_ENTRY(test_monitor_precedes_payload),
    TEST_ENTRY(test_transport_loss_and_teardown),
TEST_LIST_END

int main(void)
{
    TEST_SUITE("xhci_hid_service");
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}
