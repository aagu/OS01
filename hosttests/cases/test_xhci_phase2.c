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

int main(void)
{
    TEST_SUITE("xhci_phase2");
    test_xhci_context_layout_32_vs_64();
    test_xhci_portsc_rw1c_mask();
    test_xhci_event_poll_with_intervening_port_change();
    test_xhci_event_poll_timeout();
    test_xhci_port_reset_normal_and_failure();
    TEST_RESULTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
