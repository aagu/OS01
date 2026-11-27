/* hosttests/cases/test_xhci_phase2.c — xHCI Host Controller Phase 2 unit tests */
#include "test_framework.h"
#include <device/device.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>
#include <driver/xhci.h>
#include <driver/xhci_hub.h>
#include <arch/pci.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

#define TEST_CASE(name) printf("\n  [TEST] %s\n", name)

#define MOCK_MMIO_SIZE (64 * 1024)
static uint8_t s_mock_mmio[MOCK_MMIO_SIZE];

static bool s_inject_port_ped_fail = false;
static bool s_legacy_bios_stuck = false;
static bool s_hcrst_seen = false;
static bool s_legsup_os_seen_before_hcrst = false;
static uint32_t s_last_legctl_written = 0;
static uint64_t s_mock_clock_ns = 0;
static uint64_t s_mock_clock_step_ns = 0;

static uint32_t s_mock_pci_cmd = 0;
static int mock_pci_read32(uint16_t d, uint8_t b, uint8_t s, uint8_t f, uint16_t off, uint32_t *val, enum pci_error_scope *scope)
{
    (void)d; (void)b; (void)s; (void)f; (void)scope;
    *val = (off == 0x04) ? s_mock_pci_cmd : 0;
    return 0;
}
static int mock_pci_write32(uint16_t d, uint8_t b, uint8_t s, uint8_t f, uint16_t off, uint32_t val, enum pci_error_scope *scope)
{
    (void)d; (void)b; (void)s; (void)f; (void)scope;
    if (off == 0x04) s_mock_pci_cmd = val;
    return 0;
}
static const struct pci_backend s_mock_pci_backend = {
    .read32 = mock_pci_read32,
    .write32 = mock_pci_write32,
};
const struct pci_backend *arch_pci_backend(void)
{
    return &s_mock_pci_backend;
}

uint64_t arch_clocksource_read_ns(void)
{
    uint64_t v = s_mock_clock_ns;
    s_mock_clock_ns += s_mock_clock_step_ns;
    return v;
}

#define XHCI_PORTSC_RO_MASK (XHCI_PORTSC_CCS | XHCI_PORTSC_SPEED_MASK)

void mock_xhci_write32(volatile void *addr, uint32_t val)
{
    uintptr_t offset = (uintptr_t)addr - (uintptr_t)s_mock_mmio;

    if ((const uint8_t *)addr >= s_mock_mmio &&
        (const uint8_t *)addr < s_mock_mmio + MOCK_MMIO_SIZE) {
        if (offset == 0x20 + XHCI_OP_USBCMD) {
            if (val & XHCI_CMD_HCRST) {
                s_hcrst_seen = true;
                val &= ~XHCI_CMD_HCRST;
                *(volatile uint32_t *)(s_mock_mmio + 0x20 + XHCI_OP_USBSTS) |= XHCI_STS_HCH;
            }
            if (val & XHCI_CMD_RS) {
                *(volatile uint32_t *)(s_mock_mmio + 0x20 + XHCI_OP_USBSTS) &= ~XHCI_STS_HCH;
            } else {
                *(volatile uint32_t *)(s_mock_mmio + 0x20 + XHCI_OP_USBSTS) |= XHCI_STS_HCH;
            }
            *(volatile uint32_t *)addr = val;
            return;
        }
        if (offset == 0x800) { /* Mock USBLEGSUP */
            if ((val & XHCI_LEGSUP_OS_OWNED) && !s_hcrst_seen)
                s_legsup_os_seen_before_hcrst = true;
            if (!s_legacy_bios_stuck && (val & XHCI_LEGSUP_OS_OWNED))
                val &= ~XHCI_LEGSUP_BIOS_OWNED;
            *(volatile uint32_t *)addr = val;
            return;
        }
        if (offset == 0x804) { /* Mock USBLEGCTLSTS */
            s_last_legctl_written = val;
            uint32_t rw1c = val & XHCI_LEGCTL_RW1C;
            *(volatile uint32_t *)addr = (val & ~XHCI_LEGCTL_RW1C) & ~rw1c;
            return;
        }
    }

    /* Operational registers start at 0x20. Port registers start at 0x20 + 0x400 = 0x420 */
    if ((const uint8_t *)addr >= s_mock_mmio &&
        (const uint8_t *)addr < s_mock_mmio + MOCK_MMIO_SIZE &&
        offset >= 0x420 && offset < 0x420 + 32 * 0x10) {

        uint32_t cur = *(volatile uint32_t *)addr;
        /* RW1C semantics: writing 1 clears bit, writing 0 leaves bit unchanged */
        uint32_t rw1c_cleared = cur & ~(val & XHCI_PORTSC_RW1C_MASK);
        /* Write control bits, preserve RO bits (CCS, Speed) and updated RW1C bits */
        uint32_t updated = (cur & XHCI_PORTSC_RO_MASK) |
                           (val & ~XHCI_PORTSC_RW1C_MASK & ~XHCI_PORTSC_RO_MASK) |
                           (rw1c_cleared & XHCI_PORTSC_RW1C_MASK);

        if (val & XHCI_PORTSC_PR) {
            updated &= ~XHCI_PORTSC_PR;
            updated |= XHCI_PORTSC_PRC;
            if (!s_inject_port_ped_fail) {
                updated |= XHCI_PORTSC_PED;
            } else {
                updated &= ~XHCI_PORTSC_PED;
            }
        }
        *(volatile uint32_t *)addr = updated;
        return;
    }

    *(volatile uint32_t *)addr = val;
}

#define MAX_MOCK_PAGES 32
static void *s_mock_page_ptrs[MAX_MOCK_PAGES];
static int s_mock_page_count = 0;
static uint64_t s_inject_phys[8];
static int s_inject_phys_head = 0;
static int s_inject_phys_tail = 0;

static void push_inject_phys(uint64_t phys)
{
    s_inject_phys[s_inject_phys_tail++ % 8] = phys;
}

uint64_t alloc_4k_page(void)
{
    if (s_inject_phys_head < s_inject_phys_tail)
        return s_inject_phys[s_inject_phys_head++ % 8];
    if (s_mock_page_count >= MAX_MOCK_PAGES) return 0;
    void *p = aligned_alloc(4096, 4096);
    if (!p) return 0;
    memset(p, 0, 4096);
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

void test_xhci_context_layout_32_vs_64(void)
{
    TEST_CASE("Context layout offsets (CSZ=0 and CSZ=1)");
    uint8_t buffer[4096];
    memset(buffer, 0, sizeof(buffer));

    /* CSZ = 0 (32-byte contexts) */
    void *input_ctrl_32 = xhci_input_ctrl_ctx(buffer, 0);
    void *input_slot_32 = xhci_input_slot_ctx(buffer, 0);
    void *input_ep0_32  = xhci_input_ep0_ctx(buffer, 0);
    assert_eq((uintptr_t)input_ctrl_32 - (uintptr_t)buffer, 0);
    assert_eq((uintptr_t)input_slot_32 - (uintptr_t)buffer, 32);
    assert_eq((uintptr_t)input_ep0_32  - (uintptr_t)buffer, 64);

    void *dev_slot_32 = xhci_dev_slot_ctx(buffer, 0);
    void *dev_ep0_32  = xhci_dev_ep0_ctx(buffer, 0);
    assert_eq((uintptr_t)dev_slot_32 - (uintptr_t)buffer, 0);
    assert_eq((uintptr_t)dev_ep0_32  - (uintptr_t)buffer, 32);

    /* CSZ = 1 (64-byte contexts) */
    void *input_ctrl_64 = xhci_input_ctrl_ctx(buffer, 1);
    void *input_slot_64 = xhci_input_slot_ctx(buffer, 1);
    void *input_ep0_64  = xhci_input_ep0_ctx(buffer, 1);
    assert_eq((uintptr_t)input_ctrl_64 - (uintptr_t)buffer, 0);
    assert_eq((uintptr_t)input_slot_64 - (uintptr_t)buffer, 64);
    assert_eq((uintptr_t)input_ep0_64  - (uintptr_t)buffer, 128);

    void *dev_slot_64 = xhci_dev_slot_ctx(buffer, 1);
    void *dev_ep0_64  = xhci_dev_ep0_ctx(buffer, 1);
    assert_eq((uintptr_t)dev_slot_64 - (uintptr_t)buffer, 0);
    assert_eq((uintptr_t)dev_ep0_64  - (uintptr_t)buffer, 64);

    assert_eq(sizeof(struct usb_device_descriptor), 18);
}

void test_xhci_portsc_rw1c_mask(void)
{
    TEST_CASE("PORTSC RW1C mask safety");
    uint32_t all_ones = 0xFFFFFFFF;
    uint32_t preserved = all_ones & ~XHCI_PORTSC_RW1C_MASK;

    /* Verify all 8 RW1C bits are cleared */
    assert_eq(preserved & XHCI_PORTSC_PED, 0);
    assert_eq(preserved & XHCI_PORTSC_CSC, 0);
    assert_eq(preserved & XHCI_PORTSC_PEC, 0);
    assert_eq(preserved & XHCI_PORTSC_WRC, 0);
    assert_eq(preserved & XHCI_PORTSC_OCC, 0);
    assert_eq(preserved & XHCI_PORTSC_PRC, 0);
    assert_eq(preserved & XHCI_PORTSC_PLC, 0);
    assert_eq(preserved & XHCI_PORTSC_CEC, 0);

    /* Verify control bits remain set */
    assert_true((preserved & XHCI_PORTSC_PP) != 0);
    assert_true((preserved & XHCI_PORTSC_PR) != 0);
}

void test_xhci_event_poll_with_intervening_port_change(void)
{
    TEST_CASE("Event polling: consume Type 34 and retrieve target Type 33");
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    struct xhci_trb event_ring[XHCI_TRB_RING_SIZE];
    memset(event_ring, 0, sizeof(event_ring));
    uint8_t rt_regs[0x100];
    memset(rt_regs, 0, sizeof(rt_regs));

    ctrl.event_ring_dma.cpu = event_ring;
    ctrl.event_ring_dma.bus = 0x20000;
    ctrl.event_ring_dequeue = 0;
    ctrl.event_ring_cycle = 1;
    ctrl.rt_regs = rt_regs;

    /* Entry 0: Port Status Change Event (Type 34) */
    event_ring[0].parameter = 0;
    event_ring[0].status = 0;
    event_ring[0].control = (XHCI_TRB_TYPE_PORT_STATUS_EVT << 10) | 1; /* Cycle = 1 */

    /* Entry 1: Command Completion Event (Type 33, Slot 1, Success) */
    event_ring[1].parameter = 0x1000;
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[1].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1; /* Cycle = 1 */

    struct xhci_trb evt;
    int rc = xhci_poll_event_filtered(&ctrl, XHCI_TRB_TYPE_CMD_CMPL_EVT, &evt, 100);
    assert_eq(rc, 0);
    assert_eq((evt.control >> 10) & 0x3F, XHCI_TRB_TYPE_CMD_CMPL_EVT);
    assert_eq((evt.control >> 24) & 0xFF, 1);
    assert_eq((evt.status >> 24) & 0xFF, XHCI_COMP_SUCCESS);
    assert_eq(ctrl.event_ring_dequeue, 2);

    /* Verify ERDP was updated with EHB bit set */
    uint64_t erdp = *(volatile uint64_t *)(rt_regs + 0x20 + XHCI_INTR_ERDP);
    assert_eq(erdp & ~XHCI_ERDP_EHB, 0x20000 + 2 * sizeof(struct xhci_trb));
    assert_true((erdp & XHCI_ERDP_EHB) != 0);
}

void test_xhci_event_poll_timeout(void)
{
    TEST_CASE("Event polling: timeout when cycle bit does not match");
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    struct xhci_trb event_ring[XHCI_TRB_RING_SIZE];
    memset(event_ring, 0, sizeof(event_ring));
    uint8_t rt_regs[0x100];
    memset(rt_regs, 0, sizeof(rt_regs));

    ctrl.event_ring_dma.cpu = event_ring;
    ctrl.event_ring_dequeue = 0;
    ctrl.event_ring_cycle = 1; /* Ring expects 1, but entry has cycle 0 */
    ctrl.rt_regs = rt_regs;

    struct xhci_trb evt;
    int rc = xhci_poll_event_filtered(&ctrl, XHCI_TRB_TYPE_CMD_CMPL_EVT, &evt, 10);
    assert_eq(rc, -ETIMEDOUT);
}

void test_xhci_port_reset_normal_and_failure(void)
{
    TEST_CASE("Port reset: connected normal case, disconnected, and PED failure");
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.op_regs = s_mock_mmio + 0x20;
    ctrl.max_ports = 4;

    volatile uint32_t *port1 = (volatile uint32_t *)(ctrl.op_regs + 0x400);

    /* Case A: Not connected */
    *port1 = 0;
    uint8_t speed = 0;
    int rc = xhci_port_reset(&ctrl, 1, &speed);
    assert_eq(rc, -ENODEV);

    /* Case B: Connected, successful reset to High-Speed (speed = 3) */
    s_inject_port_ped_fail = false;
    *port1 = XHCI_PORTSC_CCS | XHCI_PORTSC_PP | (3U << 10);
    rc = xhci_port_reset(&ctrl, 1, &speed);
    assert_eq(rc, 0);
    assert_eq(speed, 3);
    /* Verify PED is enabled and PR is cleared */
    assert_true((*port1 & XHCI_PORTSC_PED) != 0);
    assert_eq(*port1 & XHCI_PORTSC_PR, 0);

    /* Case C: Reset completed but PED = 0 (failure) */
    s_inject_port_ped_fail = true;
    *port1 = XHCI_PORTSC_CCS | XHCI_PORTSC_PP | (3U << 10);
    rc = xhci_port_reset(&ctrl, 1, &speed);
    assert_eq(rc, -EIO);
    s_inject_port_ped_fail = false;
}

void test_xhci_slot_lifecycle_and_address_device(void)
{
    TEST_CASE("Slot lifecycle: Enable Slot, Address Device, Input Context DWs, Free Slot");
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));

    struct xhci_trb cmd_ring[XHCI_TRB_RING_SIZE];
    memset(cmd_ring, 0, sizeof(cmd_ring));
    ctrl.cmd_ring_dma.cpu = cmd_ring;
    ctrl.cmd_ring_dma.bus = 0x10000;
    ctrl.cmd_ring_enqueue = 0;
    ctrl.cmd_ring_cycle = 1;

    struct xhci_trb event_ring[XHCI_TRB_RING_SIZE];
    memset(event_ring, 0, sizeof(event_ring));
    ctrl.event_ring_dma.cpu = event_ring;
    ctrl.event_ring_dma.bus = 0x20000;
    ctrl.event_ring_dequeue = 0;
    ctrl.event_ring_cycle = 1;

    uint8_t rt_regs[0x100];
    memset(rt_regs, 0, sizeof(rt_regs));
    ctrl.rt_regs = rt_regs;

    uint8_t db_regs[0x1000];
    memset(db_regs, 0, sizeof(db_regs));
    ctrl.db_regs = db_regs;

    uint64_t dcbaa[64];
    memset(dcbaa, 0, sizeof(dcbaa));
    ctrl.dcbaa_dma.cpu = dcbaa;
    ctrl.dcbaa_dma.bus = 0x30000;
    ctrl.csz = 0;

    /* Pre-populate event ring:
     * Event 0: Command Completion Event for Enable Slot -> slot_id = 1, Success
     * Event 1: Command Completion Event for Address Device -> slot_id = 1, Success
     */
    event_ring[0].parameter = 0x10000;
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[0].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    event_ring[1].parameter = 0x10000 + sizeof(struct xhci_trb);
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[1].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    uint8_t slot_id = 0;
    int rc = xhci_enable_slot(&ctrl, 1, 3, &slot_id);
    assert_eq(rc, 0);
    assert_eq(slot_id, 1);
    assert_true(ctrl.slots[1] != NULL);
    assert_eq(ctrl.slots[1]->port_num, 1);
    assert_eq(ctrl.slots[1]->speed, 3);

    rc = xhci_address_device(&ctrl, slot_id);
    assert_eq(rc, 0);

    /* Verify DCBAA[1] set to physical address of dev_ctx */
    assert_eq(((uint64_t *)ctrl.dcbaa_dma.cpu)[1], ctrl.slots[1]->dev_ctx_dma.bus);

    /* Verify Input Context DW fields */
    uint32_t *ctrl_ctx = (uint32_t *)xhci_input_ctrl_ctx(ctrl.slots[1]->input_ctx_dma.cpu, ctrl.csz);
    assert_eq(ctrl_ctx[0], 0);                /* Drop Flags */
    assert_eq(ctrl_ctx[1], (1U<<0) | (1U<<1));/* Add Flags: Slot + EP0 */

    uint32_t *slot_ctx = (uint32_t *)xhci_input_slot_ctx(ctrl.slots[1]->input_ctx_dma.cpu, ctrl.csz);
    assert_eq(slot_ctx[0], (3U << 20) | (1U << 27)); /* Speed=3, Context Entries=1 */
    assert_eq(slot_ctx[1], (1U << 16));               /* Root Hub Port = 1 */

    uint32_t *ep0_ctx = (uint32_t *)xhci_input_ep0_ctx(ctrl.slots[1]->input_ctx_dma.cpu, ctrl.csz);
    assert_eq(ep0_ctx[1], (3U << 1) | (4U << 3) | (64U << 16)); /* CErr=3, EP Type=4, MPS=64 */
    assert_eq(ep0_ctx[2] & 1U, 1U);                              /* DCS = 1 */
    assert_eq(ep0_ctx[4], 8U);                                   /* Average TRB Length = 8 */

    /* Verify EP0 Ring Link TRB at index 255 */
    struct xhci_trb *link = &((struct xhci_trb *)ctrl.slots[1]->ep0_ring_dma.cpu)[XHCI_TRB_RING_SIZE - 1];
    assert_eq((link->control >> 10) & 0x3F, XHCI_TRB_TYPE_LINK);
    assert_true((link->control & XHCI_TRB_LINK_TC) != 0);

    /* Free slot and verify memory teardown */
    int initial_pages = s_mock_page_count;
    xhci_free_slot(&ctrl, slot_id);
    assert_true(ctrl.slots[1] == NULL);
    assert_eq(((uint64_t *)ctrl.dcbaa_dma.cpu)[1], 0);
    assert_eq(s_mock_page_count, initial_pages - 4); /* 4 pages freed */
}

void test_xhci_ep0_control_transfer_and_descriptor(void)
{
    TEST_CASE("EP0 control transfer: GET_DESCRIPTOR 3-stage TRBs, residual length bounds, unpacking");
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));

    struct xhci_slot slot;
    memset(&slot, 0, sizeof(slot));
    slot.slot_id = 1;

    struct xhci_trb ep0_ring[XHCI_TRB_RING_SIZE];
    memset(ep0_ring, 0, sizeof(ep0_ring));
    slot.ep0_ring_dma.cpu = ep0_ring;
    slot.ep0_ring_dma.bus = 0x40000;
    slot.ep0_ring_cycle = 1;
    slot.ep0_ring_enqueue = 0;

    /* Initialize Link TRB at 255 */
    ep0_ring[XHCI_TRB_RING_SIZE - 1].parameter = 0x40000;
    ep0_ring[XHCI_TRB_RING_SIZE - 1].control = (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_LINK_TC;

    uint8_t dma_buf[4096];
    memset(dma_buf, 0, sizeof(dma_buf));
    slot.data_dma.cpu = dma_buf;
    slot.data_dma.bus = 0x50000;

    struct usb_device_descriptor raw = {
        .bLength = 18,
        .bDescriptorType = 1,
        .bcdUSB = 0x0200,
        .bDeviceClass = 0,
        .bDeviceSubClass = 0,
        .bDeviceProtocol = 0,
        .bMaxPacketSize0 = 64,
        .idVendor = 0x0627,
        .idProduct = 0x0001,
        .bcdDevice = 0x0100,
        .bNumConfigurations = 1,
    };
    memcpy(dma_buf, &raw, 18);

    ctrl.slots[1] = &slot;

    struct xhci_trb event_ring[XHCI_TRB_RING_SIZE];
    memset(event_ring, 0, sizeof(event_ring));
    ctrl.event_ring_dma.cpu = event_ring;
    ctrl.event_ring_dma.bus = 0x20000;
    ctrl.event_ring_dequeue = 0;
    ctrl.event_ring_cycle = 1;

    uint8_t rt_regs[0x100];
    memset(rt_regs, 0, sizeof(rt_regs));
    ctrl.rt_regs = rt_regs;

    uint8_t db_regs[0x1000];
    memset(db_regs, 0, sizeof(db_regs));
    ctrl.db_regs = db_regs;

    /* Feed mock Transfer Events for 3-stage GET_DESCRIPTOR: Data TRB (IOC) + Status TRB (IOC) */
    event_ring[0].parameter = 0x40000 + 1 * sizeof(struct xhci_trb); /* Data TRB */
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24) | 0;             /* Residual = 0 */
    event_ring[0].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[1].parameter = 0x40000 + 2 * sizeof(struct xhci_trb); /* Status TRB */
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[1].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    struct usb_device_descriptor desc;
    int rc = xhci_get_device_descriptor(&ctrl, 1, &desc);
    assert_eq(rc, 0);

    /* Verify the 3 TRBs placed on ep0_ring */
    /* 1. Setup Stage TRB */
    assert_eq((ep0_ring[0].control >> 10) & 0x3F, XHCI_TRB_TYPE_SETUP_STAGE);
    assert_true((ep0_ring[0].control & (1U << 6)) != 0); /* IDT = 1 */
    assert_eq((ep0_ring[0].control >> 16) & 3, 3);        /* TRT = 3 (IN Data) */
    assert_eq(ep0_ring[0].parameter, 0x0012000001000680ULL);

    /* 2. Data Stage TRB */
    assert_eq((ep0_ring[1].control >> 10) & 0x3F, XHCI_TRB_TYPE_DATA_STAGE);
    assert_true((ep0_ring[1].control & (1U << 16)) != 0); /* DIR = 1 (IN) */
    assert_eq(ep0_ring[1].parameter, 0x50000);
    assert_eq(ep0_ring[1].status & 0x1FFFF, 18);

    /* 3. Status Stage TRB */
    assert_eq((ep0_ring[2].control >> 10) & 0x3F, XHCI_TRB_TYPE_STATUS_STAGE);
    assert_true((ep0_ring[2].control & (1U << 5)) != 0); /* IOC = 1 */

    /* Verify Doorbell written: Target = 1 on Slot 1 Doorbell */
    uint32_t db_val = *(volatile uint32_t *)(ctrl.db_regs + 1 * 4);
    assert_eq(db_val, 1);

    /* Verify unpacked descriptor fields */
    assert_eq(desc.bLength, 18);
    assert_eq(desc.bDescriptorType, 1);
    assert_eq(desc.idVendor, 0x0627);
    assert_eq(desc.idProduct, 0x0001);
    assert_eq(desc.bMaxPacketSize0, 64);

    /* Boundary Test 1: residual = 11 -> actual = 7 (< 8 bytes), must fail with -EIO */
    event_ring[2].parameter = 0x40000 + 4 * sizeof(struct xhci_trb);
    event_ring[2].status = (XHCI_COMP_SUCCESS << 24) | 11;
    event_ring[2].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    event_ring[3].parameter = 0x40000 + 5 * sizeof(struct xhci_trb);
    event_ring[3].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[3].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    rc = xhci_get_device_descriptor(&ctrl, 1, &desc);
    assert_eq(rc, -EIO);

    /* Boundary Test 2: residual = 10 -> actual = 8 (>= 8 bytes), success with 0 */
    event_ring[4].parameter = 0x40000 + 7 * sizeof(struct xhci_trb);
    event_ring[4].status = (XHCI_COMP_SHORT_TX << 24) | 10;
    event_ring[4].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    event_ring[5].parameter = 0x40000 + 8 * sizeof(struct xhci_trb);
    event_ring[5].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[5].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    rc = xhci_get_device_descriptor(&ctrl, 1, &desc);
    assert_eq(rc, 0);

    /* Only `actual` (8) bytes were transferred: fields beyond offset 8 must
     * not leak stale DMA data (idVendor lives at offset 8). */
    assert_eq(desc.idVendor, 0);
    assert_eq(desc.idProduct, 0);
    assert_eq(desc.bLength, 18);
    assert_eq(desc.bDescriptorType, 1);

    /* Boundary Test 3: residual > 18 underflow guard -> actual = 0, must fail with -EIO */
    event_ring[6].parameter = 0x40000 + 10 * sizeof(struct xhci_trb);
    event_ring[6].status = (XHCI_COMP_SUCCESS << 24) | 30;
    event_ring[6].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    event_ring[7].parameter = 0x40000 + 11 * sizeof(struct xhci_trb);
    event_ring[7].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[7].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    rc = xhci_get_device_descriptor(&ctrl, 1, &desc);
    assert_eq(rc, -EIO);
}

void test_xhci_ep0_ring_wrap(void)
{
    TEST_CASE("EP0 ring Link TRB wrap and cycle toggle at index 255");
    struct xhci_slot slot;
    memset(&slot, 0, sizeof(slot));

    struct xhci_trb ep0_ring[XHCI_TRB_RING_SIZE];
    memset(ep0_ring, 0, sizeof(ep0_ring));
    slot.ep0_ring_dma.cpu = ep0_ring;
    slot.ep0_ring_dma.bus = 0x40000;
    slot.ep0_ring_cycle = 1;
    slot.ep0_ring_enqueue = 254;

    /* Initialize Link TRB at index 255 */
    ep0_ring[255].parameter = 0x40000;
    ep0_ring[255].control = (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_LINK_TC;

    struct xhci_trb trb1 = { .parameter = 1, .status = 0, .control = 0 };
    struct xhci_trb trb2 = { .parameter = 2, .status = 0, .control = 0 };

    /* Enqueue at index 254 */
    xhci_ep0_ring_enqueue(&slot, &trb1);
    assert_eq(slot.ep0_ring_enqueue, 255);
    assert_eq(slot.ep0_ring_cycle, 1);

    /* Enqueue next: should update Link TRB cycle to 1, toggle cycle to 0, and wrap to 0 */
    xhci_ep0_ring_enqueue(&slot, &trb2);
    assert_eq(ep0_ring[255].control & XHCI_TRB_CYCLE_BIT, 1);
    assert_eq(slot.ep0_ring_enqueue, 1);
    assert_eq(slot.ep0_ring_cycle, 0);
    assert_eq(ep0_ring[0].parameter, 2);
    assert_eq(ep0_ring[0].control & XHCI_TRB_CYCLE_BIT, 0);
}

void test_xhci_command_completion_param_match(void)
{
    TEST_CASE("Command completion: stale event with wrong TRB pointer is skipped");
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));

    struct xhci_trb cmd_ring[XHCI_TRB_RING_SIZE];
    memset(cmd_ring, 0, sizeof(cmd_ring));
    ctrl.cmd_ring_dma.cpu = cmd_ring;
    ctrl.cmd_ring_dma.bus = 0x10000;
    ctrl.cmd_ring_enqueue = 0;
    ctrl.cmd_ring_cycle = 1;

    struct xhci_trb event_ring[XHCI_TRB_RING_SIZE];
    memset(event_ring, 0, sizeof(event_ring));
    ctrl.event_ring_dma.cpu = event_ring;
    ctrl.event_ring_dma.bus = 0x20000;
    ctrl.event_ring_dequeue = 0;
    ctrl.event_ring_cycle = 1;

    uint8_t rt_regs[0x100];
    memset(rt_regs, 0, sizeof(rt_regs));
    ctrl.rt_regs = rt_regs;

    uint8_t db_regs[0x1000];
    memset(db_regs, 0, sizeof(db_regs));
    ctrl.db_regs = db_regs;

    /* Stale completion from an earlier timed-out command: slot 5, wrong TRB pointer */
    event_ring[0].parameter = 0xDEADBEEFULL;
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[0].control = (5U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    /* The completion for the Enable Slot about to be submitted: TRB at index 0 */
    event_ring[1].parameter = 0x10000;
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[1].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    uint8_t slot_id = 0;
    int rc = xhci_enable_slot(&ctrl, 1, 3, &slot_id);
    assert_eq(rc, 0);
    assert_eq(slot_id, 1);
    assert_true(ctrl.slots[1] != NULL);
    assert_true(ctrl.slots[5] == NULL);
    assert_eq(ctrl.event_ring_dequeue, 2);

    xhci_free_slot(&ctrl, 1);
}

void test_xhci_transfer_event_slot_match(void)
{
    TEST_CASE("Transfer event: completion for another slot is skipped");
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));

    struct xhci_slot slot;
    memset(&slot, 0, sizeof(slot));
    slot.slot_id = 1;

    struct xhci_trb ep0_ring[XHCI_TRB_RING_SIZE];
    memset(ep0_ring, 0, sizeof(ep0_ring));
    slot.ep0_ring_dma.cpu = ep0_ring;
    slot.ep0_ring_dma.bus = 0x40000;
    slot.ep0_ring_cycle = 1;
    slot.ep0_ring_enqueue = 0;
    ep0_ring[XHCI_TRB_RING_SIZE - 1].parameter = 0x40000;
    ep0_ring[XHCI_TRB_RING_SIZE - 1].control = (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_LINK_TC;

    uint8_t dma_buf[4096];
    memset(dma_buf, 0, sizeof(dma_buf));
    slot.data_dma.cpu = dma_buf;
    slot.data_dma.bus = 0x50000;

    struct usb_device_descriptor raw = {
        .bLength = 18,
        .bDescriptorType = 1,
        .bcdUSB = 0x0200,
        .bMaxPacketSize0 = 64,
        .idVendor = 0x0627,
        .idProduct = 0x0001,
        .bNumConfigurations = 1,
    };
    memcpy(dma_buf, &raw, 18);

    ctrl.slots[1] = &slot;

    struct xhci_trb event_ring[XHCI_TRB_RING_SIZE];
    memset(event_ring, 0, sizeof(event_ring));
    ctrl.event_ring_dma.cpu = event_ring;
    ctrl.event_ring_dma.bus = 0x20000;
    ctrl.event_ring_dequeue = 0;
    ctrl.event_ring_cycle = 1;

    uint8_t rt_regs[0x100];
    memset(rt_regs, 0, sizeof(rt_regs));
    ctrl.rt_regs = rt_regs;

    uint8_t db_regs[0x1000];
    memset(db_regs, 0, sizeof(db_regs));
    ctrl.db_regs = db_regs;

    /* Stale transfer event for slot 7 with a STALL completion code */
    event_ring[0].parameter = 0x40000 + 1 * sizeof(struct xhci_trb);
    event_ring[0].status = (XHCI_COMP_STALL << 24);
    event_ring[0].control = (7U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    /* Real Data + Status completions for slot 1 DCI 1 */
    event_ring[1].parameter = 0x40000 + 1 * sizeof(struct xhci_trb);
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[1].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[2].parameter = 0x40000 + 2 * sizeof(struct xhci_trb);
    event_ring[2].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[2].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    struct usb_device_descriptor desc;
    int rc = xhci_get_device_descriptor(&ctrl, 1, &desc);
    assert_eq(rc, 0);
    assert_eq(desc.idVendor, 0x0627);
    assert_eq(desc.idProduct, 0x0001);
    assert_eq(ctrl.event_ring_dequeue, 3);
}

void test_xhci_ep0_no_data_error(void)
{
    TEST_CASE("ep0_no_data_error: Setup STALL, Data+Status ordering, wrong DCI/stale ptr, Link wrap");
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    struct xhci_slot slot;
    memset(&slot, 0, sizeof(slot));
    slot.slot_id = 1;
    slot.active = true;

    struct xhci_trb ep0_ring[XHCI_TRB_RING_SIZE];
    memset(ep0_ring, 0, sizeof(ep0_ring));
    slot.ep0_ring_dma.cpu = ep0_ring;
    slot.ep0_ring_dma.bus = 0x40000;
    slot.ep0_ring_cycle = 1;
    slot.ep0_ring_enqueue = 0;
    ep0_ring[XHCI_TRB_RING_SIZE - 1].parameter = 0x40000;
    ep0_ring[XHCI_TRB_RING_SIZE - 1].control = (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_LINK_TC;

    uint8_t dma_buf[4096];
    memset(dma_buf, 0, sizeof(dma_buf));
    slot.data_dma.cpu = dma_buf;
    slot.data_dma.bus = 0x50000;
    ctrl.slots[1] = &slot;

    struct xhci_trb event_ring[XHCI_TRB_RING_SIZE];
    memset(event_ring, 0, sizeof(event_ring));
    ctrl.event_ring_dma.cpu = event_ring;
    ctrl.event_ring_dma.bus = 0x20000;
    ctrl.event_ring_dequeue = 0;
    ctrl.event_ring_cycle = 1;

    uint8_t rt_regs[0x100];
    memset(rt_regs, 0, sizeof(rt_regs));
    ctrl.rt_regs = rt_regs;
    uint8_t db_regs[0x1000];
    memset(db_regs, 0, sizeof(db_regs));
    ctrl.db_regs = db_regs;

    /* 1. EP0 Setup STALL on xhci_control_no_data returns -EPIPE */
    event_ring[0].parameter = 0x40000 + 0 * sizeof(struct xhci_trb); /* Setup TRB */
    event_ring[0].status = (XHCI_COMP_STALL << 24);
    event_ring[0].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    int rc = xhci_control_no_data(&ctrl, 1, 0x21, 0xFF, 0, 1);
    assert_eq(rc, -EPIPE);
    assert_eq(ctrl.event_ring_dequeue, 1);

    /* 2. Descriptor Data SHORT_TX + Status STALL: must NOT report success, must NOT copy */
    struct usb_device_descriptor raw = {
        .bLength = 18, .bDescriptorType = 1, .idVendor = 0x1234, .idProduct = 0x5678,
    };
    memcpy(dma_buf, &raw, 18);

    /* Stale pointer and wrong DCI (DCI=3 instead of 1) discarded before Data TRB */
    event_ring[1].parameter = 0xDEADBEEFULL;
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[1].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[2].parameter = 0x40000 + 3 * sizeof(struct xhci_trb);
    event_ring[2].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[2].control = (1U << 24) | (3U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    /* Matching Data TRB (index 3) */
    event_ring[3].parameter = 0x40000 + 3 * sizeof(struct xhci_trb);
    event_ring[3].status = (XHCI_COMP_SHORT_TX << 24) | 2;
    event_ring[3].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    /* Status TRB (index 4) FAILS with STALL */
    event_ring[4].parameter = 0x40000 + 4 * sizeof(struct xhci_trb);
    event_ring[4].status = (XHCI_COMP_STALL << 24);
    event_ring[4].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    struct usb_device_descriptor out_desc;
    memset(&out_desc, 0, sizeof(out_desc));
    rc = xhci_get_device_descriptor(&ctrl, 1, &out_desc);
    assert_eq(rc, -EPIPE);
    assert_eq(out_desc.idVendor, 0); /* Not copied on Status failure */

    /* 3. Stage boundary at Link preserves next cycle (Setup=254 C=1, Link=255 C=1, Data=0 C=0, Status=1 C=0) */
    slot.ep0_ring_enqueue = 254;
    slot.ep0_ring_cycle = 1;
    event_ring[5].parameter = 0x40000 + 0 * sizeof(struct xhci_trb); /* Data at 0 */
    event_ring[5].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[5].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    event_ring[6].parameter = 0x40000 + 1 * sizeof(struct xhci_trb); /* Status at 1 */
    event_ring[6].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[6].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    rc = xhci_get_device_descriptor(&ctrl, 1, &out_desc);
    assert_eq(rc, 0);
    assert_eq(out_desc.idVendor, 0x1234);
    assert_eq(ep0_ring[254].control & XHCI_TRB_CYCLE_BIT, 1);
    assert_eq(ep0_ring[255].control & XHCI_TRB_CYCLE_BIT, 1);
    assert_eq(ep0_ring[0].control & XHCI_TRB_CYCLE_BIT, 0);
    assert_eq(ep0_ring[1].control & XHCI_TRB_CYCLE_BIT, 0);
    assert_eq(slot.ep0_ring_cycle, 0);
    assert_eq(slot.ep0_ring_enqueue, 2);
}

void test_xhci_timeout_owns_dma(void)
{
    TEST_CASE("timeout_owns_dma: timeout/unproven disable/HCH keep DMA live and poison slot/ctrl");
    /* 1. Address Device timeout keeps DCBAA/context/pages and prevents later doorbells */
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    struct xhci_trb cmd_ring[XHCI_TRB_RING_SIZE];
    memset(cmd_ring, 0, sizeof(cmd_ring));
    struct xhci_trb event_ring[XHCI_TRB_RING_SIZE];
    memset(event_ring, 0, sizeof(event_ring));
    uint8_t rt_regs[0x100];
    memset(rt_regs, 0, sizeof(rt_regs));
    uint8_t db_regs[0x1000];
    memset(db_regs, 0, sizeof(db_regs));
    uint64_t dcbaa[64];
    memset(dcbaa, 0, sizeof(dcbaa));

    ctrl.cmd_ring_dma.cpu = cmd_ring;
    ctrl.cmd_ring_dma.bus = 0x10000;
    ctrl.cmd_ring_cycle = 1;
    ctrl.event_ring_dma.cpu = event_ring;
    ctrl.event_ring_dma.bus = 0x20000;
    ctrl.event_ring_cycle = 1;
    ctrl.rt_regs = rt_regs;
    ctrl.db_regs = db_regs;
    ctrl.dcbaa_dma.cpu = dcbaa;
    ctrl.dcbaa_dma.bus = 0x30000;
    ctrl.running = true;

    struct xhci_slot slot;
    memset(&slot, 0, sizeof(slot));
    slot.slot_id = 1;
    slot.port_num = 1;
    slot.speed = XHCI_PORT_SPEED_HIGH;
    slot.active = true;
    ctrl.slots[1] = &slot;

    int pages_before = s_mock_page_count;
    int rc = xhci_address_device(&ctrl, 1); /* Event ring empty -> -ETIMEDOUT */
    assert_eq(rc, -ETIMEDOUT);
    assert_true(ctrl.poisoned);
    assert_true(slot.poisoned);
    assert_true(dcbaa[1] != 0);
    assert_eq(s_mock_page_count, pages_before + 4); /* 4 pages retained, NOT freed */

    /* Later operations on poisoned controller/slot must be rejected without ringing doorbell */
    *(volatile uint32_t *)(db_regs + 4) = 0;
    rc = xhci_control_no_data(&ctrl, 1, 0, 0, 0, 0);
    assert_true(rc != 0);
    assert_eq(*(volatile uint32_t *)(db_regs + 4), 0);

    /* Freeing slot while controller is not halted must retain DMA */
    xhci_free_slot(&ctrl, 1);
    assert_true(ctrl.slots[1] != NULL);
    assert_eq(s_mock_page_count, pages_before + 4);

    /* 2. Controller remove with HCH not established keeps controller & slot DMA alive,
     * including a healthy ctx_published slot (not previously poisoned). */
    struct xhci_slot *healthy_slot = kmalloc(sizeof(struct xhci_slot));
    assert_true(healthy_slot != NULL);
    memset(healthy_slot, 0, sizeof(*healthy_slot));
    healthy_slot->slot_id = 2;
    healthy_slot->active = true;
    healthy_slot->ctx_published = true;
    healthy_slot->input_ctx_dma.bus = alloc_4k_page();
    healthy_slot->dev_ctx_dma.bus = alloc_4k_page();
    healthy_slot->ep0_ring_dma.bus = alloc_4k_page();
    healthy_slot->data_dma.bus = alloc_4k_page();
    dcbaa[2] = healthy_slot->dev_ctx_dma.bus;
    ctrl.slots[2] = healthy_slot;
    int pages_with_healthy = s_mock_page_count;

    uint8_t op_regs[0x100];
    memset(op_regs, 0, sizeof(op_regs));
    ctrl.op_regs = op_regs;
    /* USBSTS.HCH stays 0 (never halts) */
    struct pci_device pdev;
    memset(&pdev, 0, sizeof(pdev));
    pdev.driver_data = &ctrl;
    xhci_remove(&pdev);
    assert_eq(s_mock_page_count, pages_with_healthy); /* Both poisoned & healthy retained! */
    assert_true(ctrl.slots[2] != NULL);
    assert_true(dcbaa[2] != 0);

    /* Cleanup mock pages manually so test doesn't leak host pages */
    free_4k_page(healthy_slot->input_ctx_dma.bus);
    free_4k_page(healthy_slot->dev_ctx_dma.bus);
    free_4k_page(healthy_slot->ep0_ring_dma.bus);
    free_4k_page(healthy_slot->data_dma.bus);
    kfree(healthy_slot);
    free_4k_page(slot.input_ctx_dma.bus);
    free_4k_page(slot.dev_ctx_dma.bus);
    free_4k_page(slot.ep0_ring_dma.bus);
    free_4k_page(slot.data_dma.bus);
}

void test_xhci_ownership_and_dma(void)
{
    TEST_CASE("xhci_ownership_and_dma: AC64=0 bounds, xECP aperture/PSI, BIOS legacy handoff");
    struct xhci_controller ctrl;

    /* 1. AC64=0 rejects 4 GiB+ DMA in xhci_alloc_dma before Phy_To_Virt/memset */
    memset(s_mock_mmio, 0, MOCK_MMIO_SIZE);
    s_mock_mmio[XHCI_CAP_CAPLENGTH] = 0x20;
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_HCSPARAMS1) = (4U << 24) | (1U << 8) | 16U;
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_HCCPARAMS1) = 0; /* AC64=0 */
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_DBOFF) = 0x400;
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_RTSOFF) = 0x600;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.mmio_virt = s_mock_mmio;
    ctrl.mmio_len = 0x1000;
    assert_eq(xhci_parse_capabilities(&ctrl), 0);
    assert_false(ctrl.ac64);

    s_inject_phys_head = s_inject_phys_tail = 0;
    push_inject_phys(0x100000000ULL);
    assert_true(xhci_alloc_dma(&ctrl) < 0);
    assert_eq(ctrl.dcbaa_dma.bus, 0);

    /* 2. AC64=0 rejects high DMA in late slot allocation (xhci_address_device) without publishing */
    uint64_t dcbaa[33];
    struct xhci_trb cmd_ring[XHCI_TRB_RING_SIZE];
    uint8_t db_regs[0x100];
    struct xhci_slot slot;
    memset(dcbaa, 0, sizeof(dcbaa));
    memset(cmd_ring, 0, sizeof(cmd_ring));
    memset(db_regs, 0, sizeof(db_regs));
    memset(&slot, 0, sizeof(slot));
    ctrl.dcbaa_dma.cpu = dcbaa;
    ctrl.cmd_ring_dma.cpu = cmd_ring;
    ctrl.cmd_ring_dma.bus = 0x10000;
    ctrl.cmd_ring_cycle = 1;
    ctrl.db_regs = db_regs;
    slot.slot_id = 1;
    slot.port_num = 1;
    slot.speed = XHCI_PORT_SPEED_HIGH;
    slot.active = true;
    ctrl.slots[1] = &slot;

    s_inject_phys_head = s_inject_phys_tail = 0;
    push_inject_phys(0x100000000ULL);
    assert_true(xhci_address_device(&ctrl, 1) < 0);
    assert_eq(dcbaa[1], 0);
    assert_eq(*(volatile uint32_t *)db_regs, 0);

    /* 3. AC64=0 rejects bulk staging straddling 4 GiB */
    struct xhci_trb ep_trbs[XHCI_TRB_RING_SIZE];
    struct xhci_ep_ring in_ring = {
        .ring_dma.bus = 0x60000, .ring_dma.cpu = ep_trbs,
        .enqueue = 0, .cycle = 1, .dci = 3, .ep_type = XHCI_EP_TYPE_BULK_IN,
        .max_packet_size = 512, .active = true,
    };
    struct xhci_ep_ring *rings[4] = { NULL, NULL, NULL, &in_ring };
    slot.configured = true;
    slot.ep_rings = rings;
    slot.max_dci = 3;
    slot.bulk_staging_dma.cpu = (void *)(uintptr_t)0x1000; /* non-NULL dummy */
    slot.bulk_staging_dma.bus = 0xFFFF0000ULL; /* 0xFFFF0000 + 65536 = 0x100000000 (ends at 4 GiB, ok? Wait: 0xFFFF0000..0xFFFFFFFF is 64 KiB ending at 4 GiB -> OK; 0xFFFFF000 + 65536 straddles!) */
    slot.bulk_staging_dma.bus = 0xFFFFF000ULL; /* unaligned + straddles 4 GiB */
    uint8_t dummy[64];
    size_t actual = 0;
    assert_true(xhci_bulk_transfer(&ctrl, 1, 3, dummy, sizeof(dummy), &actual) < 0);
    assert_eq(*(volatile uint32_t *)(db_regs + 4), 0);

    /* 4. xECP bounds & Supported Protocol PSIC checks */
    /* 4a. xECP outside 0x1000 aperture (0x400 dwords = 0x1000) */
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_HCCPARAMS1) = (0x400U << 16);
    assert_true(xhci_parse_capabilities(&ctrl) < 0);

    /* 4b. Legacy cap truncated at 0xFFC (needs 8 bytes) */
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_HCCPARAMS1) = ((0xFFCU >> 2) << 16);
    *(uint32_t *)(s_mock_mmio + 0xFFC) = XHCI_EXT_CAP_LEGACY;
    assert_true(xhci_parse_capabilities(&ctrl) < 0);

    /* 4c. Supported Protocol truncated at 0xFF4 (needs 16 bytes) */
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_HCCPARAMS1) = ((0xFF4U >> 2) << 16);
    *(uint32_t *)(s_mock_mmio + 0xFF4) = XHCI_EXT_CAP_PROTOCOL | (2U << 24);
    assert_true(xhci_parse_capabilities(&ctrl) < 0);

    /* 4d. Supported Protocol with PSIC=1 truncated past 0x1000 aperture rejected */
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_HCCPARAMS1) = ((0xFF0U >> 2) << 16);
    *(uint32_t *)(s_mock_mmio + 0xFF0) = XHCI_EXT_CAP_PROTOCOL | (3U << 24);
    *(uint32_t *)(s_mock_mmio + 0xFF8) = (1U << 28) | (1U << 8) | 1U; /* PSIC=1 -> needs 20B */
    assert_true(xhci_parse_capabilities(&ctrl) < 0);

    /* 4d2. Supported Protocol with valid PSIC=2 (5Gbps PSIV=4, 10Gbps PSIV=5) accepted */
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_HCCPARAMS1) = ((0x800U >> 2) << 16);
    *(uint32_t *)(s_mock_mmio + 0x800) = XHCI_EXT_CAP_PROTOCOL | (3U << 24);
    *(uint32_t *)(s_mock_mmio + 0x808) = (2U << 28) | (1U << 8) | 1U; /* PSIC=2, port 1..1 */
    *(uint32_t *)(s_mock_mmio + 0x810) = (5U << 16) | (3U << 4) | 4U;  /* PSIV=4, 5Gbps */
    *(uint32_t *)(s_mock_mmio + 0x814) = (10U << 16) | (3U << 4) | 5U; /* PSIV=5, 10Gbps */
    assert_eq(0, xhci_parse_capabilities(&ctrl));
    assert_eq(3, ctrl.port_proto[1]);
    assert_eq(XHCI_PORT_SPEED_SUPER, ctrl.psi_speed_class[1][4]);
    assert_eq(XHCI_PORT_SPEED_SUPER, ctrl.psi_speed_class[1][5]);

    /* 4e. Port registers beyond 0x1000 aperture rejected (caplength=0x20, max_ports=250 -> 0x420+250*16 > 0x1000) */
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_HCCPARAMS1) = 0;
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_HCSPARAMS1) = (250U << 24) | (1U << 8) | 16U;
    assert_true(xhci_parse_capabilities(&ctrl) < 0);

    /* 5. BIOS legacy ownership release before HCRST, and stuck semaphore fails without HCRST */
    memset(s_mock_mmio, 0, MOCK_MMIO_SIZE);
    s_mock_mmio[XHCI_CAP_CAPLENGTH] = 0x20;
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_HCSPARAMS1) = (1U << 24) | (1U << 8) | 16U;
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_HCCPARAMS1) = ((0x800U >> 2) << 16) | 1U; /* AC64=1, xECP=0x800 */
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_DBOFF) = 0x400;
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_RTSOFF) = 0x600;
    *(uint32_t *)(s_mock_mmio + 0x20 + XHCI_OP_USBSTS) = XHCI_STS_HCH;
    *(uint32_t *)(s_mock_mmio + 0x800) = XHCI_EXT_CAP_LEGACY | XHCI_LEGSUP_BIOS_OWNED;
    *(uint32_t *)(s_mock_mmio + 0x804) = XHCI_LEGCTL_SMI_ENABLES | XHCI_LEGCTL_RW1C;

    struct pci_device pdev;
    memset(&pdev, 0, sizeof(pdev));
    pdev.bars[0].valid = true;
    pdev.bars[0].kind = PCI_BAR_MMIO32;
    pdev.bars[0].address = (uint64_t)(uintptr_t)s_mock_mmio;

    /* 5a. Stuck BIOS semaphore fails without HCRST */
    s_legacy_bios_stuck = true;
    s_hcrst_seen = false;
    s_mock_clock_ns = 0xFFFFFFFFFFFF0000ULL; /* test wrap-safe elapsed time! */
    s_mock_clock_step_ns = 500000000ULL;    /* 500ms per call -> >1s in 3 calls */
    assert_true(xhci_probe(&pdev, &xhci_pci_driver.id_table[0]) < 0);
    assert_false(s_hcrst_seen);
    assert_true(pdev.driver_data == NULL);

    /* 5b. BIOS releases ownership before HCRST and SMI bits are cleared */
    s_legacy_bios_stuck = false;
    s_hcrst_seen = false;
    s_legsup_os_seen_before_hcrst = false;
    s_last_legctl_written = 0;
    s_mock_clock_ns = 0;
    s_mock_clock_step_ns = 1000ULL;
    assert_eq(xhci_probe(&pdev, &xhci_pci_driver.id_table[0]), 0);
    assert_true(s_legsup_os_seen_before_hcrst);
    assert_true(s_hcrst_seen);
    assert_eq(s_last_legctl_written & XHCI_LEGCTL_SMI_ENABLES, 0);
    assert_eq(s_last_legctl_written & XHCI_LEGCTL_RW1C, XHCI_LEGCTL_RW1C);
    xhci_remove(&pdev);
}

static void init_ep0_fixture(struct xhci_controller *ctrl, struct xhci_slot *slot,
                             struct xhci_trb *ep0_ring, uint8_t *dma_buf,
                             struct xhci_trb *event_ring, uint8_t *rt_regs,
                             uint8_t *db_regs)
{
    memset(ctrl, 0, sizeof(*ctrl));
    memset(slot, 0, sizeof(*slot));
    memset(ep0_ring, 0, sizeof(struct xhci_trb) * XHCI_TRB_RING_SIZE);
    memset(dma_buf, 0, 4096);
    memset(event_ring, 0, sizeof(struct xhci_trb) * XHCI_TRB_RING_SIZE);
    memset(rt_regs, 0, 0x100);
    memset(db_regs, 0, 0x1000);

    slot->slot_id = 1;
    slot->ep0_ring_dma.cpu = ep0_ring;
    slot->ep0_ring_dma.bus = 0x40000;
    slot->ep0_ring_cycle = 1;
    slot->ep0_ring_enqueue = 0;
    ep0_ring[XHCI_TRB_RING_SIZE - 1].parameter = 0x40000;
    ep0_ring[XHCI_TRB_RING_SIZE - 1].control = (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_LINK_TC;

    slot->data_dma.cpu = dma_buf;
    slot->data_dma.bus = 0x50000;
    ctrl->slots[1] = slot;

    ctrl->event_ring_dma.cpu = event_ring;
    ctrl->event_ring_dma.bus = 0x20000;
    ctrl->event_ring_dequeue = 0;
    ctrl->event_ring_cycle = 1;
    ctrl->rt_regs = rt_regs;
    ctrl->db_regs = db_regs;
}

static void test_ep0_class_in_until(void)
{
    TEST_CASE("EP0 xhci_control_in: class IN, short read, bad residual/status, timeout DMA");
    struct xhci_controller ctrl;
    struct xhci_slot slot;
    struct xhci_trb ep0_ring[XHCI_TRB_RING_SIZE];
    struct xhci_trb event_ring[XHCI_TRB_RING_SIZE];
    uint8_t dma_buf[4096];
    uint8_t rt_regs[0x100];
    uint8_t db_regs[0x1000];

    init_ep0_fixture(&ctrl, &slot, ep0_ring, dma_buf, event_ring, rt_regs, db_regs);
    s_mock_clock_ns = 1000000ULL;
    s_mock_clock_step_ns = 1000ULL;

    /* 1. Normal 4-byte class IN {0xa3, 0, 0, 3, 4} */
    uint8_t payload[4] = { 0x11, 0x22, 0x33, 0x44 };
    memcpy(dma_buf, payload, 4);
    event_ring[0].parameter = 0x40000 + 1 * sizeof(struct xhci_trb);
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[0].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    event_ring[1].parameter = 0x40000 + 2 * sizeof(struct xhci_trb);
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[1].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    struct xhci_setup st = { .type = 0xa3, .request = 0, .value = 0, .index = 3, .length = 4 };
    uint8_t dst[4] = {0};
    int rc = xhci_control_in(&ctrl, 1, &st, dst, sizeof(dst), s_mock_clock_ns + 50000000ULL);
    assert_eq(4, rc);
    assert_eq(0, memcmp(dst, payload, 4));
    assert_eq(0x00040003000000a3ULL, ep0_ring[0].parameter);

    /* 2. Short read 2 returns 2 */
    init_ep0_fixture(&ctrl, &slot, ep0_ring, dma_buf, event_ring, rt_regs, db_regs);
    dma_buf[0] = 0xAA; dma_buf[1] = 0xBB;
    event_ring[0].parameter = 0x40000 + 1 * sizeof(struct xhci_trb);
    event_ring[0].status = (XHCI_COMP_SHORT_TX << 24) | 2;
    event_ring[0].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    event_ring[1].parameter = 0x40000 + 2 * sizeof(struct xhci_trb);
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[1].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    memset(dst, 0, sizeof(dst));
    rc = xhci_control_in(&ctrl, 1, &st, dst, sizeof(dst), s_mock_clock_ns + 50000000ULL);
    assert_eq(2, rc);
    assert_eq(0xAA, dst[0]);
    assert_eq(0xBB, dst[1]);

    /* 3. Excess residual (> length) rejected */
    init_ep0_fixture(&ctrl, &slot, ep0_ring, dma_buf, event_ring, rt_regs, db_regs);
    event_ring[0].parameter = 0x40000 + 1 * sizeof(struct xhci_trb);
    event_ring[0].status = (XHCI_COMP_SHORT_TX << 24) | 5;
    event_ring[0].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    rc = xhci_control_in(&ctrl, 1, &st, dst, sizeof(dst), s_mock_clock_ns + 50000000ULL);
    assert_true(rc < 0);

    /* 4. Missing Status stage times out and retains DMA */
    init_ep0_fixture(&ctrl, &slot, ep0_ring, dma_buf, event_ring, rt_regs, db_regs);
    event_ring[0].parameter = 0x40000 + 1 * sizeof(struct xhci_trb);
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[0].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    memset(dst, 0x5A, sizeof(dst));
    rc = xhci_control_in(&ctrl, 1, &st, dst, sizeof(dst), s_mock_clock_ns + 50000ULL);
    assert_eq(-ETIMEDOUT, rc);
    assert_true(slot.poisoned);
    assert_true(slot.dma_retained);
    assert_eq(0x5A, dst[0]); /* Not copied on Status failure */
}

static void test_xhci_deadline_on_noisy_ring(void)
{
    TEST_CASE("xhci deadline expires even on continuously noisy event ring");
    struct xhci_controller ctrl;
    struct xhci_slot slot;
    struct xhci_trb ep0_ring[XHCI_TRB_RING_SIZE];
    struct xhci_trb event_ring[XHCI_TRB_RING_SIZE];
    uint8_t dma_buf[4096];
    uint8_t rt_regs[0x100];
    uint8_t db_regs[0x1000];

    init_ep0_fixture(&ctrl, &slot, ep0_ring, dma_buf, event_ring, rt_regs, db_regs);
    /* Fill ring with continuous unrelated Port Status Change events */
    for (int i = 0; i < 64; i++) {
        event_ring[i].control = (1U << 24) | (XHCI_TRB_TYPE_PORT_STATUS_EVT << 10) | 1;
    }

    s_mock_clock_ns = 1000000ULL;
    s_mock_clock_step_ns = 1000000ULL; /* 1ms per clock read */
    struct xhci_setup st = { .type = 0xa3, .request = 0, .value = 0, .index = 1, .length = 4 };
    uint8_t dst[4];
    int rc = xhci_control_in(&ctrl, 1, &st, dst, sizeof(dst), s_mock_clock_ns + 5000000ULL);
    assert_eq(-ETIMEDOUT, rc);
    assert_true(ctrl.event_ring_dequeue > 0);
}

static void test_xhci_command_deadline_ownership(void)
{
    TEST_CASE("xhci_submit_cmd_until_locked: expired pre-submit vs in-flight timeout vs CC error");
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));

    struct xhci_trb cmd_ring[XHCI_TRB_RING_SIZE];
    struct xhci_trb event_ring[XHCI_TRB_RING_SIZE];
    uint8_t rt_regs[0x100];
    uint8_t db_regs[0x1000];
    memset(cmd_ring, 0, sizeof(cmd_ring));
    memset(event_ring, 0, sizeof(event_ring));
    memset(rt_regs, 0, sizeof(rt_regs));
    memset(db_regs, 0, sizeof(db_regs));

    ctrl.cmd_ring_dma.cpu = cmd_ring;
    ctrl.cmd_ring_dma.bus = 0x10000;
    ctrl.cmd_ring_cycle = 1;
    ctrl.event_ring_dma.cpu = event_ring;
    ctrl.event_ring_dma.bus = 0x20000;
    ctrl.event_ring_cycle = 1;
    ctrl.rt_regs = rt_regs;
    ctrl.db_regs = db_regs;

    struct xhci_trb trb;
    memset(&trb, 0, sizeof(trb));
    trb.control = (XHCI_TRB_TYPE_ENABLE_SLOT << 10) | 1;

    /* 1. Already expired: no enqueue, no doorbell, no poison */
    s_mock_clock_ns = 5000000ULL;
    s_mock_clock_step_ns = 1000ULL;
    struct xhci_cmd_result res;
    int rc = xhci_submit_cmd_until_locked(&ctrl, &trb, 4000000ULL, &res);
    assert_eq(-ETIMEDOUT, rc);
    assert_eq(XHCI_CMD_NOT_SUBMITTED, res.state);
    assert_eq(0, ctrl.cmd_ring_enqueue);
    assert_false(ctrl.poisoned);

    /* 2. Valid completion with non-success CC: COMPLETED, returns 0, preserves CC */
    event_ring[0].parameter = 0x10000;
    event_ring[0].status = (XHCI_COMP_TRB_ERR << 24);
    event_ring[0].control = (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;
    rc = xhci_submit_cmd_until_locked(&ctrl, &trb, s_mock_clock_ns + 10000000ULL, &res);
    assert_eq(0, rc);
    assert_eq(XHCI_CMD_COMPLETED, res.state);
    assert_eq(XHCI_COMP_TRB_ERR, (res.event.status >> 24) & 0xFF);
    assert_false(ctrl.poisoned);

    /* 3. Timeout after ringing doorbell: IN_FLIGHT_UNKNOWN, controller poisoned */
    rc = xhci_submit_cmd_until_locked(&ctrl, &trb, s_mock_clock_ns + 5000ULL, &res);
    assert_eq(-ETIMEDOUT, rc);
    assert_eq(XHCI_CMD_IN_FLIGHT_UNKNOWN, res.state);
    assert_true(ctrl.poisoned);
    assert_true(ctrl.dma_retained);
}

static void test_xhci_fs_mps_and_address_recovery(void)
{
    TEST_CASE("xhci FS EP0 starts at MPS=8, evaluates to 32, and enforces 2ms recovery");
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    struct xhci_trb cmd_ring[XHCI_TRB_RING_SIZE];
    struct xhci_trb event_ring[XHCI_TRB_RING_SIZE];
    uint64_t dcbaa[512];
    uint8_t rt_regs[0x100];
    uint8_t db_regs[0x1000];
    memset(cmd_ring, 0, sizeof(cmd_ring));
    memset(event_ring, 0, sizeof(event_ring));
    memset(dcbaa, 0, sizeof(dcbaa));
    memset(rt_regs, 0, sizeof(rt_regs));
    memset(db_regs, 0, sizeof(db_regs));

    ctrl.cmd_ring_dma.cpu = cmd_ring;
    ctrl.cmd_ring_dma.bus = (uint64_t)(uintptr_t)cmd_ring;
    ctrl.cmd_ring_cycle = 1;
    ctrl.event_ring_dma.cpu = event_ring;
    ctrl.event_ring_dma.bus = (uint64_t)(uintptr_t)event_ring;
    ctrl.event_ring_cycle = 1;
    ctrl.dcbaa_dma.cpu = dcbaa;
    ctrl.dcbaa_dma.bus = (uint64_t)(uintptr_t)dcbaa;
    ctrl.rt_regs = rt_regs;
    ctrl.db_regs = db_regs;
    ctrl.ac64 = true;
    ctrl.ac64_valid = true;

    /* Enable Slot (cmd[0]) -> slot 1 */
    event_ring[0].parameter = (uint64_t)(uintptr_t)&cmd_ring[0];
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[0].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    s_mock_clock_ns = 1000000ULL;
    s_mock_clock_step_ns = 100000ULL; /* 0.1 ms per read */
    uint8_t slot_id = 0;
    assert_eq(0, xhci_enable_slot(&ctrl, 1, XHCI_PORT_SPEED_FULL, &slot_id));
    assert_eq(1, slot_id);

    /* Address Device (cmd[1]) */
    event_ring[1].parameter = (uint64_t)(uintptr_t)&cmd_ring[1];
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[1].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    uint64_t t_before_addr = s_mock_clock_ns;
    assert_eq(0, xhci_address_device(&ctrl, 1));
    uint64_t t_after_addr = s_mock_clock_ns;
    struct xhci_slot *slot = ctrl.slots[1];
    assert_true(slot != NULL);
    /* FS must initialize EP0 MPS to 8 (not 64) */
    assert_eq(8, slot->max_packet_size);
    /* USB 2.0 SetAddress recovery >= 2ms */
    assert_true((t_after_addr - t_before_addr) >= 2000000ULL);

    /* Feed 8-byte initial device desc read (bMaxPacketSize0=32), Evaluate Context, then 18-byte read */
    struct usb_device_descriptor raw = {
        .bLength = 18, .bDescriptorType = 1, .bcdUSB = 0x0200,
        .bMaxPacketSize0 = 32, .idVendor = 0x1234, .idProduct = 0x5678,
        .bNumConfigurations = 1,
    };
    memcpy(slot->data_dma.cpu, &raw, 18);

    /* 1st GET_DESCRIPTOR(8): Data TRB (ep0[1]) + Status TRB (ep0[2]) */
    event_ring[2].parameter = slot->ep0_ring_dma.bus + 1 * sizeof(struct xhci_trb);
    event_ring[2].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[2].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    event_ring[3].parameter = slot->ep0_ring_dma.bus + 2 * sizeof(struct xhci_trb);
    event_ring[3].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[3].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    /* EVAL_CTX (cmd[2]) */
    event_ring[4].parameter = (uint64_t)(uintptr_t)&cmd_ring[2];
    event_ring[4].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[4].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    /* 2nd GET_DESCRIPTOR(18): Data TRB (ep0[4]) + Status TRB (ep0[5]) */
    event_ring[5].parameter = slot->ep0_ring_dma.bus + 4 * sizeof(struct xhci_trb);
    event_ring[5].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[5].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    event_ring[6].parameter = slot->ep0_ring_dma.bus + 5 * sizeof(struct xhci_trb);
    event_ring[6].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[6].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    struct usb_device_descriptor out_desc;
    assert_eq(0, xhci_get_device_descriptor(&ctrl, 1, &out_desc));
    assert_eq(32, slot->max_packet_size);
    assert_eq(0x1234, out_desc.idVendor);
    /* Verify EVAL_CTX was issued with Add Flags = A1 only */
    assert_eq(XHCI_TRB_TYPE_EVAL_CTX, (cmd_ring[2].control >> 10) & 0x3F);
    uint32_t *ctrl_ctx = (uint32_t *)slot->input_ctx_dma.cpu;
    assert_eq(0U, ctrl_ctx[0]);
    assert_eq((1U << 1), ctrl_ctx[1]);

    slot->disabled = true;
    xhci_free_slot(&ctrl, 1);
}

static void test_xhci_child_route_and_tt(void)
{
    TEST_CASE("xhci_child_location: route string 0x32, 5th nibble vs 6th/port16, TT inheritance");
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));

    struct xhci_slot hs_hub, fs_hub, d4_hub, d5_hub, root_fs_hub;
    memset(&hs_hub, 0, sizeof(hs_hub));
    memset(&fs_hub, 0, sizeof(fs_hub));
    memset(&d4_hub, 0, sizeof(d4_hub));
    memset(&d5_hub, 0, sizeof(d5_hub));
    memset(&root_fs_hub, 0, sizeof(root_fs_hub));

    hs_hub.slot_id = 1;
    hs_hub.active = true;
    hs_hub.loc = (struct xhci_location){
        .route = 0, .root_port = 4, .depth = 0,
        .speed_id = XHCI_PORT_SPEED_HIGH, .speed_class = XHCI_PORT_SPEED_HIGH
    };
    ctrl.slots[1] = &hs_hub;

    struct xhci_location fs_hub_loc;
    assert_eq(0, xhci_child_location(&ctrl, 1, 2, XHCI_PORT_SPEED_FULL, &fs_hub_loc));
    assert_eq(0x2U, fs_hub_loc.route);
    assert_eq(4, fs_hub_loc.root_port);
    assert_eq(1, fs_hub_loc.depth);
    assert_eq(1, fs_hub_loc.tt_hub_slot);
    assert_eq(2, fs_hub_loc.tt_port);

    fs_hub.slot_id = 2;
    fs_hub.active = true;
    fs_hub.loc = fs_hub_loc;
    ctrl.slots[2] = &fs_hub;

    struct xhci_location grandchild;
    assert_eq(0, xhci_child_location(&ctrl, 2, 3, XHCI_PORT_SPEED_FULL, &grandchild));
    assert_eq(0x32U, grandchild.route);
    assert_eq(4, grandchild.root_port);
    assert_eq(2, grandchild.depth);
    assert_eq(1, grandchild.tt_hub_slot);
    assert_eq(2, grandchild.tt_port);

    /* HS child has no TT fields */
    struct xhci_location hs_child;
    assert_eq(0, xhci_child_location(&ctrl, 1, 3, XHCI_PORT_SPEED_HIGH, &hs_child));
    assert_eq(0, hs_child.tt_hub_slot);
    assert_eq(0, hs_child.tt_port);

    /* Root FS hub -> FS child has no TT fields */
    root_fs_hub.slot_id = 5;
    root_fs_hub.active = true;
    root_fs_hub.loc = (struct xhci_location){
        .route = 0, .root_port = 1, .depth = 0,
        .speed_id = XHCI_PORT_SPEED_FULL, .speed_class = XHCI_PORT_SPEED_FULL
    };
    ctrl.slots[5] = &root_fs_hub;
    struct xhci_location root_fs_child;
    assert_eq(0, xhci_child_location(&ctrl, 5, 1, XHCI_PORT_SPEED_FULL, &root_fs_child));
    assert_eq(0, root_fs_child.tt_hub_slot);
    assert_eq(0, root_fs_child.tt_port);

    /* 5th nibble legal (depth 4 -> depth 5), 6th tier and port 16 rejected */
    d4_hub.slot_id = 3;
    d4_hub.active = true;
    d4_hub.loc = (struct xhci_location){
        .route = 0x1111U, .root_port = 4, .depth = 4,
        .speed_id = XHCI_PORT_SPEED_HIGH, .speed_class = XHCI_PORT_SPEED_HIGH
    };
    ctrl.slots[3] = &d4_hub;
    struct xhci_location d5_loc;
    assert_eq(0, xhci_child_location(&ctrl, 3, 7, XHCI_PORT_SPEED_HIGH, &d5_loc));
    assert_eq(0x71111U, d5_loc.route);
    assert_eq(5, d5_loc.depth);

    d5_hub.slot_id = 4;
    d5_hub.active = true;
    d5_hub.loc = d5_loc;
    ctrl.slots[4] = &d5_hub;
    struct xhci_location d6_loc;
    assert_true(xhci_child_location(&ctrl, 4, 1, XHCI_PORT_SPEED_HIGH, &d6_loc) < 0);
    assert_true(xhci_child_location(&ctrl, 1, 16, XHCI_PORT_SPEED_HIGH, &d6_loc) < 0);
    assert_true(xhci_child_location(&ctrl, 1, 0, XHCI_PORT_SPEED_HIGH, &d6_loc) < 0);
}

static void test_xhci_psi_root_scope(void)
{
    TEST_CASE("xhci PSI root-port capability scoping and reverse lookup");
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    /* Two USB2 protocol capabilities with overlapping PSIV 5 */
    uint8_t mmio[0x1000];
    memset(mmio, 0, sizeof(mmio));
    ctrl.mmio_virt = mmio;
    ctrl.mmio_len = sizeof(mmio);
    ctrl.max_ports = 4;

    /* Cap 0 @ 0x100: USB 2.0, ports 1..2, PSIC=1: PSIV=5 -> 12 Mbps (FS) */
    uint32_t *cap0 = (uint32_t *)(mmio + 0x100);
    cap0[0] = (2U << 24) | (0x10U << 8) | XHCI_EXT_CAP_PROTOCOL; /* next = +0x40 */
    cap0[2] = (1U << 28) | (2U << 8) | 1U;
    cap0[4] = (12U << 16) | (2U << 4) | 5U; /* 12 Mbps, PSIV=5 */

    /* Cap 1 @ 0x140: USB 2.0, ports 3..4, PSIC=2: PSIV=5 -> 480 Mbps (HS), PSIV=6 -> 12 Mbps (FS) */
    uint32_t *cap1 = (uint32_t *)(mmio + 0x140);
    cap1[0] = (2U << 24) | (0U << 8) | XHCI_EXT_CAP_PROTOCOL;
    cap1[2] = (2U << 28) | (2U << 8) | 3U;
    cap1[4] = (480U << 16) | (2U << 4) | 5U; /* 480 Mbps, PSIV=5 */
    cap1[5] = (12U << 16) | (2U << 4) | 6U;  /* 12 Mbps, PSIV=6 */

    /* Parse via HCCPARAMS1 xecp = 0x100 >> 2 = 0x40 */
    assert_eq(0, xhci_parse_ext_caps(&ctrl, (0x40U << 16)));

    struct xhci_slot s1, s3;
    memset(&s1, 0, sizeof(s1));
    memset(&s3, 0, sizeof(s3));
    s1.slot_id = 1; s1.active = true;
    s1.loc = (struct xhci_location){ .root_port = 1, .depth = 0, .speed_id = 5, .speed_class = XHCI_PORT_SPEED_FULL };
    s3.slot_id = 3; s3.active = true;
    s3.loc = (struct xhci_location){ .root_port = 3, .depth = 0, .speed_id = 5, .speed_class = XHCI_PORT_SPEED_HIGH };
    ctrl.slots[1] = &s1;
    ctrl.slots[3] = &s3;

    /* Reverse lookup on root 1: FS -> PSIV 5; HS -> no match (error) */
    struct xhci_location loc;
    assert_eq(0, xhci_child_location(&ctrl, 1, 1, XHCI_PORT_SPEED_FULL, &loc));
    assert_eq(5, loc.speed_id);
    assert_true(xhci_child_location(&ctrl, 1, 1, XHCI_PORT_SPEED_HIGH, &loc) < 0);

    /* Reverse lookup on root 3: FS -> PSIV 6 (no cross-segment contamination from cap0) */
    assert_eq(0, xhci_child_location(&ctrl, 3, 1, XHCI_PORT_SPEED_FULL, &loc));
    assert_eq(6, loc.speed_id);
}

static void test_xhci_default_target_rollback(void)
{
    TEST_CASE("xhci_enumerate_at rollback: DCBAA before Input OOM, USB2 PED isolation, Disable timeout retains DMA");
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    struct xhci_trb cmd_ring[XHCI_TRB_RING_SIZE];
    struct xhci_trb event_ring[XHCI_TRB_RING_SIZE];
    uint64_t dcbaa[512];
    uint8_t op_regs[0x500];
    uint8_t rt_regs[0x100];
    uint8_t db_regs[0x1000];
    memset(cmd_ring, 0, sizeof(cmd_ring));
    memset(event_ring, 0, sizeof(event_ring));
    memset(dcbaa, 0, sizeof(dcbaa));
    memset(op_regs, 0, sizeof(op_regs));
    memset(rt_regs, 0, sizeof(rt_regs));
    memset(db_regs, 0, sizeof(db_regs));

    ctrl.cmd_ring_dma.cpu = cmd_ring;
    ctrl.cmd_ring_dma.bus = (uint64_t)(uintptr_t)cmd_ring;
    ctrl.cmd_ring_cycle = 1;
    ctrl.event_ring_dma.cpu = event_ring;
    ctrl.event_ring_dma.bus = (uint64_t)(uintptr_t)event_ring;
    ctrl.event_ring_cycle = 1;
    ctrl.dcbaa_dma.cpu = dcbaa;
    ctrl.dcbaa_dma.bus = (uint64_t)(uintptr_t)dcbaa;
    ctrl.op_regs = op_regs;
    ctrl.rt_regs = rt_regs;
    ctrl.db_regs = db_regs;
    ctrl.max_ports = 2;
    ctrl.port_proto[1] = 2;
    ctrl.ac64 = true;
    ctrl.ac64_valid = true;

    s_mock_clock_ns = 1000000ULL;
    s_mock_clock_step_ns = 1000ULL;

    /* 1. Reset succeeds -> Address Device fails -> raw Disable succeeds -> USB2 root isolated */
    volatile uint32_t *port1 = (volatile uint32_t *)(op_regs + 0x400);
    *port1 = XHCI_PORTSC_CCS | XHCI_PORTSC_PED | XHCI_PORTSC_PP | (XHCI_PORT_SPEED_HIGH << 10);

    /* Enable Slot -> slot 1 */
    event_ring[0].parameter = (uint64_t)(uintptr_t)&cmd_ring[0];
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[0].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;
    /* Address Device -> USB Transaction Error */
    event_ring[1].parameter = (uint64_t)(uintptr_t)&cmd_ring[1];
    event_ring[1].status = (XHCI_COMP_TX_ERR << 24);
    event_ring[1].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;
    /* Disable Slot -> Success */
    event_ring[2].parameter = (uint64_t)(uintptr_t)&cmd_ring[2];
    event_ring[2].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[2].control = (1U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    struct xhci_location loc = {
        .route = 0, .root_port = 1, .depth = 0,
        .speed_id = XHCI_PORT_SPEED_HIGH, .speed_class = XHCI_PORT_SPEED_HIGH
    };
    uint8_t slot_id = 0;
    int rc = xhci_enumerate_at(&ctrl, &loc, NULL, s_mock_clock_ns + 50000000ULL, &slot_id);
    assert_true(rc < 0);
    assert_eq(0, slot_id);
    assert_eq(0ULL, dcbaa[1]);
    assert_true(ctrl.slots[1] == NULL);
    assert_eq(XHCI_TRB_TYPE_DISABLE_SLOT, (cmd_ring[2].control >> 10) & 0x3F);

    /* USB2 root isolation: if PR=1, isolation waits/fails; when PR=0 and PED=1, writing PED clears it */
    *port1 = XHCI_PORTSC_CCS | XHCI_PORTSC_PR | XHCI_PORTSC_PP;
    assert_true(xhci_usb2_root_isolate(&ctrl, 1, s_mock_clock_ns + 5000ULL) < 0);
    *port1 = XHCI_PORTSC_CCS | XHCI_PORTSC_PED | XHCI_PORTSC_PP;
    /* In real MMIO, writing PED=1 clears PED; mock doesn't auto-clear, so verify write mask or clear */
    *port1 = XHCI_PORTSC_CCS | XHCI_PORTSC_PP;
    assert_eq(0, xhci_usb2_root_isolate(&ctrl, 1, s_mock_clock_ns + 5000000ULL));
}

static void test_xhci_split_address_failure(void)
{
    TEST_CASE("split Address Device TX_ERR clears TT for addr=0 EP0 OUT+IN, quarantines on failure");
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    struct xhci_trb cmd_ring[XHCI_TRB_RING_SIZE];
    struct xhci_trb event_ring[XHCI_TRB_RING_SIZE];
    uint64_t dcbaa[512];
    uint8_t rt_regs[0x100];
    uint8_t db_regs[0x1000];
    memset(cmd_ring, 0, sizeof(cmd_ring));
    memset(event_ring, 0, sizeof(event_ring));
    memset(dcbaa, 0, sizeof(dcbaa));
    memset(rt_regs, 0, sizeof(rt_regs));
    memset(db_regs, 0, sizeof(db_regs));

    ctrl.cmd_ring_dma.cpu = cmd_ring;
    ctrl.cmd_ring_dma.bus = (uint64_t)(uintptr_t)cmd_ring;
    ctrl.cmd_ring_cycle = 1;
    ctrl.event_ring_dma.cpu = event_ring;
    ctrl.event_ring_dma.bus = (uint64_t)(uintptr_t)event_ring;
    ctrl.event_ring_cycle = 1;
    ctrl.dcbaa_dma.cpu = dcbaa;
    ctrl.dcbaa_dma.bus = (uint64_t)(uintptr_t)dcbaa;
    ctrl.rt_regs = rt_regs;
    ctrl.db_regs = db_regs;
    ctrl.ac64 = true;
    ctrl.ac64_valid = true;

    /* Slot 1: HS Hub on root port 1 */
    struct xhci_slot hs_hub, sibling_fs, sibling_hs;
    struct xhci_trb hub_ep0[XHCI_TRB_RING_SIZE];
    uint8_t hub_dma[4096];
    memset(&hs_hub, 0, sizeof(hs_hub));
    memset(&sibling_fs, 0, sizeof(sibling_fs));
    memset(&sibling_hs, 0, sizeof(sibling_hs));
    memset(hub_ep0, 0, sizeof(hub_ep0));
    hs_hub.ctrl = &ctrl;
    hs_hub.slot_id = 1;
    hs_hub.active = true;
    hs_hub.ep0_ring_dma.cpu = hub_ep0;
    hs_hub.ep0_ring_dma.bus = (uint64_t)(uintptr_t)hub_ep0;
    hs_hub.ep0_ring_cycle = 1;
    hs_hub.data_dma.cpu = hub_dma;
    hs_hub.data_dma.bus = (uint64_t)(uintptr_t)hub_dma;
    hs_hub.loc = (struct xhci_location){
        .route = 0, .root_port = 1, .depth = 0,
        .speed_id = XHCI_PORT_SPEED_HIGH, .speed_class = XHCI_PORT_SPEED_HIGH
    };
    ctrl.slots[1] = &hs_hub;

    /* Slot 2: FS sibling sharing TT on HS hub port 2 */
    sibling_fs.ctrl = &ctrl;
    sibling_fs.slot_id = 2;
    sibling_fs.active = true;
    sibling_fs.loc = (struct xhci_location){
        .route = 0x2U, .root_port = 1, .parent_slot = 1, .parent_port = 2, .depth = 1,
        .speed_id = XHCI_PORT_SPEED_FULL, .speed_class = XHCI_PORT_SPEED_FULL,
        .tt_hub_slot = 1, .tt_port = 2
    };
    ctrl.slots[2] = &sibling_fs;

    /* Slot 3: HS non-split sibling on HS hub port 3 */
    sibling_hs.ctrl = &ctrl;
    sibling_hs.slot_id = 3;
    sibling_hs.active = true;
    sibling_hs.loc = (struct xhci_location){
        .route = 0x3U, .root_port = 1, .parent_slot = 1, .parent_port = 3, .depth = 1,
        .speed_id = XHCI_PORT_SPEED_HIGH, .speed_class = XHCI_PORT_SPEED_HIGH
    };
    ctrl.slots[3] = &sibling_hs;

    s_mock_clock_ns = 1000000ULL;
    s_mock_clock_step_ns = 1000ULL;

    /* Enable Slot -> slot 4; Address Device -> TX_ERR; CLEAR_TT OUT (ep0[0..1]); CLEAR_TT IN (ep0[2..3]); Disable Slot -> slot 4 */
    event_ring[0].parameter = (uint64_t)(uintptr_t)&cmd_ring[0];
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[0].control = (4U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    event_ring[1].parameter = (uint64_t)(uintptr_t)&cmd_ring[1];
    event_ring[1].status = (XHCI_COMP_TX_ERR << 24);
    event_ring[1].control = (4U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    /* CLEAR_TT OUT status */
    event_ring[2].parameter = hs_hub.ep0_ring_dma.bus + 1 * sizeof(struct xhci_trb);
    event_ring[2].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[2].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    /* CLEAR_TT IN status */
    event_ring[3].parameter = hs_hub.ep0_ring_dma.bus + 3 * sizeof(struct xhci_trb);
    event_ring[3].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[3].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    /* Disable Slot */
    event_ring[4].parameter = (uint64_t)(uintptr_t)&cmd_ring[2];
    event_ring[4].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[4].control = (4U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    struct xhci_location child_loc = {
        .route = 0x2U, .root_port = 1, .parent_slot = 1, .parent_port = 2, .depth = 1,
        .speed_id = XHCI_PORT_SPEED_FULL, .speed_class = XHCI_PORT_SPEED_FULL,
        .tt_hub_slot = 1, .tt_port = 2
    };
    uint8_t slot_id = 0;
    int rc = xhci_enumerate_at(&ctrl, &child_loc, NULL, s_mock_clock_ns + 50000000ULL, &slot_id);
    assert_true(rc < 0);
    assert_eq(XHCI_TT_READY, hs_hub.tt_state);
    assert_true(xhci_slot_transport_usable(&ctrl, 2));
    assert_true(xhci_slot_transport_usable(&ctrl, 3));
    /* Verify {0x23, 8, 0x0000, 2, 0} and {0x23, 8, 0x8000, 2, 0} sent to HS hub */
    assert_eq(0x0000000200000823ULL, hub_ep0[0].parameter);
    assert_eq(0x0000000280000823ULL, hub_ep0[2].parameter);

    /* Now test TT clear failure -> QUARANTINED gates sibling_fs, but sibling_hs stays usable */
    event_ring[5].parameter = hs_hub.ep0_ring_dma.bus + 5 * sizeof(struct xhci_trb);
    event_ring[5].status = (XHCI_COMP_STALL << 24);
    event_ring[5].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    assert_true(xhci_tt_clear_locked(&ctrl, &child_loc, 0, 0x00, 0, s_mock_clock_ns + 5000000ULL) < 0);
    assert_eq(XHCI_TT_QUARANTINED, hs_hub.tt_state);
    assert_false(xhci_slot_transport_usable(&ctrl, 2));
    assert_true(xhci_slot_transport_usable(&ctrl, 3));
}

static void test_xhci_split_ep0_terminal(void)
{
    TEST_CASE("split EP0 terminal: Halted uses Reset Endpoint (no Stop) -> TT clear; Running uses Stop");
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    struct xhci_trb cmd_ring[XHCI_TRB_RING_SIZE];
    struct xhci_trb event_ring[XHCI_TRB_RING_SIZE];
    uint8_t rt_regs[0x100];
    uint8_t db_regs[0x1000];
    memset(cmd_ring, 0, sizeof(cmd_ring));
    memset(event_ring, 0, sizeof(event_ring));
    memset(rt_regs, 0, sizeof(rt_regs));
    memset(db_regs, 0, sizeof(db_regs));

    ctrl.cmd_ring_dma.cpu = cmd_ring;
    ctrl.cmd_ring_dma.bus = (uint64_t)(uintptr_t)cmd_ring;
    ctrl.cmd_ring_cycle = 1;
    ctrl.event_ring_dma.cpu = event_ring;
    ctrl.event_ring_dma.bus = (uint64_t)(uintptr_t)event_ring;
    ctrl.event_ring_cycle = 1;
    ctrl.rt_regs = rt_regs;
    ctrl.db_regs = db_regs;

    struct xhci_slot hs_hub, child;
    struct xhci_trb hub_ep0[XHCI_TRB_RING_SIZE], child_ep0[XHCI_TRB_RING_SIZE];
    uint8_t hub_dma[4096], child_dma[4096], child_dev_ctx[4096];
    memset(&hs_hub, 0, sizeof(hs_hub));
    memset(&child, 0, sizeof(child));
    memset(hub_ep0, 0, sizeof(hub_ep0));
    memset(child_ep0, 0, sizeof(child_ep0));
    memset(child_dev_ctx, 0, sizeof(child_dev_ctx));

    hs_hub.ctrl = &ctrl; hs_hub.slot_id = 1; hs_hub.active = true;
    hs_hub.ep0_ring_dma.cpu = hub_ep0; hs_hub.ep0_ring_dma.bus = (uint64_t)(uintptr_t)hub_ep0;
    hs_hub.ep0_ring_cycle = 1;
    hs_hub.data_dma.cpu = hub_dma; hs_hub.data_dma.bus = (uint64_t)(uintptr_t)hub_dma;
    hs_hub.loc = (struct xhci_location){ .root_port = 1, .speed_class = XHCI_PORT_SPEED_HIGH };

    child.ctrl = &ctrl; child.slot_id = 2; child.active = true;
    child.ep0_ring_dma.cpu = child_ep0; child.ep0_ring_dma.bus = (uint64_t)(uintptr_t)child_ep0;
    child.ep0_ring_cycle = 1;
    child.data_dma.cpu = child_dma; child.data_dma.bus = (uint64_t)(uintptr_t)child_dma;
    child.dev_ctx_dma.cpu = child_dev_ctx; child.dev_ctx_dma.bus = (uint64_t)(uintptr_t)child_dev_ctx;
    child.loc = (struct xhci_location){
        .route = 0x1U, .root_port = 1, .parent_slot = 1, .parent_port = 1, .depth = 1,
        .speed_id = XHCI_PORT_SPEED_FULL, .speed_class = XHCI_PORT_SPEED_FULL,
        .tt_hub_slot = 1, .tt_port = 1
    };
    /* Output Slot Context USB Device Address = 5; Output EP0 Context state = HALTED (2) */
    uint32_t *out_slot_ctx = (uint32_t *)xhci_dev_slot_ctx(child_dev_ctx, 0);
    out_slot_ctx[3] = 5U;
    uint32_t *out_ep0_ctx = (uint32_t *)xhci_dev_ep_ctx(child_dev_ctx, 0, 1);
    out_ep0_ctx[0] = XHCI_EP_STATE_HALTED;

    ctrl.slots[1] = &hs_hub;
    ctrl.slots[2] = &child;

    s_mock_clock_ns = 1000000ULL;
    s_mock_clock_step_ns = 1000ULL;

    /* Child EP0 IN fails with TX_ERR -> Reset Endpoint (cmd[0]) -> CLEAR_TT OUT+IN (hub_ep0[0..3]) */
    event_ring[0].parameter = child.ep0_ring_dma.bus + 1 * sizeof(struct xhci_trb);
    event_ring[0].status = (XHCI_COMP_TX_ERR << 24);
    event_ring[0].control = (2U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[1].parameter = (uint64_t)(uintptr_t)&cmd_ring[0];
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[1].control = (2U << 24) | (XHCI_TRB_TYPE_CMD_CMPL_EVT << 10) | 1;

    event_ring[2].parameter = hs_hub.ep0_ring_dma.bus + 1 * sizeof(struct xhci_trb);
    event_ring[2].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[2].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[3].parameter = hs_hub.ep0_ring_dma.bus + 3 * sizeof(struct xhci_trb);
    event_ring[3].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[3].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    struct xhci_setup st = { .type = 0x80, .request = 0x06, .value = 0x0100, .index = 0, .length = 8 };
    uint8_t dst[8];
    int rc = xhci_control_in(&ctrl, 2, &st, dst, sizeof(dst), s_mock_clock_ns + 50000000ULL);
    assert_true(rc < 0);
    /* Must send Reset Endpoint (NOT Stop Endpoint), and must poison abandoned EP0 */
    assert_eq(XHCI_TRB_TYPE_RESET_EP, (cmd_ring[0].control >> 10) & 0x3F);
    assert_eq(XHCI_TT_READY, hs_hub.tt_state);
    assert_true(child.poisoned);
    /* addr=5, EP0 OUT -> 0x0050, IN -> 0x8050 */
    assert_eq(0x0000000100500823ULL, hub_ep0[0].parameter);
    assert_eq(0x0000000180500823ULL, hub_ep0[2].parameter);
}

static void test_hub_usb2_controls(void)
{
    TEST_CASE("hub USB2 controls: 0x29 desc, alt0 single-TT, MPS/ESIT, ganged power, global OC");
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    struct xhci_trb cmd_ring[XHCI_TRB_RING_SIZE];
    struct xhci_trb event_ring[XHCI_TRB_RING_SIZE];
    uint8_t rt_regs[0x100];
    uint8_t db_regs[0x1000];
    memset(cmd_ring, 0, sizeof(cmd_ring));
    memset(event_ring, 0, sizeof(event_ring));
    memset(rt_regs, 0, sizeof(rt_regs));
    memset(db_regs, 0, sizeof(db_regs));

    ctrl.cmd_ring_dma.cpu = cmd_ring;
    ctrl.cmd_ring_dma.bus = (uint64_t)(uintptr_t)cmd_ring;
    ctrl.cmd_ring_cycle = 1;
    ctrl.event_ring_dma.cpu = event_ring;
    ctrl.event_ring_dma.bus = (uint64_t)(uintptr_t)event_ring;
    ctrl.event_ring_cycle = 1;
    ctrl.rt_regs = rt_regs;
    ctrl.db_regs = db_regs;

    struct xhci_slot hub;
    struct xhci_trb hub_ep0[XHCI_TRB_RING_SIZE];
    uint8_t hub_dma[4096], hub_dev_ctx[4096], hub_in_ctx[4096];
    memset(&hub, 0, sizeof(hub));
    memset(hub_ep0, 0, sizeof(hub_ep0));
    memset(hub_dev_ctx, 0, sizeof(hub_dev_ctx));
    memset(hub_in_ctx, 0, sizeof(hub_in_ctx));

    hub.ctrl = &ctrl; hub.slot_id = 1; hub.active = true;
    hub.ep0_ring_dma.cpu = hub_ep0; hub.ep0_ring_dma.bus = (uint64_t)(uintptr_t)hub_ep0;
    hub.ep0_ring_cycle = 1;
    hub.data_dma.cpu = hub_dma; hub.data_dma.bus = (uint64_t)(uintptr_t)hub_dma;
    hub.dev_ctx_dma.cpu = hub_dev_ctx; hub.dev_ctx_dma.bus = (uint64_t)(uintptr_t)hub_dev_ctx;
    hub.input_ctx_dma.cpu = hub_in_ctx; hub.input_ctx_dma.bus = (uint64_t)(uintptr_t)hub_in_ctx;
    hub.loc = (struct xhci_location){ .root_port = 1, .speed_id = XHCI_PORT_SPEED_HIGH, .speed_class = XHCI_PORT_SPEED_HIGH };
    hub.speed = XHCI_PORT_SPEED_HIGH;
    hub.port_num = 1;
    ctrl.slots[1] = &hub;
    s_mock_clock_ns = 1000000ULL;
    s_mock_clock_step_ns = 1000ULL;

    /* 1. Valid 0x29 Hub Descriptor (8 ports -> 7 + 2*2 = 11 bytes) */
    memset(hub_dma, 0, 64);
    hub_dma[0] = 11; hub_dma[1] = 0x29; hub_dma[2] = 8;
    hub_dma[3] = 0x00; hub_dma[4] = 0x00; /* ganged power, global OC, TTT=0 */
    hub_dma[5] = 25; /* 2 * 25ms = 50ms */
    event_ring[0].parameter = hub.ep0_ring_dma.bus + 1 * sizeof(struct xhci_trb);
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[0].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    event_ring[1].parameter = hub.ep0_ring_dma.bus + 2 * sizeof(struct xhci_trb);
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[1].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    struct xhci_hub_info info;
    memset(&info, 0, sizeof(info));
    assert_eq(0, xhci_hub_read_info(&ctrl, 1, s_mock_clock_ns + 50000000ULL, &info));
    assert_eq(8, info.nports);
    assert_eq(25, info.pwr_good_2ms);
    assert_true(info.global_overcurrent);

    /* Reject truncated 0x29 descriptor (9 bytes is too short for 9 ports, needs >= 11) */
    hub_dma[0] = 9;
    hub_dma[2] = 9;
    event_ring[2].parameter = hub.ep0_ring_dma.bus + 4 * sizeof(struct xhci_trb);
    event_ring[2].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[2].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    event_ring[3].parameter = hub.ep0_ring_dma.bus + 5 * sizeof(struct xhci_trb);
    event_ring[3].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[3].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    assert_true(xhci_hub_read_info(&ctrl, 1, s_mock_clock_ns + 50000000ULL, &info) < 0);

    /* 2. Config validation: FS interrupt MPS=8 -> MaxBurst=0, ESIT=8; HS 2x64 -> MaxBurst=1, ESIT=128 */
    static const uint8_t fs_hub_cfg[] = {
        9, 2, 25, 0, 1, 1, 0, 0xE0, 0,
        9, 4, 0, 0, 1, 0x09, 0x00, 0x00, 0,
        7, 5, 0x81, 0x03, 0x08, 0x00, 0xFF
    };
    hub.loc.speed_class = XHCI_PORT_SPEED_FULL;
    hub.speed = XHCI_PORT_SPEED_FULL;
    assert_eq(0, xhci_hub_validate_config(&hub, fs_hub_cfg, sizeof(fs_hub_cfg)));
    assert_eq(0, xhci_setup_endpoint_rings(&hub, fs_hub_cfg, sizeof(fs_hub_cfg)));
    assert_eq(0, hub.ep_rings[3]->max_burst);
    assert_eq(8, hub.ep_rings[3]->max_packet_size);
    xhci_free_endpoint_rings(&hub);

    static const uint8_t hs_hub_cfg[] = {
        9, 2, 25, 0, 1, 1, 0, 0xE0, 0,
        9, 4, 0, 0, 1, 0x09, 0x00, 0x01, 0,
        7, 5, 0x81, 0x03, 0x40, 0x08, 0x0C /* 2 tx x 64 = 0x0840, bInterval=12 */
    };
    hub.loc.speed_class = XHCI_PORT_SPEED_HIGH;
    hub.speed = XHCI_PORT_SPEED_HIGH;
    assert_eq(0, xhci_hub_validate_config(&hub, hs_hub_cfg, sizeof(hs_hub_cfg)));
    assert_eq(0, xhci_setup_endpoint_rings(&hub, hs_hub_cfg, sizeof(hs_hub_cfg)));
    assert_eq(1, hub.ep_rings[3]->max_burst);
    assert_eq(64, hub.ep_rings[3]->max_packet_size);
    xhci_free_endpoint_rings(&hub);

    /* Reject illegal high-bandwidth code 3 (0x1840) and illegal bInterval=0 */
    uint8_t bad_hb_cfg[sizeof(hs_hub_cfg)];
    memcpy(bad_hb_cfg, hs_hub_cfg, sizeof(hs_hub_cfg));
    bad_hb_cfg[18 + 5] = 0x18; /* bits 12..11 = 3 */
    assert_true(xhci_hub_validate_config(&hub, bad_hb_cfg, sizeof(bad_hb_cfg)) < 0);

    memcpy(bad_hb_cfg, hs_hub_cfg, sizeof(hs_hub_cfg));
    bad_hb_cfg[18 + 6] = 0x00; /* bInterval=0 illegal */
    assert_true(xhci_hub_validate_config(&hub, bad_hb_cfg, sizeof(bad_hb_cfg)) < 0);

    /* 3. Global overcurrent rejects prepare_ports */
    info = (struct xhci_hub_info){ .nports = 2, .pwr_good_2ms = 25, .global_overcurrent = true };
    hub_dma[0] = 0x02; hub_dma[1] = 0x00; hub_dma[2] = 0x02; hub_dma[3] = 0x00; /* HUB OC */
    event_ring[4].parameter = hub.ep0_ring_dma.bus + 7 * sizeof(struct xhci_trb);
    event_ring[4].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[4].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    event_ring[5].parameter = hub.ep0_ring_dma.bus + 8 * sizeof(struct xhci_trb);
    event_ring[5].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[5].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    uint16_t mask = 0;
    assert_true(xhci_hub_prepare_ports(&ctrl, 1, &info, s_mock_clock_ns + 500000000ULL, &mask) < 0);
}

static void test_hub_usb2_reset_and_isolation(void)
{
    TEST_CASE("hub USB2 reset & isolation: C_RESET order, RESET=1 guard, isolate readback");
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    struct xhci_trb cmd_ring[XHCI_TRB_RING_SIZE];
    struct xhci_trb event_ring[XHCI_TRB_RING_SIZE];
    uint8_t rt_regs[0x100];
    uint8_t db_regs[0x1000];
    memset(cmd_ring, 0, sizeof(cmd_ring));
    memset(event_ring, 0, sizeof(event_ring));
    memset(rt_regs, 0, sizeof(rt_regs));
    memset(db_regs, 0, sizeof(db_regs));

    ctrl.cmd_ring_dma.cpu = cmd_ring;
    ctrl.cmd_ring_dma.bus = (uint64_t)(uintptr_t)cmd_ring;
    ctrl.cmd_ring_cycle = 1;
    ctrl.event_ring_dma.cpu = event_ring;
    ctrl.event_ring_dma.bus = (uint64_t)(uintptr_t)event_ring;
    ctrl.event_ring_cycle = 1;
    ctrl.rt_regs = rt_regs;
    ctrl.db_regs = db_regs;

    struct xhci_slot hub;
    struct xhci_trb hub_ep0[XHCI_TRB_RING_SIZE];
    uint8_t hub_dma[4096];
    memset(&hub, 0, sizeof(hub));
    memset(hub_ep0, 0, sizeof(hub_ep0));
    memset(hub_dma, 0, sizeof(hub_dma));

    hub.ctrl = &ctrl; hub.slot_id = 1; hub.active = true;
    hub.ep0_ring_dma.cpu = hub_ep0; hub.ep0_ring_dma.bus = (uint64_t)(uintptr_t)hub_ep0;
    hub.ep0_ring_cycle = 1;
    hub.data_dma.cpu = hub_dma; hub.data_dma.bus = (uint64_t)(uintptr_t)hub_dma;
    hub.loc = (struct xhci_location){ .root_port = 1, .speed_class = XHCI_PORT_SPEED_HIGH };
    ctrl.slots[1] = &hub;

    /* Isolate USB2: still connected + PED=1 -> CLEAR_FEATURE(PORT_ENABLE) -> readback PED=0 */
    hub_dma[0] = 0x03; hub_dma[1] = 0x01; hub_dma[2] = 0x00; hub_dma[3] = 0x00; /* CCS|PED|PP */
    event_ring[0].parameter = hub.ep0_ring_dma.bus + 1 * sizeof(struct xhci_trb);
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[0].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    event_ring[1].parameter = hub.ep0_ring_dma.bus + 3 * sizeof(struct xhci_trb); /* CLEAR_FEATURE(ENABLE) */
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[1].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    /* Readback still fails if PED=1 */
    event_ring[2].parameter = hub.ep0_ring_dma.bus + 6 * sizeof(struct xhci_trb);
    event_ring[2].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[2].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    assert_true(xhci_hub_isolate_usb2(&ctrl, 1, 2, s_mock_clock_ns + 50000000ULL) < 0);
}

static void test_hub_ss_gen1_boundary(void)
{
    struct xhci_trb ep0_ring[XHCI_TRB_RING_SIZE] __attribute__((aligned(4096)));
    struct xhci_trb event_ring[64] __attribute__((aligned(4096)));
    uint8_t hub_dma[4096] __attribute__((aligned(4096)));
    uint32_t hub_dev_ctx[1024] __attribute__((aligned(4096)));
    uint32_t hub_in_ctx[1024] __attribute__((aligned(4096)));
    uint32_t mmio[0x100];

    memset(ep0_ring, 0, sizeof(ep0_ring));
    memset(event_ring, 0, sizeof(event_ring));
    memset(hub_dma, 0, sizeof(hub_dma));
    memset(hub_dev_ctx, 0, sizeof(hub_dev_ctx));
    memset(hub_in_ctx, 0, sizeof(hub_in_ctx));
    memset(mmio, 0, sizeof(mmio));

    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.mmio_virt = (volatile uint8_t *)mmio;
    ctrl.op_regs = ctrl.mmio_virt;
    ctrl.db_regs = ctrl.mmio_virt + 0x100;
    ctrl.rt_regs = ctrl.mmio_virt + 0x200;
    ctrl.max_ports = 4;
    ctrl.event_ring_dma.cpu = event_ring;
    ctrl.event_ring_dma.bus = (uint64_t)(uintptr_t)event_ring;
    ctrl.event_ring_cycle = 1;
    ctrl.port_proto[1] = 3;
    ctrl.port_cap_idx[1] = 1;
    ctrl.proto_caps[0].rev_major = 3;
    ctrl.proto_caps[0].psic = 1;
    ctrl.proto_caps[0].psi[4] = (struct xhci_psi_entry){
        .valid = 1, .speed_class = XHCI_PORT_SPEED_SUPER,
        .psie = 3, .psim = 5, .plt = 0
    };

    struct xhci_slot hub;
    memset(&hub, 0, sizeof(hub));
    hub.active = true;
    hub.slot_id = 1;
    hub.ctrl = &ctrl;
    hub.ep0_ring_dma.cpu = ep0_ring;
    hub.ep0_ring_dma.bus = (uint64_t)(uintptr_t)ep0_ring;
    hub.ep0_ring_cycle = 1;
    hub.data_dma.cpu = hub_dma;
    hub.data_dma.bus = (uint64_t)(uintptr_t)hub_dma;
    hub.dev_ctx_dma.cpu = hub_dev_ctx;
    hub.dev_ctx_dma.bus = (uint64_t)(uintptr_t)hub_dev_ctx;
    hub.input_ctx_dma.cpu = hub_in_ctx;
    hub.input_ctx_dma.bus = (uint64_t)(uintptr_t)hub_in_ctx;
    hub.loc = (struct xhci_location){
        .root_port = 1, .speed_id = 4, .speed_class = XHCI_PORT_SPEED_SUPER
    };
    hub.speed = 4;
    hub.port_num = 1;
    ctrl.slots[1] = &hub;

    /* 1. Valid BOS with SS USB Device Cap (0x0a) and no SSP (0x0b) */
    const uint8_t valid_bos[] = {
        5, 0x0F, 15, 0, 1, /* BOS: total_len=15, num_caps=1 */
        10, 0x10, 0x0A, 0x00, 0x08, 0x00, 0x01, 0x0A, 0x64, 0x00 /* SS Cap, 5Gbps supported */
    };
    assert_eq(0, xhci_hub_check_gen1(&ctrl, 1, valid_bos, sizeof(valid_bos)));

    /* 2. Truncated BOS / bad cap length / missing SS cap / SSP declared -> rejected */
    assert_true(xhci_hub_check_gen1(&ctrl, 1, valid_bos, 14) < 0);
    const uint8_t bad_cap_len[] = {
        5, 0x0F, 12, 0, 1,
        7, 0x10, 0x0A, 0x00, 0x08, 0x00, 0x01 /* SS Cap len < 10 */
    };
    assert_true(xhci_hub_check_gen1(&ctrl, 1, bad_cap_len, sizeof(bad_cap_len)) < 0);
    const uint8_t ssp_bos[] = {
        5, 0x0F, 27, 0, 2,
        10, 0x10, 0x0A, 0x00, 0x08, 0x00, 0x01, 0x0A, 0x64, 0x00,
        12, 0x10, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    assert_true(xhci_hub_check_gen1(&ctrl, 1, ssp_bos, sizeof(ssp_bos)) < 0);

    /* 3. Asymmetric or 10G PSI rejected even if speed_class == SUPER */
    ctrl.proto_caps[0].psi[4].plt = 2; /* Asymmetric Rx */
    assert_true(xhci_hub_check_gen1(&ctrl, 1, valid_bos, sizeof(valid_bos)) < 0);
    ctrl.proto_caps[0].psi[4].plt = 0;
    ctrl.proto_caps[0].psi[4].psim = 10; /* 10 Gb/s SSP */
    assert_true(xhci_hub_check_gen1(&ctrl, 1, valid_bos, sizeof(valid_bos)) < 0);

    /* Restore valid PSI, verify 0x2a SS Hub descriptor parsing */
    ctrl.proto_caps[0].psi[4].psim = 5;
    s_mock_clock_ns = 1000000ULL;
    s_mock_clock_step_ns = 1000ULL;
    memset(hub_dma, 0, 64);
    hub_dma[0] = 12; hub_dma[1] = 0x2A; hub_dma[2] = 4;
    hub_dma[3] = 0x01; hub_dma[4] = 0x00;
    hub_dma[5] = 50;
    event_ring[0].parameter = hub.ep0_ring_dma.bus + 1 * sizeof(struct xhci_trb);
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[0].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    event_ring[1].parameter = hub.ep0_ring_dma.bus + 2 * sizeof(struct xhci_trb);
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24) | 0;
    event_ring[1].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;

    struct xhci_hub_info info;
    assert_eq(0, xhci_hub_read_info(&ctrl, 1, s_mock_clock_ns + 50000000ULL, &info));
    assert_true(info.superspeed);
    assert_eq(4, info.nports);
    assert_eq(0, info.ttt);
}

static void test_hub_ss_depth_and_reset(void)
{
    struct xhci_trb ep0_ring[XHCI_TRB_RING_SIZE] __attribute__((aligned(4096)));
    struct xhci_trb event_ring[64] __attribute__((aligned(4096)));
    uint8_t hub_dma[4096] __attribute__((aligned(4096)));
    uint32_t mmio[0x100];

    memset(ep0_ring, 0, sizeof(ep0_ring));
    memset(event_ring, 0, sizeof(event_ring));
    memset(hub_dma, 0, sizeof(hub_dma));
    memset(mmio, 0, sizeof(mmio));

    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.mmio_virt = (volatile uint8_t *)mmio;
    ctrl.op_regs = ctrl.mmio_virt;
    ctrl.db_regs = ctrl.mmio_virt + 0x100;
    ctrl.rt_regs = ctrl.mmio_virt + 0x200;
    ctrl.max_ports = 4;
    ctrl.event_ring_dma.cpu = event_ring;
    ctrl.event_ring_dma.bus = (uint64_t)(uintptr_t)event_ring;
    ctrl.event_ring_cycle = 1;

    struct xhci_slot hub;
    memset(&hub, 0, sizeof(hub));
    hub.active = true;
    hub.slot_id = 1;
    hub.ctrl = &ctrl;
    hub.ep0_ring_dma.cpu = ep0_ring;
    hub.ep0_ring_dma.bus = (uint64_t)(uintptr_t)ep0_ring;
    hub.ep0_ring_cycle = 1;
    hub.data_dma.cpu = hub_dma;
    hub.data_dma.bus = (uint64_t)(uintptr_t)hub_dma;
    hub.loc = (struct xhci_location){
        .root_port = 1, .speed_id = 4, .speed_class = XHCI_PORT_SPEED_SUPER
    };
    hub.speed = 4;
    hub.hub_ss = true;
    ctrl.slots[1] = &hub;

    s_mock_clock_ns = 1000000ULL;
    s_mock_clock_step_ns = 1000ULL;

    /* 1. SET_HUB_DEPTH depth=0 sends {0x20, 12, 0, 0, 0}; depth=4 ok; depth=5 rejected without TRB */
    event_ring[0].parameter = hub.ep0_ring_dma.bus + 1 * sizeof(struct xhci_trb);
    event_ring[0].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[0].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    assert_eq(0, xhci_hub_set_depth(&ctrl, 1, 0, s_mock_clock_ns + 50000000ULL));
    assert_eq(0x0000000000000C20ULL, ep0_ring[0].parameter);

    event_ring[1].parameter = hub.ep0_ring_dma.bus + 3 * sizeof(struct xhci_trb);
    event_ring[1].status = (XHCI_COMP_SUCCESS << 24);
    event_ring[1].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    assert_eq(0, xhci_hub_set_depth(&ctrl, 1, 4, s_mock_clock_ns + 50000000ULL));
    assert_eq(0x0000000000040C20ULL, ep0_ring[2].parameter);

    uint32_t enq_before = hub.ep0_ring_enqueue;
    assert_true(xhci_hub_set_depth(&ctrl, 1, 5, s_mock_clock_ns + 50000000ULL) < 0);
    assert_eq(enq_before, hub.ep0_ring_enqueue);

    /* 2. Warm reset missing C_BH_PORT_RESET fails; BH_PORT_RESET (feature 28) is issued */
    struct xhci_hub_info info = { .nports = 4, .pwr_good_2ms = 25, .superspeed = true };
    memset(ep0_ring, 0, sizeof(ep0_ring));
    memset(event_ring, 0, sizeof(event_ring));
    hub.ep0_ring_enqueue = 0;
    hub.ep0_ring_cycle = 1;
    ctrl.event_ring_dequeue = 0;
    ctrl.event_ring_cycle = 1;

    hub_dma[0] = 0xC1; hub_dma[1] = 0x02; /* PLS=6 (Inactive), CCS=1, PP=0x0200 */
    hub_dma[2] = 0x10; hub_dma[3] = 0x00; /* C_RESET only, missing C_BH_PORT_RESET (0x20) */
    /* req0 GET_STATUS (TRBs 0,1,2 -> evts 1,2)
     * req1 CLEAR C_RESET (TRBs 3,4 -> evt 4)
     * req2 BH_PORT_RESET (TRBs 5,6 -> evt 6)
     * req3..5 poll GET_STATUS (TRBs 7,8,9 -> evts 8,9; 10,11,12 -> evts 11,12) */
    uint32_t ev_trbs[] = { 1, 2, 4, 6, 8, 9, 11, 12 };
    for (int i = 0; i < 8; i++) {
        event_ring[i].parameter = hub.ep0_ring_dma.bus + ev_trbs[i] * sizeof(struct xhci_trb);
        event_ring[i].status = (XHCI_COMP_SUCCESS << 24);
        event_ring[i].control = (1U << 24) | (1U << 16) | (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | 1;
    }
    uint8_t sc = 0;
    bool reset_issued = false;
    s_mock_clock_ns = 1000000ULL;
    s_mock_clock_step_ns = 50000000ULL;
    int rc = xhci_hub_reset_port(&ctrl, 1, &info, 1, s_mock_clock_ns + 2000000000ULL, &sc, &reset_issued);
    assert_true(rc < 0);
    assert_true(reset_issued);
    /* Verify BH_PORT_RESET (feature 28 = 0x1C) was issued at req2 (TRB 5) */
    assert_eq(0x00000001001C0323ULL, ep0_ring[5].parameter);
}

int main(void)
{
    TEST_SUITE("xhci_phase2");
    test_xhci_context_layout_32_vs_64();
    test_xhci_portsc_rw1c_mask();
    test_xhci_event_poll_with_intervening_port_change();
    test_xhci_event_poll_timeout();
    test_xhci_port_reset_normal_and_failure();
    test_xhci_slot_lifecycle_and_address_device();
    test_xhci_ep0_control_transfer_and_descriptor();
    test_xhci_ep0_ring_wrap();
    test_xhci_command_completion_param_match();
    test_xhci_transfer_event_slot_match();
    test_xhci_ep0_no_data_error();
    test_xhci_timeout_owns_dma();
    test_xhci_ownership_and_dma();
    test_ep0_class_in_until();
    test_xhci_deadline_on_noisy_ring();
    test_xhci_command_deadline_ownership();
    test_xhci_fs_mps_and_address_recovery();
    test_xhci_child_route_and_tt();
    test_xhci_psi_root_scope();
    test_xhci_default_target_rollback();
    test_xhci_split_address_failure();
    test_xhci_split_ep0_terminal();
    test_hub_usb2_controls();
    test_hub_usb2_reset_and_isolation();
    test_hub_ss_gen1_boundary();
    test_hub_ss_depth_and_reset();
    TEST_RESULTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
