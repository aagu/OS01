/* @os01-test
 * id: test_xhci_hid_desc
 * layer: host
 * subsystem: drivers
 * timing: strict
 * tier: nightly
 * link: xhci-hid-desc
 * scenarios: [drivers.xhci.hid_desc]
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

static volatile uint32_t *s_mock_db_ptr = NULL;

void mock_xhci_write32(volatile void *addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
    (void)s_mock_db_ptr;
}

uint32_t mock_xhci_read32(volatile const void *addr)
{
    return *(volatile const uint32_t *)addr;
}

uint64_t mock_xhci_read64(volatile const void *addr)
{
    return *(volatile const uint64_t *)addr;
}

void mock_xhci_write64(volatile void *addr, uint64_t val)
{
    *(volatile uint64_t *)addr = val;
}

uint64_t alloc_4k_page(void)
{
    void *p = aligned_alloc(4096, 4096);
    if (p) memset(p, 0, 4096);
    return (uint64_t)(uintptr_t)p;
}

void free_4k_page(uint64_t phys)
{
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

static void setup_mock_ctrl_and_slot(struct xhci_controller *ctrl,
                                     struct xhci_slot *slot,
                                     struct xhci_trb *cmd_ring,
                                     struct xhci_trb *event_ring,
                                     uint32_t *db_regs,
                                     uint8_t *in_ctx,
                                     uint8_t *dev_ctx,
                                     uint8_t speed_class)
{
    memset(ctrl, 0, sizeof(*ctrl));
    memset(slot, 0, sizeof(*slot));
    memset(cmd_ring, 0, 4096);
    memset(event_ring, 0, 4096);
    memset(db_regs, 0, 64);
    memset(in_ctx, 0, 4096);
    memset(dev_ctx, 0, 4096);

    ctrl->cmd_ring_dma.cpu = cmd_ring;
    ctrl->cmd_ring_dma.bus = (uint64_t)(uintptr_t)cmd_ring;
    ctrl->cmd_ring_cycle = 1;
    ctrl->event_ring_dma.cpu = event_ring;
    ctrl->event_ring_dma.bus = (uint64_t)(uintptr_t)event_ring;
    ctrl->event_ring_cycle = 1;
    ctrl->db_regs = (volatile uint8_t *)db_regs;
    ctrl->rt_regs = (volatile uint8_t *)db_regs; /* unused in configure_endpoints */
    ctrl->ac64 = true;
    ctrl->ac64_valid = true;
    ctrl->running = true;
    s_mock_db_ptr = &db_regs[0];

    slot->ctrl = ctrl;
    slot->slot_id = 1;
    slot->port_num = 1;
    slot->speed = speed_class;
    slot->loc.root_port = 1;
    slot->loc.speed_id = speed_class;
    slot->loc.speed_class = speed_class;
    slot->active = true;
    slot->addr_completed = true;
    slot->input_ctx_dma.cpu = in_ctx;
    slot->input_ctx_dma.bus = (uint64_t)(uintptr_t)in_ctx;
    slot->dev_ctx_dma.cpu = dev_ctx;
    slot->dev_ctx_dma.bus = (uint64_t)(uintptr_t)dev_ctx;
    ctrl->slots[1] = slot;
}

static void prepare_cmd_success_event(struct xhci_trb *cmd_ring,
                                      struct xhci_trb *event_ring,
                                      uint32_t cmd_idx,
                                      uint32_t evt_idx)
{
    event_ring[evt_idx].parameter = (uint64_t)(uintptr_t)&cmd_ring[cmd_idx];
    event_ring[evt_idx].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[evt_idx].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1U;
}

static void test_hid_scan_config(void)
{
    struct xhci_hid_candidate out[XHCI_HID_KINDS];
    uint32_t skip = 0;
    int rc;

    /* 1. Non-Boot iface 0 (0x81->DCI 3), Kbd iface 3 alt 0 (0x82->DCI 5),
     *    Kbd iface 3 alt 1 (0x84->DCI 9), Mouse iface 8 alt 0 (0x83->DCI 7). */
    const uint8_t mixed_cfg[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 91, 0, 3, 1, 0, 0xA0, 50,
        /* iface 0: non-Boot HID */
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, USB_CLASS_HID, 0, 0, 0,
        9, USB_DESC_TYPE_HID, 0x11, 0x01, 0, 1, 0x22, 32, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 3, 8, 0, 10,
        /* iface 3 alt 0: Boot Keyboard */
        9, USB_DESC_TYPE_INTERFACE, 3, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_KEYBOARD, 0,
        9, USB_DESC_TYPE_HID, 0x11, 0x01, 0, 1, 0x22, 64, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x82, 3, 8, 0, 10,
        /* iface 3 alt 1: inactive alternate setting */
        9, USB_DESC_TYPE_INTERFACE, 3, 1, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_KEYBOARD, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x84, 3, 8, 0, 10,
        /* iface 8 alt 0: Boot Mouse */
        9, USB_DESC_TYPE_INTERFACE, 8, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_MOUSE, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x83, 3, 4, 0, 8,
    };
    rc = xhci_hid_scan_config(mixed_cfg, sizeof(mixed_cfg), XHCI_PORT_SPEED_FULL, out, &skip);
    assert_eq(0, rc);
    assert_true(out[XHCI_HID_KEYBOARD].valid);
    assert_eq(3, out[XHCI_HID_KEYBOARD].iface);
    assert_eq(5, out[XHCI_HID_KEYBOARD].dci);
    assert_eq(0x82, out[XHCI_HID_KEYBOARD].ep_addr);
    assert_eq(8, out[XHCI_HID_KEYBOARD].mps);
    assert_eq(10, out[XHCI_HID_KEYBOARD].interval);

    assert_true(out[XHCI_HID_MOUSE].valid);
    assert_eq(8, out[XHCI_HID_MOUSE].iface);
    assert_eq(7, out[XHCI_HID_MOUSE].dci);
    assert_eq(0x83, out[XHCI_HID_MOUSE].ep_addr);
    assert_eq(4, out[XHCI_HID_MOUSE].mps);
    assert_eq(8, out[XHCI_HID_MOUSE].interval);

    assert_true((skip & (1U << 3)) != 0);
    assert_true((skip & (1U << 9)) != 0);
    assert_true((skip & (1U << 5)) == 0);
    assert_true((skip & (1U << 7)) == 0);

    /* 2. Out-of-order interfaces: select lowest valid interface number per kind */
    const uint8_t ooo_cfg[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 73, 0, 4, 1, 0, 0xA0, 50,
        9, USB_DESC_TYPE_INTERFACE, 5, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_KEYBOARD, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 3, 8, 0, 10,
        9, USB_DESC_TYPE_INTERFACE, 2, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_KEYBOARD, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x82, 3, 8, 0, 10,
        9, USB_DESC_TYPE_INTERFACE, 7, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_MOUSE, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x83, 3, 4, 0, 10,
        9, USB_DESC_TYPE_INTERFACE, 4, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_MOUSE, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x84, 3, 3, 0, 10,
    };
    rc = xhci_hid_scan_config(ooo_cfg, sizeof(ooo_cfg), XHCI_PORT_SPEED_FULL, out, &skip);
    assert_eq(0, rc);
    assert_true(out[XHCI_HID_KEYBOARD].valid);
    assert_eq(2, out[XHCI_HID_KEYBOARD].iface);
    assert_eq(5, out[XHCI_HID_KEYBOARD].dci);
    assert_true(out[XHCI_HID_MOUSE].valid);
    assert_eq(4, out[XHCI_HID_MOUSE].iface);
    assert_eq(9, out[XHCI_HID_MOUSE].dci);
    assert_true((skip & (1U << 3)) != 0);
    assert_true((skip & (1U << 7)) != 0);

    /* 3. Low-numbered undersized (Kbd MPS=7, Mouse MPS=2) rejected -> high-numbered selected */
    const uint8_t size_first_cfg[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 73, 0, 4, 1, 0, 0xA0, 50,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_KEYBOARD, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 3, 7, 0, 10,
        9, USB_DESC_TYPE_INTERFACE, 1, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_MOUSE, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x82, 3, 2, 0, 10,
        9, USB_DESC_TYPE_INTERFACE, 2, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_KEYBOARD, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x83, 3, 8, 0, 10,
        9, USB_DESC_TYPE_INTERFACE, 3, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_MOUSE, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x84, 3, 3, 0, 10,
    };
    rc = xhci_hid_scan_config(size_first_cfg, sizeof(size_first_cfg), XHCI_PORT_SPEED_LOW, out, &skip);
    assert_eq(0, rc);
    assert_true(out[XHCI_HID_KEYBOARD].valid);
    assert_eq(2, out[XHCI_HID_KEYBOARD].iface);
    assert_eq(7, out[XHCI_HID_KEYBOARD].dci);
    assert_true(out[XHCI_HID_MOUSE].valid);
    assert_eq(3, out[XHCI_HID_MOUSE].iface);
    assert_eq(9, out[XHCI_HID_MOUSE].dci);

    /* 4. Invalid HID control configs: ep0, reserved addr bits, dup DCI, ep count, type, MPS, interval */
    const uint8_t ep0_hid[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 25, 0, 1, 1, 0, 0xA0, 50,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_KEYBOARD, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x80, 3, 8, 0, 10,
    };
    rc = xhci_hid_scan_config(ep0_hid, sizeof(ep0_hid), XHCI_PORT_SPEED_FULL, out, &skip);
    assert_eq(0, rc);
    assert_false(out[XHCI_HID_KEYBOARD].valid);

    const uint8_t rsvd_addr_hid[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 25, 0, 1, 1, 0, 0xA0, 50,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_KEYBOARD, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x91, 3, 8, 0, 10,
    };
    rc = xhci_hid_scan_config(rsvd_addr_hid, sizeof(rsvd_addr_hid), XHCI_PORT_SPEED_FULL, out, &skip);
    assert_eq(0, rc);
    assert_false(out[XHCI_HID_KEYBOARD].valid);

    const uint8_t dup_dci_hid[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 41, 0, 2, 1, 0, 0xA0, 50,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_KEYBOARD, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 3, 8, 0, 10,
        9, USB_DESC_TYPE_INTERFACE, 1, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_MOUSE, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 3, 4, 0, 10,
    };
    rc = xhci_hid_scan_config(dup_dci_hid, sizeof(dup_dci_hid), XHCI_PORT_SPEED_FULL, out, &skip);
    assert_true(rc == -EINVAL || (!out[XHCI_HID_KEYBOARD].valid && !out[XHCI_HID_MOUSE].valid));

    const uint8_t bad_ep_cnt_hid[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 32, 0, 1, 1, 0, 0xA0, 50,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 2, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_KEYBOARD, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 3, 8, 0, 10,
        7, USB_DESC_TYPE_ENDPOINT, 0x02, 3, 8, 0, 10,
    };
    rc = xhci_hid_scan_config(bad_ep_cnt_hid, sizeof(bad_ep_cnt_hid), XHCI_PORT_SPEED_FULL, out, &skip);
    assert_eq(0, rc);
    assert_false(out[XHCI_HID_KEYBOARD].valid);

    const uint8_t bulk_type_hid[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 25, 0, 1, 1, 0, 0xA0, 50,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_KEYBOARD, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 2, 8, 0, 10,
    };
    rc = xhci_hid_scan_config(bulk_type_hid, sizeof(bulk_type_hid), XHCI_PORT_SPEED_FULL, out, &skip);
    assert_eq(0, rc);
    assert_false(out[XHCI_HID_KEYBOARD].valid);

    /* LS MPS=9 (>8), HS tx_per_uframe!=0 (0x0808), LS bInterval=9 (<10), HS bInterval=17 (>16) */
    const uint8_t bad_ls_mps[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 25, 0, 1, 1, 0, 0xA0, 50,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_KEYBOARD, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 3, 9, 0, 10,
    };
    assert_eq(0, xhci_hid_scan_config(bad_ls_mps, sizeof(bad_ls_mps), XHCI_PORT_SPEED_LOW, out, &skip));
    assert_false(out[XHCI_HID_KEYBOARD].valid);

    const uint8_t bad_mps_bits[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 25, 0, 1, 1, 0, 0xA0, 50,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_KEYBOARD, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 3, 0x08, 0x08, 4,
    };
    assert_eq(0, xhci_hid_scan_config(bad_mps_bits, sizeof(bad_mps_bits), XHCI_PORT_SPEED_HIGH, out, &skip));
    assert_false(out[XHCI_HID_KEYBOARD].valid);

    /* LS bInterval < 10 (real-world quirk) is accepted and clamped to 10 */
    const uint8_t quirk_ls_intv[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 25, 0, 1, 1, 0, 0xA0, 50,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_KEYBOARD, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 3, 8, 0, 9,
    };
    assert_eq(0, xhci_hid_scan_config(quirk_ls_intv, sizeof(quirk_ls_intv), XHCI_PORT_SPEED_LOW, out, &skip));
    assert_true(out[XHCI_HID_KEYBOARD].valid);
    assert_eq(10, out[XHCI_HID_KEYBOARD].interval);

    const uint8_t bad_hs_intv[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 25, 0, 1, 1, 0, 0xA0, 50,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_KEYBOARD, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 3, 8, 0, 17,
    };
    assert_eq(0, xhci_hid_scan_config(bad_hs_intv, sizeof(bad_hs_intv), XHCI_PORT_SPEED_HIGH, out, &skip));
    assert_false(out[XHCI_HID_KEYBOARD].valid);

    /* Out-of-bounds / corrupt wTotalLength / orphan endpoint -> -EINVAL */
    const uint8_t bad_total_len[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 99, 0, 1, 1, 0, 0xA0, 50,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_KEYBOARD, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 3, 8, 0, 10,
    };
    assert_eq(-EINVAL, xhci_hid_scan_config(bad_total_len, sizeof(bad_total_len), XHCI_PORT_SPEED_FULL, out, &skip));

    const uint8_t orphan_ep[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 16, 0, 1, 1, 0, 0xA0, 50,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 3, 8, 0, 10,
    };
    assert_eq(-EINVAL, xhci_hid_scan_config(orphan_ep, sizeof(orphan_ep), XHCI_PORT_SPEED_FULL, out, &skip));

    /* 5. Configure Endpoint: higher invalid HID DCI (DCI 9) excluded from Add Flags,
     *    Context Entries == highest active DCI (3); all filtered -> Context Entries == 1 */
    struct xhci_controller ctrl;
    struct xhci_slot slot;
    uint8_t cmd_ring[4096] __attribute__((aligned(64)));
    uint8_t event_ring[4096] __attribute__((aligned(64)));
    uint32_t db_regs[16];
    uint8_t in_ctx[4096] __attribute__((aligned(64)));
    uint8_t dev_ctx[4096] __attribute__((aligned(64)));

    setup_mock_ctrl_and_slot(&ctrl, &slot, (struct xhci_trb *)cmd_ring,
                             (struct xhci_trb *)event_ring, db_regs,
                             in_ctx, dev_ctx, XHCI_PORT_SPEED_FULL);

    const uint8_t higher_invalid_hid[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 41, 0, 2, 1, 0, 0xA0, 50,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_KEYBOARD, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 3, 8, 0, 10, /* DCI 3 */
        9, USB_DESC_TYPE_INTERFACE, 1, 0, 1, USB_CLASS_HID, 0, 0, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x84, 3, 8, 0, 10, /* non-Boot DCI 9 */
    };
    assert_eq(0, xhci_setup_endpoint_rings(&slot, higher_invalid_hid, sizeof(higher_invalid_hid)));
    assert_eq(3, slot.max_dci);
    prepare_cmd_success_event((struct xhci_trb *)cmd_ring, (struct xhci_trb *)event_ring, 0, 0);
    assert_eq(0, xhci_configure_endpoints(&ctrl, 1));
    uint32_t *ctrl_ctx = (uint32_t *)xhci_input_ctrl_ctx(in_ctx, 0);
    uint32_t *slot_ctx = (uint32_t *)xhci_input_slot_ctx(in_ctx, 0);
    assert_eq((1U << 0) | (1U << 3), ctrl_ctx[1]);
    assert_true((ctrl_ctx[1] & (1U << 9)) == 0);
    assert_eq(3U, (slot_ctx[0] >> 27) & 0x1FU);
    xhci_free_endpoint_rings(&slot);

    /* All non-EP0 filtered -> Context Entries == 1 */
    setup_mock_ctrl_and_slot(&ctrl, &slot, (struct xhci_trb *)cmd_ring,
                             (struct xhci_trb *)event_ring, db_regs,
                             in_ctx, dev_ctx, XHCI_PORT_SPEED_FULL);
    const uint8_t all_filtered[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 25, 0, 1, 1, 0, 0xA0, 50,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, USB_CLASS_HID, 0, 0, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x83, 3, 8, 0, 10,
    };
    assert_eq(0, xhci_setup_endpoint_rings(&slot, all_filtered, sizeof(all_filtered)));
    assert_eq(0, slot.max_dci);
    prepare_cmd_success_event((struct xhci_trb *)cmd_ring, (struct xhci_trb *)event_ring, 0, 0);
    assert_eq(0, xhci_configure_endpoints(&ctrl, 1));
    ctrl_ctx = (uint32_t *)xhci_input_ctrl_ctx(in_ctx, 0);
    slot_ctx = (uint32_t *)xhci_input_slot_ctx(in_ctx, 0);
    assert_eq(1U << 0, ctrl_ctx[1]);
    assert_eq(1U, (slot_ctx[0] >> 27) & 0x1FU);
    xhci_free_endpoint_rings(&slot);
}

static void test_mixed_msc_invalid_hid(void)
{
    struct xhci_controller ctrl;
    struct xhci_slot slot;
    uint8_t cmd_ring[4096] __attribute__((aligned(64)));
    uint8_t event_ring[4096] __attribute__((aligned(64)));
    uint32_t db_regs[16];
    uint8_t in_ctx[4096] __attribute__((aligned(64)));
    uint8_t dev_ctx[4096] __attribute__((aligned(64)));

    /* 1. Illegal interrupt MPS HID + legal MSC BOT in same cfg -> only bulk rings */
    setup_mock_ctrl_and_slot(&ctrl, &slot, (struct xhci_trb *)cmd_ring,
                             (struct xhci_trb *)event_ring, db_regs,
                             in_ctx, dev_ctx, XHCI_PORT_SPEED_HIGH);
    const uint8_t msc_bad_hid[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 48, 0, 2, 1, 0, 0x80, 50,
        /* iface 0: HID with illegal high-bandwidth MPS (0x1840) */
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_KEYBOARD, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x83, 3, 0x40, 0x18, 4,
        /* iface 1: MSC BOT */
        9, USB_DESC_TYPE_INTERFACE, 1, 0, 2, 0x08, 0x06, 0x50, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 2, 0x00, 0x02, 0, /* DCI 3 */
        7, USB_DESC_TYPE_ENDPOINT, 0x02, 2, 0x00, 0x02, 0, /* DCI 4 */
    };
    assert_eq(0, xhci_setup_endpoint_rings(&slot, msc_bad_hid, sizeof(msc_bad_hid)));
    assert_false(slot.hid_candidates[XHCI_HID_KEYBOARD].valid);
    assert_false(slot.hid_candidates[XHCI_HID_MOUSE].valid);
    assert_true(slot.msc_candidate.active);
    assert_eq(1, slot.msc_candidate.interface_num);
    assert_eq(3, slot.msc_candidate.in_dci);
    assert_eq(4, slot.msc_candidate.out_dci);
    assert_eq(4, slot.max_dci);
    assert_true(slot.ep_rings[7] == NULL);
    assert_true(slot.ep_rings[3] != NULL);
    assert_true(slot.ep_rings[4] != NULL);

    prepare_cmd_success_event((struct xhci_trb *)cmd_ring, (struct xhci_trb *)event_ring, 0, 0);
    assert_eq(0, xhci_configure_endpoints(&ctrl, 1));
    uint32_t *ctrl_ctx = (uint32_t *)xhci_input_ctrl_ctx(in_ctx, 0);
    assert_eq((1U << 0) | (1U << 3) | (1U << 4), ctrl_ctx[1]);
    xhci_free_endpoint_rings(&slot);

    /* 2. HID + Bulk DCI collision (bad HID iface 0 DCI 3 vs Bulk iface 1 DCI 3):
     *    Bulk always owns DCI 3, never preempted by bad HID. */
    setup_mock_ctrl_and_slot(&ctrl, &slot, (struct xhci_trb *)cmd_ring,
                             (struct xhci_trb *)event_ring, db_regs,
                             in_ctx, dev_ctx, XHCI_PORT_SPEED_HIGH);
    const uint8_t collision_cfg[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 48, 0, 2, 1, 0, 0x80, 50,
        /* iface 0: Boot HID on 0x81 (DCI 3) - filtered because MSC BOT present */
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_KEYBOARD, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 3, 8, 0, 4,
        /* iface 1: MSC BOT on 0x81 (DCI 3) and 0x02 (DCI 4) */
        9, USB_DESC_TYPE_INTERFACE, 1, 0, 2, 0x08, 0x06, 0x50, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 2, 0x00, 0x02, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x02, 2, 0x00, 0x02, 0,
    };
    uint32_t skip = 0;
    struct xhci_hid_candidate out[XHCI_HID_KINDS];
    assert_eq(0, xhci_hid_scan_config(collision_cfg, sizeof(collision_cfg), XHCI_PORT_SPEED_HIGH, out, &skip));
    assert_false(out[XHCI_HID_KEYBOARD].valid);
    assert_true((skip & (1U << 3)) == 0); /* Never mark non-HID occupied DCI */

    assert_eq(0, xhci_setup_endpoint_rings(&slot, collision_cfg, sizeof(collision_cfg)));
    assert_true(slot.msc_candidate.active);
    assert_true(slot.ep_rings[3] != NULL);
    assert_eq(XHCI_EP_TYPE_BULK_IN, slot.ep_rings[3]->ep_type);
    assert_eq(512, slot.ep_rings[3]->max_packet_size);
    xhci_free_endpoint_rings(&slot);
}

static void test_interrupt_context_payload(void)
{
    struct xhci_controller ctrl;
    struct xhci_slot slot;
    uint8_t cmd_ring[4096] __attribute__((aligned(64)));
    uint8_t event_ring[4096] __attribute__((aligned(64)));
    uint32_t db_regs[16];
    uint8_t in_ctx[4096] __attribute__((aligned(64)));
    uint8_t dev_ctx[4096] __attribute__((aligned(64)));

    /* 1. LS MPS 8, bInterval=10 -> Interval=6 / ESIT=8 */
    setup_mock_ctrl_and_slot(&ctrl, &slot, (struct xhci_trb *)cmd_ring,
                             (struct xhci_trb *)event_ring, db_regs,
                             in_ctx, dev_ctx, XHCI_PORT_SPEED_LOW);
    const uint8_t ls_hid[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 25, 0, 1, 1, 0, 0xA0, 50,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_KEYBOARD, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 3, 8, 0, 10,
    };
    assert_eq(0, xhci_setup_endpoint_rings(&slot, ls_hid, sizeof(ls_hid)));
    prepare_cmd_success_event((struct xhci_trb *)cmd_ring, (struct xhci_trb *)event_ring, 0, 0);
    assert_eq(0, xhci_configure_endpoints(&ctrl, 1));
    uint32_t *ep_ctx = (uint32_t *)xhci_input_ep_ctx(in_ctx, 0, 3);
    assert_eq(6U, (ep_ctx[0] >> 16) & 0xFFU);
    assert_eq(8U, (ep_ctx[4] >> 16) & 0xFFFFU);
    xhci_free_endpoint_rings(&slot);

    /* 2. FS MPS 64, bInterval=8 -> Interval=6 / ESIT=64 */
    setup_mock_ctrl_and_slot(&ctrl, &slot, (struct xhci_trb *)cmd_ring,
                             (struct xhci_trb *)event_ring, db_regs,
                             in_ctx, dev_ctx, XHCI_PORT_SPEED_FULL);
    const uint8_t fs_hid[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 25, 0, 1, 1, 0, 0xA0, 50,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_KEYBOARD, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 3, 64, 0, 8,
    };
    assert_eq(0, xhci_setup_endpoint_rings(&slot, fs_hid, sizeof(fs_hid)));
    prepare_cmd_success_event((struct xhci_trb *)cmd_ring, (struct xhci_trb *)event_ring, 0, 0);
    assert_eq(0, xhci_configure_endpoints(&ctrl, 1));
    ep_ctx = (uint32_t *)xhci_input_ep_ctx(in_ctx, 0, 3);
    assert_eq(6U, (ep_ctx[0] >> 16) & 0xFFU);
    assert_eq(64U, (ep_ctx[4] >> 16) & 0xFFFFU);
    xhci_free_endpoint_rings(&slot);

    /* 3. HS MPS 64, bInterval=4 -> Interval=3 / ESIT=64 */
    setup_mock_ctrl_and_slot(&ctrl, &slot, (struct xhci_trb *)cmd_ring,
                             (struct xhci_trb *)event_ring, db_regs,
                             in_ctx, dev_ctx, XHCI_PORT_SPEED_HIGH);
    const uint8_t hs_hid[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 25, 0, 1, 1, 0, 0xA0, 50,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, USB_CLASS_HID, USB_HID_SUBCLASS_BOOT, USB_HID_PROTO_KEYBOARD, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 3, 64, 0, 4,
    };
    assert_eq(0, xhci_setup_endpoint_rings(&slot, hs_hid, sizeof(hs_hid)));
    prepare_cmd_success_event((struct xhci_trb *)cmd_ring, (struct xhci_trb *)event_ring, 0, 0);
    assert_eq(0, xhci_configure_endpoints(&ctrl, 1));
    ep_ctx = (uint32_t *)xhci_input_ep_ctx(in_ctx, 0, 3);
    assert_eq(3U, (ep_ctx[0] >> 16) & 0xFFU);
    assert_eq(64U, (ep_ctx[4] >> 16) & 0xFFFFU);
    xhci_free_endpoint_rings(&slot);

    /* 4. Bulk Max ESIT=0 preserved */
    setup_mock_ctrl_and_slot(&ctrl, &slot, (struct xhci_trb *)cmd_ring,
                             (struct xhci_trb *)event_ring, db_regs,
                             in_ctx, dev_ctx, XHCI_PORT_SPEED_HIGH);
    const uint8_t bulk_cfg[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 32, 0, 1, 1, 0, 0x80, 50,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 2, 0x08, 0x06, 0x50, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x81, 2, 0x00, 0x02, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x02, 2, 0x00, 0x02, 0,
    };
    assert_eq(0, xhci_setup_endpoint_rings(&slot, bulk_cfg, sizeof(bulk_cfg)));
    prepare_cmd_success_event((struct xhci_trb *)cmd_ring, (struct xhci_trb *)event_ring, 0, 0);
    assert_eq(0, xhci_configure_endpoints(&ctrl, 1));
    ep_ctx = (uint32_t *)xhci_input_ep_ctx(in_ctx, 0, 3);
    assert_eq(0U, (ep_ctx[4] >> 16) & 0xFFFFU);
    xhci_free_endpoint_rings(&slot);

    /* 5. Hub status DCI taken from verified alt 0 Intr IN (0x82 -> DCI 5, not guessed 0x81),
     *    and HS Hub + LS/FS child location TT slot/port preserved. */
    setup_mock_ctrl_and_slot(&ctrl, &slot, (struct xhci_trb *)cmd_ring,
                             (struct xhci_trb *)event_ring, db_regs,
                             in_ctx, dev_ctx, XHCI_PORT_SPEED_HIGH);
    slot.is_hub = true;
    slot.slot_id = 2;
    ctrl.slots[2] = &slot;
    const uint8_t hub_ep2_cfg[] = {
        9, USB_DESC_TYPE_CONFIGURATION, 25, 0, 1, 1, 0, 0xE0, 0,
        9, USB_DESC_TYPE_INTERFACE, 0, 0, 1, USB_CLASS_HUB, 0, 0, 0,
        7, USB_DESC_TYPE_ENDPOINT, 0x82, 3, 0x40, 0x08, 12,
    };
    uint8_t hub_dci = 0;
    assert_eq(0, xhci_hub_validate_config_dci(&slot, hub_ep2_cfg, sizeof(hub_ep2_cfg), &hub_dci));
    assert_eq(5, hub_dci);
    assert_eq(0, xhci_setup_endpoint_rings(&slot, hub_ep2_cfg, sizeof(hub_ep2_cfg)));
    prepare_cmd_success_event((struct xhci_trb *)cmd_ring, (struct xhci_trb *)event_ring, 0, 0);
    assert_eq(0, xhci_configure_endpoints(&ctrl, 2));
    ep_ctx = (uint32_t *)xhci_input_ep_ctx(in_ctx, 0, 5);
    assert_eq(128U, (ep_ctx[4] >> 16) & 0xFFFFU);

    struct xhci_location child_loc;
    assert_eq(0, xhci_child_location(&ctrl, 2, 3, XHCI_PORT_SPEED_LOW, &child_loc));
    assert_eq(XHCI_PORT_SPEED_LOW, child_loc.speed_class);
    assert_eq(2, child_loc.tt_hub_slot);
    assert_eq(3, child_loc.tt_port);
    xhci_free_endpoint_rings(&slot);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_hid_scan_config),
    TEST_ENTRY(test_mixed_msc_invalid_hid),
    TEST_ENTRY(test_interrupt_context_payload),
TEST_LIST_END

int main(void)
{
    TEST_SUITE("xhci_hid_desc");
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}
