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

int main(void)
{
    TEST_SUITE("xhci_phase2");
    test_xhci_context_layout_32_vs_64();
    test_xhci_portsc_rw1c_mask();
    TEST_RESULTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
