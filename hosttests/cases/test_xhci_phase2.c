/* hosttests/cases/test_xhci_phase2.c — xHCI Host Controller Phase 2 unit tests */
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

#define MOCK_MMIO_SIZE (64 * 1024)
static uint8_t s_mock_mmio[MOCK_MMIO_SIZE];

static bool s_inject_port_ped_fail = false;

#define XHCI_PORTSC_RO_MASK (XHCI_PORTSC_CCS | XHCI_PORTSC_SPEED_MASK)

void mock_xhci_write32(volatile void *addr, uint32_t val)
{
    uintptr_t offset = (uintptr_t)addr - (uintptr_t)s_mock_mmio;

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

    ctrl.event_ring_virt = event_ring;
    ctrl.event_ring_phys = 0x20000;
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

    ctrl.event_ring_virt = event_ring;
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

    uint64_t dcbaa[64];
    memset(dcbaa, 0, sizeof(dcbaa));
    ctrl.dcbaa_virt = dcbaa;
    ctrl.dcbaa_phys = 0x30000;
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
    assert_eq(ctrl.dcbaa_virt[1], ctrl.slots[1]->dev_ctx_phys);

    /* Verify Input Context DW fields */
    uint32_t *ctrl_ctx = (uint32_t *)xhci_input_ctrl_ctx(ctrl.slots[1]->input_ctx_virt, ctrl.csz);
    assert_eq(ctrl_ctx[0], 0);                /* Drop Flags */
    assert_eq(ctrl_ctx[1], (1U<<0) | (1U<<1));/* Add Flags: Slot + EP0 */

    uint32_t *slot_ctx = (uint32_t *)xhci_input_slot_ctx(ctrl.slots[1]->input_ctx_virt, ctrl.csz);
    assert_eq(slot_ctx[0], (3U << 20) | (1U << 27)); /* Speed=3, Context Entries=1 */
    assert_eq(slot_ctx[1], (1U << 16));               /* Root Hub Port = 1 */

    uint32_t *ep0_ctx = (uint32_t *)xhci_input_ep0_ctx(ctrl.slots[1]->input_ctx_virt, ctrl.csz);
    assert_eq(ep0_ctx[1], (3U << 1) | (4U << 3) | (64U << 16)); /* CErr=3, EP Type=4, MPS=64 */
    assert_eq(ep0_ctx[2] & 1U, 1U);                              /* DCS = 1 */
    assert_eq(ep0_ctx[4], 8U);                                   /* Average TRB Length = 8 */

    /* Verify EP0 Ring Link TRB at index 255 */
    struct xhci_trb *link = &ctrl.slots[1]->ep0_ring_virt[XHCI_TRB_RING_SIZE - 1];
    assert_eq((link->control >> 10) & 0x3F, XHCI_TRB_TYPE_LINK);
    assert_true((link->control & XHCI_TRB_LINK_TC) != 0);

    /* Free slot and verify memory teardown */
    int initial_pages = s_mock_page_count;
    xhci_free_slot(&ctrl, slot_id);
    assert_true(ctrl.slots[1] == NULL);
    assert_eq(ctrl.dcbaa_virt[1], 0);
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
    slot.ep0_ring_virt = ep0_ring;
    slot.ep0_ring_phys = 0x40000;
    slot.ep0_ring_cycle = 1;
    slot.ep0_ring_enqueue = 0;

    /* Initialize Link TRB at 255 */
    ep0_ring[XHCI_TRB_RING_SIZE - 1].parameter = 0x40000;
    ep0_ring[XHCI_TRB_RING_SIZE - 1].control = (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_LINK_TC;

    uint8_t dma_buf[4096];
    memset(dma_buf, 0, sizeof(dma_buf));
    slot.dma_buf_virt = dma_buf;
    slot.dma_buf_phys = 0x50000;

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
    slot.ep0_ring_virt = ep0_ring;
    slot.ep0_ring_phys = 0x40000;
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
    slot.ep0_ring_virt = ep0_ring;
    slot.ep0_ring_phys = 0x40000;
    slot.ep0_ring_cycle = 1;
    slot.ep0_ring_enqueue = 0;
    ep0_ring[XHCI_TRB_RING_SIZE - 1].parameter = 0x40000;
    ep0_ring[XHCI_TRB_RING_SIZE - 1].control = (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_LINK_TC;

    uint8_t dma_buf[4096];
    memset(dma_buf, 0, sizeof(dma_buf));
    slot.dma_buf_virt = dma_buf;
    slot.dma_buf_phys = 0x50000;

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
    slot.ep0_ring_virt = ep0_ring;
    slot.ep0_ring_phys = 0x40000;
    slot.ep0_ring_cycle = 1;
    slot.ep0_ring_enqueue = 0;
    ep0_ring[XHCI_TRB_RING_SIZE - 1].parameter = 0x40000;
    ep0_ring[XHCI_TRB_RING_SIZE - 1].control = (XHCI_TRB_TYPE_LINK << 10) | XHCI_TRB_LINK_TC;

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

    ctrl.cmd_ring_virt = cmd_ring;
    ctrl.cmd_ring_phys = 0x10000;
    ctrl.cmd_ring_cycle = 1;
    ctrl.event_ring_virt = event_ring;
    ctrl.event_ring_phys = 0x20000;
    ctrl.event_ring_cycle = 1;
    ctrl.rt_regs = rt_regs;
    ctrl.db_regs = db_regs;
    ctrl.dcbaa_virt = dcbaa;
    ctrl.dcbaa_phys = 0x30000;
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
    healthy_slot->input_ctx_phys = alloc_4k_page();
    healthy_slot->dev_ctx_phys = alloc_4k_page();
    healthy_slot->ep0_ring_phys = alloc_4k_page();
    healthy_slot->dma_buf_phys = alloc_4k_page();
    dcbaa[2] = healthy_slot->dev_ctx_phys;
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
    free_4k_page(healthy_slot->input_ctx_phys);
    free_4k_page(healthy_slot->dev_ctx_phys);
    free_4k_page(healthy_slot->ep0_ring_phys);
    free_4k_page(healthy_slot->dma_buf_phys);
    kfree(healthy_slot);
    free_4k_page(slot.input_ctx_phys);
    free_4k_page(slot.dev_ctx_phys);
    free_4k_page(slot.ep0_ring_phys);
    free_4k_page(slot.dma_buf_phys);
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
    TEST_RESULTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
