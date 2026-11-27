/* hosttests/cases/test_xhci_init.c — xHCI Host Controller Phase 1 tests */
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

/* Mock MMIO buffer (64KB) */
#define MOCK_MMIO_SIZE (64 * 1024)
static uint8_t s_mock_mmio[MOCK_MMIO_SIZE];

/* Hardware simulation and failure injection flags */
static bool s_inject_hcrst_hang = false;
static bool s_inject_hse = false;
static bool s_inject_hch_hang = false;

void mock_xhci_write32(volatile void *addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
    uintptr_t offset = (uintptr_t)addr - (uintptr_t)s_mock_mmio;

    /* Operational registers base = 0x20 */
    if (offset == 0x20 + XHCI_OP_USBCMD) {
        volatile uint32_t *sts = (volatile uint32_t *)(s_mock_mmio + 0x20 + XHCI_OP_USBSTS);
        if (s_inject_hse) {
            *sts |= XHCI_STS_HSE;
        }
        if (val & XHCI_CMD_HCRST) {
            if (!s_inject_hcrst_hang) {
                /* Hardware clears HCRST automatically once reset completes */
                *(volatile uint32_t *)addr &= ~XHCI_CMD_HCRST;
                /* Hardware clears CNR */
                *sts &= ~XHCI_STS_CNR;
                /* Reset puts controller into halted state */
                *sts |= XHCI_STS_HCH;
            }
        }
        if (!(val & XHCI_CMD_RS)) {
            if (!s_inject_hch_hang) {
                /* Stopping controller transitions HCH to 1 */
                *sts |= XHCI_STS_HCH;
            }
        } else {
            /* Starting controller clears HCH to 0 */
            *sts &= ~XHCI_STS_HCH;
        }
    }
}

/* Mock 4KB page allocator */
#define MAX_MOCK_PAGES 16
static void *s_mock_page_ptrs[MAX_MOCK_PAGES];
static int s_mock_page_count = 0;
static int s_mock_page_alloc_limit = -1; /* -1 means no limit */

uint64_t alloc_4k_page(void)
{
    if (s_mock_page_alloc_limit >= 0 && s_mock_page_count >= s_mock_page_alloc_limit) {
        return 0;
    }
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
            s_mock_page_ptrs[i] = s_mock_page_ptrs[s_mock_page_count - 1];
            s_mock_page_count--;
            return;
        }
    }
}

void *kmalloc(size_t size)
{
    return malloc(size ? size : 1);
}

size_t kfree(void *ptr)
{
    free(ptr);
    return 1;
}

static void reset_mock_pages(void)
{
    for (int i = 0; i < s_mock_page_count; i++) {
        free(s_mock_page_ptrs[i]);
    }
    s_mock_page_count = 0;
    s_mock_page_alloc_limit = -1;
}

/* Mock PCI backend */
static uint32_t s_mock_cmd_reg = 0;
static int mock_read32(uint16_t d, uint8_t b, uint8_t s, uint8_t f, uint16_t off, uint32_t *val, enum pci_error_scope *scope)
{
    (void)d; (void)b; (void)s; (void)f; (void)scope;
    if (off == 0x04) *val = s_mock_cmd_reg;
    else *val = 0;
    return 0;
}

static int mock_write32(uint16_t d, uint8_t b, uint8_t s, uint8_t f, uint16_t off, uint32_t val, enum pci_error_scope *scope)
{
    (void)d; (void)b; (void)s; (void)f; (void)scope;
    if (off == 0x04) s_mock_cmd_reg = val;
    return 0;
}

static int mock_route_gsi(const struct pci_device *pdev, uint32_t *out)
{
    (void)pdev;
    if (out) *out = 16;
    return 0;
}

static const struct pci_backend s_mock_backend = {
    .read32 = mock_read32,
    .write32 = mock_write32,
    .route_gsi = mock_route_gsi,
};

const struct pci_backend *arch_pci_backend(void)
{
    return &s_mock_backend;
}

static void init_mock_mmio_defaults(void)
{
    memset(s_mock_mmio, 0, MOCK_MMIO_SIZE);
    s_inject_hcrst_hang = false;
    s_inject_hse = false;
    s_inject_hch_hang = false;

    /* Capability registers */
    s_mock_mmio[XHCI_CAP_CAPLENGTH] = 0x20; /* 32 bytes */
    *(uint16_t *)(s_mock_mmio + XHCI_CAP_HCIVERSION) = 0x0100; /* xHCI 1.0 */
    /* HCSPARAMS1: MaxSlots=64 (bits 7:0), MaxIntrs=1 (bits 18:8), MaxPorts=4 (bits 31:24) */
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_HCSPARAMS1) = 64 | (1 << 8) | (4 << 24);
    /* HCSPARAMS2: Max Scratchpads = 0 */
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_HCSPARAMS2) = 0;
    /* HCCPARAMS1: AC64 = 1 (host calloc pointers are > 4 GiB) */
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_HCCPARAMS1) = 1U;
    /* DBOFF = 0x400, RTSOFF = 0x600 */
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_DBOFF) = 0x400;
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_RTSOFF) = 0x600;

    /* Operational registers: base = 0x20 */
    *(uint32_t *)(s_mock_mmio + 0x20 + XHCI_OP_PAGESIZE) = 1U; /* 4KB */
    /* USBSTS: initial state: HCH=1 (halted), CNR=0 (ready) */
    *(uint32_t *)(s_mock_mmio + 0x20 + XHCI_OP_USBSTS) = XHCI_STS_HCH;
}

static void test_xhci_cap_parsing(void)
{
    TEST_CASE("xhci capability registers parsing");
    init_mock_mmio_defaults();

    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.mmio_virt = s_mock_mmio;
    ctrl.mmio_len = sizeof(s_mock_mmio);

    int rc = xhci_parse_capabilities(&ctrl);
    assert_eq(0, rc);
    assert_eq(0x20, ctrl.cap_length);
    assert_eq(0x0100, ctrl.hci_version);
    assert_eq(64, ctrl.max_slots);
    assert_eq(4, ctrl.max_ports);
    assert_eq(1, ctrl.max_intrs);
    assert_eq(0, ctrl.max_scratchpads);
    assert_eq(0x400, ctrl.dboff);
    assert_eq(0x600, ctrl.rtsoff);
    assert_true(ctrl.op_regs == s_mock_mmio + 0x20);
    assert_true(ctrl.rt_regs == s_mock_mmio + 0x600);
    assert_true(ctrl.db_regs == s_mock_mmio + 0x400);
}

static void test_xhci_scratchpad_buffers(void)
{
    TEST_CASE("xhci scratchpad buffers allocation and >512 rejection");
    init_mock_mmio_defaults();
    reset_mock_pages();

    /* Set scratchpad count = 4 in HCSPARAMS2 (bits 31:27 = 4) */
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_HCSPARAMS2) = (4U << 27);
    *(uint32_t *)(s_mock_mmio + 0x20 + XHCI_OP_PAGESIZE) = 1U; /* 4KB */

    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.mmio_virt = s_mock_mmio;
    ctrl.mmio_len = sizeof(s_mock_mmio);
    ctrl.ac64 = true;
    ctrl.ac64_valid = true;

    int rc = xhci_parse_capabilities(&ctrl);
    assert_eq(0, rc);
    assert_eq(4, ctrl.max_scratchpads);

    rc = xhci_alloc_dma(&ctrl);
    assert_eq(0, rc);
    assert_true(ctrl.scratchpad_array_dma.bus != 0);
    assert_true(ctrl.scratchpad_array_dma.cpu != NULL);
    assert_true(ctrl.scratchpad_bufs != NULL);
    assert_eq(4, ctrl.scratchpad_count);
    assert_eq(ctrl.scratchpad_array_dma.bus, ((uint64_t *)ctrl.dcbaa_dma.cpu)[0]);
    for (uint32_t i = 0; i < 4; i++) {
        assert_true(ctrl.scratchpad_bufs[i].bus != 0);
        assert_eq(ctrl.scratchpad_bufs[i].bus, ((uint64_t *)ctrl.scratchpad_array_dma.cpu)[i]);
    }

    /* Retain scratchpads when dma_retained && !halted_confirmed */
    ctrl.halted_confirmed = false;
    ctrl.dma_retained = true;
    xhci_free_dma(&ctrl);
    assert_true(ctrl.scratchpad_array_dma.bus != 0);
    assert_eq(4, ctrl.scratchpad_count);
    assert_eq(9, s_mock_page_count); /* dcbaa + array + 4 buffers + cmd + event + erst */

    ctrl.halted_confirmed = true;
    xhci_free_dma(&ctrl);
    assert_eq(0, ctrl.scratchpad_array_dma.bus);
    assert_eq(0, ctrl.scratchpad_count);
    assert_true(ctrl.scratchpad_bufs == NULL);
    assert_eq(0, s_mock_page_count);

    /* Partial-allocation rollback (dcbaa=1, array=2, buf0=3, buf1=4, fail at 5) */
    s_mock_page_alloc_limit = 4;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.mmio_virt = s_mock_mmio;
    ctrl.mmio_len = sizeof(s_mock_mmio);
    ctrl.ac64 = true;
    ctrl.ac64_valid = true;
    assert_eq(0, xhci_parse_capabilities(&ctrl));
    assert_eq(-ENOMEM, xhci_alloc_dma(&ctrl));
    assert_eq(0, ctrl.scratchpad_array_dma.bus);
    assert_eq(0, ctrl.scratchpad_count);
    assert_true(ctrl.scratchpad_bufs == NULL);
    ctrl.halted_confirmed = true;
    xhci_free_dma(&ctrl);
    assert_eq(0, s_mock_page_count);
    s_mock_page_alloc_limit = -1;

    /* PAGESIZE != 4KB (and 0) returns -ENOTSUP */
    *(uint32_t *)(s_mock_mmio + 0x20 + XHCI_OP_PAGESIZE) = 2U;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.mmio_virt = s_mock_mmio;
    ctrl.mmio_len = sizeof(s_mock_mmio);
    ctrl.ac64 = true;
    ctrl.ac64_valid = true;
    assert_eq(0, xhci_parse_capabilities(&ctrl));
    assert_eq(-ENOTSUP, xhci_alloc_dma(&ctrl));
    ctrl.halted_confirmed = true;
    xhci_free_dma(&ctrl);
    assert_eq(0, s_mock_page_count);

    *(uint32_t *)(s_mock_mmio + 0x20 + XHCI_OP_PAGESIZE) = 0U;
    assert_eq(-ENOTSUP, xhci_alloc_dma(&ctrl));
    ctrl.halted_confirmed = true;
    xhci_free_dma(&ctrl);
    assert_eq(0, s_mock_page_count);

    /* ==512 scratchpad buffers boundary (16 << 5 = 512) succeeds cap parse */
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_HCSPARAMS2) = (16U << 21);
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.mmio_virt = s_mock_mmio;
    ctrl.mmio_len = sizeof(s_mock_mmio);
    assert_eq(0, xhci_parse_capabilities(&ctrl));
    assert_eq(512, ctrl.max_scratchpads);

    /* >512 scratchpad buffers (exceeds 1-page array limit) returns -ENOTSUP */
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_HCSPARAMS2) = (17U << 21); /* 17 << 5 = 544 */
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.mmio_virt = s_mock_mmio;
    ctrl.mmio_len = sizeof(s_mock_mmio);
    assert_eq(-ENOTSUP, xhci_parse_capabilities(&ctrl));
}

static void test_xhci_reset_success(void)
{
    TEST_CASE("xhci five-step reset handshake success");
    init_mock_mmio_defaults();

    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.mmio_virt = s_mock_mmio;
    ctrl.mmio_len = sizeof(s_mock_mmio);
    assert_eq(0, xhci_parse_capabilities(&ctrl));

    int rc = xhci_reset(&ctrl);
    assert_eq(0, rc);

    /* Verify USBCMD.HCRST is 0 and USBSTS.CNR is 0 */
    uint32_t cmd = *(uint32_t *)(ctrl.op_regs + XHCI_OP_USBCMD);
    uint32_t sts = *(uint32_t *)(ctrl.op_regs + XHCI_OP_USBSTS);
    assert_true((cmd & XHCI_CMD_HCRST) == 0);
    assert_true((sts & XHCI_STS_CNR) == 0);
    assert_true((sts & XHCI_STS_HCH) != 0);
}

static void test_xhci_reset_running_controller(void)
{
    TEST_CASE("xhci reset running controller stops first");
    init_mock_mmio_defaults();

    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.mmio_virt = s_mock_mmio;
    ctrl.mmio_len = sizeof(s_mock_mmio);
    assert_eq(0, xhci_parse_capabilities(&ctrl));

    /* Simulate controller initially running: RS=1, HCH=0 */
    *(uint32_t *)(ctrl.op_regs + XHCI_OP_USBCMD) = XHCI_CMD_RS;
    *(uint32_t *)(ctrl.op_regs + XHCI_OP_USBSTS) = 0; /* HCH=0 */

    int rc = xhci_reset(&ctrl);
    assert_eq(0, rc);

    uint32_t cmd = *(uint32_t *)(ctrl.op_regs + XHCI_OP_USBCMD);
    assert_true((cmd & XHCI_CMD_RS) == 0);
}

static void test_xhci_reset_timeout_and_hse(void)
{
    TEST_CASE("xhci reset timeout and HSE detection");
    init_mock_mmio_defaults();

    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.mmio_virt = s_mock_mmio;
    ctrl.mmio_len = sizeof(s_mock_mmio);
    assert_eq(0, xhci_parse_capabilities(&ctrl));

    /* Inject HCRST hang (stuck at 1) */
    s_inject_hcrst_hang = true;
    int rc = xhci_reset(&ctrl);
    assert_eq(-ETIMEDOUT, rc);

    /* Inject HSE error */
    init_mock_mmio_defaults();
    s_inject_hse = true;
    rc = xhci_reset(&ctrl);
    assert_eq(-EIO, rc);
}

static void test_xhci_dma_alloc_and_link_trb(void)
{
    TEST_CASE("xhci DMA rings allocation and Link TRB setup");
    init_mock_mmio_defaults();
    reset_mock_pages();

    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.mmio_virt = s_mock_mmio;
    ctrl.mmio_len = sizeof(s_mock_mmio);
    assert_eq(0, xhci_parse_capabilities(&ctrl));

    int rc = xhci_alloc_dma(&ctrl);
    assert_eq(0, rc);
    assert_true(ctrl.dcbaa_dma.bus != 0);
    assert_true(ctrl.cmd_ring_dma.bus != 0);
    assert_true(ctrl.event_ring_dma.bus != 0);
    assert_true(ctrl.erst_dma.bus != 0);

    assert_eq(0, ctrl.cmd_ring_enqueue);
    assert_eq(1, ctrl.cmd_ring_cycle);
    assert_eq(0, ctrl.event_ring_dequeue);
    assert_eq(1, ctrl.event_ring_cycle);

    /* Check TRB 255 (Link TRB) */
    struct xhci_trb *link = &((struct xhci_trb *)ctrl.cmd_ring_dma.cpu)[255];
    assert_eq(ctrl.cmd_ring_dma.bus, link->parameter);
    uint32_t type = (link->control >> 10) & 0x3F;
    assert_eq(XHCI_TRB_TYPE_LINK, type);
    assert_true((link->control & XHCI_TRB_LINK_TC) != 0);

    /* Check ERST entry 0 */
    assert_eq(ctrl.event_ring_dma.bus, ((struct xhci_erst_entry *)ctrl.erst_dma.cpu)[0].ring_segment_base_address);
    assert_eq(XHCI_TRB_RING_SIZE, ((struct xhci_erst_entry *)ctrl.erst_dma.cpu)[0].ring_segment_size);

    /* Cleanup mock pages */
    free_4k_page(ctrl.dcbaa_dma.bus);
    free_4k_page(ctrl.cmd_ring_dma.bus);
    free_4k_page(ctrl.event_ring_dma.bus);
    free_4k_page(ctrl.erst_dma.bus);
}

static void test_xhci_command_ring_enqueue_and_wrap(void)
{
    TEST_CASE("xhci command ring enqueue and cycle bit toggle on wrap");
    init_mock_mmio_defaults();
    reset_mock_pages();

    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.mmio_virt = s_mock_mmio;
    ctrl.mmio_len = sizeof(s_mock_mmio);
    assert_eq(0, xhci_parse_capabilities(&ctrl));
    assert_eq(0, xhci_alloc_dma(&ctrl));

    struct xhci_trb dummy_cmd;
    memset(&dummy_cmd, 0, sizeof(dummy_cmd));
    dummy_cmd.control = (1 << 10); /* No-op command (Type 1) */

    /* Enqueue 254 commands (indices 0..253) */
    for (int i = 0; i < 254; i++) {
        dummy_cmd.parameter = 0x1000 + i;
        int rc = xhci_cmd_ring_enqueue(&ctrl, &dummy_cmd);
        assert_eq(0, rc);
        assert_eq(i + 1, ctrl.cmd_ring_enqueue);
        assert_eq(1, ctrl.cmd_ring_cycle);
        /* Check TRB parameter and cycle bit */
        assert_eq(0x1000 + i, ((struct xhci_trb *)ctrl.cmd_ring_dma.cpu)[i].parameter);
        assert_true((((struct xhci_trb *)ctrl.cmd_ring_dma.cpu)[i].control & XHCI_TRB_CYCLE_BIT) != 0);
    }

    /* Enqueue 255th command (index 254) -> should wrap! */
    dummy_cmd.parameter = 0x9999;
    int rc = xhci_cmd_ring_enqueue(&ctrl, &dummy_cmd);
    assert_eq(0, rc);

    /* Command was placed at index 254 with cycle bit 1 */
    assert_eq(0x9999, ((struct xhci_trb *)ctrl.cmd_ring_dma.cpu)[254].parameter);
    assert_true((((struct xhci_trb *)ctrl.cmd_ring_dma.cpu)[254].control & XHCI_TRB_CYCLE_BIT) != 0);

    /* Link TRB at index 255 had its cycle bit updated to 1 */
    assert_true((((struct xhci_trb *)ctrl.cmd_ring_dma.cpu)[255].control & XHCI_TRB_CYCLE_BIT) != 0);

    /* Enqueue index wrapped to 0, cycle flipped to 0! */
    assert_eq(0, ctrl.cmd_ring_enqueue);
    assert_eq(0, ctrl.cmd_ring_cycle);

    /* Enqueue one more command (256th, into index 0) -> cycle bit should now be 0 */
    dummy_cmd.parameter = 0xAAAA;
    rc = xhci_cmd_ring_enqueue(&ctrl, &dummy_cmd);
    assert_eq(0, rc);
    assert_eq(1, ctrl.cmd_ring_enqueue);
    assert_eq(0, ctrl.cmd_ring_cycle);
    assert_eq(0xAAAA, ((struct xhci_trb *)ctrl.cmd_ring_dma.cpu)[0].parameter);
    assert_true((((struct xhci_trb *)ctrl.cmd_ring_dma.cpu)[0].control & XHCI_TRB_CYCLE_BIT) == 0);

    /* Cleanup mock pages */
    free_4k_page(ctrl.dcbaa_dma.bus);
    free_4k_page(ctrl.cmd_ring_dma.bus);
    free_4k_page(ctrl.event_ring_dma.bus);
    free_4k_page(ctrl.erst_dma.bus);
}

static void test_xhci_probe_full_success(void)
{
    TEST_CASE("xhci full probe and running state");
    init_mock_mmio_defaults();
    reset_mock_pages();
    s_mock_cmd_reg = 0;

    struct pci_device dev;
    memset(&dev, 0, sizeof(dev));
    dev.bars[0].valid = true;
    dev.bars[0].kind = PCI_BAR_MMIO32;
    dev.bars[0].address = (uint64_t)(uintptr_t)s_mock_mmio;

    int rc = xhci_probe(&dev, &xhci_pci_driver.id_table[0]);
    assert_eq(0, rc);
    assert_true(dev.driver_data != NULL);

    struct xhci_controller *ctrl = (struct xhci_controller *)dev.driver_data;
    assert_true(ctrl->running);
    assert_eq(64, ctrl->max_slots);
    assert_eq(4, ctrl->max_ports);

    /* Verify PCI command register: Bus Master (bit 2) and Memory Space (bit 1) */
    assert_true((s_mock_cmd_reg & 0x04) != 0);
    assert_true((s_mock_cmd_reg & 0x02) != 0);

    /* Verify Operational registers */
    uint32_t config = *(uint32_t *)(ctrl->op_regs + XHCI_OP_CONFIG);
    assert_eq(16, config & 0xFF); /* MaxSlotsEn clamped to 16 */

    uint64_t dcbaap = *(uint64_t *)(ctrl->op_regs + XHCI_OP_DCBAAP);
    assert_eq(ctrl->dcbaa_dma.bus, dcbaap);

    uint64_t crcr = *(uint64_t *)(ctrl->op_regs + XHCI_OP_CRCR);
    assert_eq(ctrl->cmd_ring_dma.bus | 1, crcr);

    uint32_t usbcmd = *(uint32_t *)(ctrl->op_regs + XHCI_OP_USBCMD);
    assert_true((usbcmd & XHCI_CMD_RS) != 0);

    uint32_t usbsts = *(uint32_t *)(ctrl->op_regs + XHCI_OP_USBSTS);
    assert_true((usbsts & XHCI_STS_HCH) == 0);

    /* Cleanup via remove */
    xhci_remove(&dev);
    assert_true(dev.driver_data == NULL);
    assert_eq(0, s_mock_page_count);
    assert_true((s_mock_cmd_reg & 0x06) == 0);
}

static void test_xhci_probe_dma_cleanup_on_error(void)
{
    TEST_CASE("xhci probe failure cascade DMA cleanup");
    init_mock_mmio_defaults();
    reset_mock_pages();
    s_mock_cmd_reg = 0;

    struct pci_device dev;
    memset(&dev, 0, sizeof(dev));
    dev.bars[0].valid = true;
    dev.bars[0].kind = PCI_BAR_MMIO32;
    dev.bars[0].address = (uint64_t)(uintptr_t)s_mock_mmio;

    /* Inject allocation failure after 2 pages (3rd allocation fails) */
    s_mock_page_alloc_limit = 2;

    int rc = xhci_probe(&dev, &xhci_pci_driver.id_table[0]);
    assert_eq(-ENOMEM, rc);
    assert_true(dev.driver_data == NULL);

    /* Assert all previously allocated pages were freed */
    assert_eq(0, s_mock_page_count);

    /* Assert PCI command register was restored */
    assert_true((s_mock_cmd_reg & 0x06) == 0);
}

static void test_xhci_erst_and_interrupter(void)
{
    TEST_CASE("xhci ERST and Interrupter 0 configuration");
    init_mock_mmio_defaults();
    reset_mock_pages();

    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.mmio_virt = s_mock_mmio;
    ctrl.mmio_len = sizeof(s_mock_mmio);
    assert_eq(0, xhci_parse_capabilities(&ctrl));
    assert_eq(0, xhci_alloc_dma(&ctrl));

    xhci_setup_interrupter0(&ctrl);

    volatile uint8_t *ir0 = ctrl.rt_regs + 0x20;
    uint32_t erstsz = *(volatile uint32_t *)(ir0 + XHCI_INTR_ERSTSZ);
    uint64_t erstba = *(volatile uint64_t *)(ir0 + XHCI_INTR_ERSTBA);
    uint64_t erdp = *(volatile uint64_t *)(ir0 + XHCI_INTR_ERDP);
    uint32_t iman = *(volatile uint32_t *)(ir0 + XHCI_INTR_IMAN);

    assert_eq(1, erstsz);
    assert_eq(ctrl.erst_dma.bus, erstba);
    assert_eq(ctrl.event_ring_dma.bus | XHCI_ERDP_EHB, erdp);
    assert_true((iman & XHCI_IMAN_IE) == 0);

    /* Cleanup mock pages */
    free_4k_page(ctrl.dcbaa_dma.bus);
    free_4k_page(ctrl.cmd_ring_dma.bus);
    free_4k_page(ctrl.event_ring_dma.bus);
    free_4k_page(ctrl.erst_dma.bus);
}

static void test_program_rings_keeps_controller_stopped(void)
{
    TEST_CASE("xhci_program_rings keeps controller stopped until xhci_run");
    init_mock_mmio_defaults();
    reset_mock_pages();

    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.mmio_virt = s_mock_mmio;
    ctrl.mmio_len = sizeof(s_mock_mmio);
    assert_eq(0, xhci_parse_capabilities(&ctrl));
    assert_eq(0, xhci_alloc_dma(&ctrl));

    int rc = xhci_program_rings(&ctrl);
    assert_eq(0, rc);

    uint32_t usbcmd = *(volatile uint32_t *)(ctrl.op_regs + XHCI_OP_USBCMD);
    assert_true((usbcmd & XHCI_CMD_RS) == 0);
    assert_true(!ctrl.running);

    uint64_t dcbaap = *(volatile uint64_t *)(ctrl.op_regs + XHCI_OP_DCBAAP);
    uint64_t crcr = *(volatile uint64_t *)(ctrl.op_regs + XHCI_OP_CRCR);
    volatile uint8_t *ir0 = ctrl.rt_regs + 0x20;
    uint32_t erstsz = *(volatile uint32_t *)(ir0 + XHCI_INTR_ERSTSZ);
    uint64_t erstba = *(volatile uint64_t *)(ir0 + XHCI_INTR_ERSTBA);
    assert_eq(ctrl.dcbaa_dma.bus, dcbaap);
    assert_eq(ctrl.cmd_ring_dma.bus | 1, crcr);
    assert_eq(1, erstsz);
    assert_eq(ctrl.erst_dma.bus, erstba);

    rc = xhci_run(&ctrl);
    assert_eq(0, rc);
    usbcmd = *(volatile uint32_t *)(ctrl.op_regs + XHCI_OP_USBCMD);
    assert_true((usbcmd & XHCI_CMD_RS) != 0);
    assert_true(ctrl.running);

    assert_eq(0, xhci_stop(&ctrl));
    xhci_free_dma(&ctrl);
    assert_eq(0, s_mock_page_count);
}

static void test_xhci_driver_registration(void)
{
    TEST_CASE("xhci driver registration");
    assert_str_eq("xhci", xhci_pci_driver.name);
    assert_true(xhci_pci_driver.id_count > 0);
    assert_eq(0x0C0330, xhci_pci_driver.id_table[0].class_value);
    assert_eq(0xFFFFFF, xhci_pci_driver.id_table[0].class_mask);
}

int main(void)
{
    printf("========================================\n");
    printf("       OS01 xHCI Phase 1 Tests         \n");
    printf("========================================\n");

    test_xhci_driver_registration();
    test_xhci_cap_parsing();
    test_xhci_scratchpad_buffers();
    test_xhci_reset_success();
    test_xhci_reset_running_controller();
    test_xhci_reset_timeout_and_hse();
    test_xhci_dma_alloc_and_link_trb();
    test_xhci_command_ring_enqueue_and_wrap();
    test_xhci_erst_and_interrupter();
    test_program_rings_keeps_controller_stopped();
    test_xhci_probe_full_success();
    test_xhci_probe_dma_cleanup_on_error();

    reset_mock_pages();
    printf("\nAll xHCI Phase 1 tests passed!\n");
    return 0;
}
