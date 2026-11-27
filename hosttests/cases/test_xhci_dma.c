/* @os01-test
 * id: test_xhci_dma
 * layer: host
 * subsystem: drivers
 * timing: strict
 * tier: pr
 * link: xhci-dma
 * scenarios: [drivers.xhci.dma]
 */
#include "test_framework.h"
#include <device/device.h>
#include <bus/pci/pci.h>
#include <bus/pci/driver.h>
#include <driver/xhci.h>
#include <driver/xhci_dma.h>
#include <driver/usb_msc.h>
#include <arch/pci.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

#define MOCK_MMIO_SIZE (64 * 1024)
static uint8_t s_mock_mmio[MOCK_MMIO_SIZE];

/* Trace events */
enum trace_op {
    TRACE_NONE = 0,
    TRACE_CLEAN,
    TRACE_PREPARE_IN,
    TRACE_INVALIDATE_IN,
    TRACE_DOORBELL,
};

struct trace_entry {
    enum trace_op op;
    uintptr_t cpu;
    size_t size;
    uint32_t val;
};

#define MAX_TRACE 256
static struct trace_entry s_trace[MAX_TRACE];
static size_t s_trace_len = 0;

static void trace_push(enum trace_op op, uintptr_t cpu, size_t size, uint32_t val)
{
    if (s_trace_len < MAX_TRACE) {
        s_trace[s_trace_len++] = (struct trace_entry){
            .op = op, .cpu = cpu, .size = size, .val = val
        };
    }
}

static bool s_inject_hcrst_hang = false;
static bool s_inject_hch_hang = false;
static bool s_inject_run_hang = false;

void mock_xhci_write32(volatile void *addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
    uintptr_t offset = (uintptr_t)addr - (uintptr_t)s_mock_mmio;

    if (offset >= 0x400 && offset < 0x500) {
        trace_push(TRACE_DOORBELL, (uintptr_t)addr, 4, val);
    }

    if (offset == 0x20 + XHCI_OP_USBCMD) {
        volatile uint32_t *sts = (volatile uint32_t *)(s_mock_mmio + 0x20 + XHCI_OP_USBSTS);
        if ((val & XHCI_CMD_HCRST) && !s_inject_hcrst_hang) {
            *(volatile uint32_t *)addr &= ~XHCI_CMD_HCRST;
            *sts &= ~XHCI_STS_CNR;
            *sts |= XHCI_STS_HCH;
        }
        if (!(val & XHCI_CMD_RS)) {
            if (!s_inject_hch_hang)
                *sts |= XHCI_STS_HCH;
        } else if (!s_inject_run_hang) {
            *sts &= ~XHCI_STS_HCH;
        }
    }
}

/* Mock allocator with non-identity bus offset */
#define MOCK_PHYS_BASE 0x100000ULL
#define MOCK_BUS_OFFSET 0x2000000ULL /* phys=0x100000 -> bus=0x2100000 */
#define MAX_DMA_ALLOCS 32

struct mock_alloc_rec {
    void *cpu;
    uint64_t phys;
    uint64_t bus;
    size_t size;
    bool active;
};

static struct mock_alloc_rec s_allocs[MAX_DMA_ALLOCS];
static uint64_t s_next_phys = MOCK_PHYS_BASE;
static uint64_t s_forced_bus = 0;
static size_t s_active_allocs = 0;
static uint64_t s_last_freed_phys = 0;

static int mock_dma_alloc(void *ctx, size_t bytes, size_t align, struct xhci_dma_buf *out)
{
    (void)ctx;
    if (align < 4096) align = 4096;
    size_t alloc_sz = (bytes + 4095U) & ~4095ULL;
    void *cpu = calloc(1, alloc_sz);
    if (!cpu) return -ENOMEM;

    s_next_phys = (s_next_phys + (uint64_t)align - 1ULL) & ~((uint64_t)align - 1ULL);
    uint64_t phys = s_next_phys;
    s_next_phys += alloc_sz;
    uint64_t bus = s_forced_bus ? s_forced_bus : (phys + MOCK_BUS_OFFSET);

    for (size_t i = 0; i < MAX_DMA_ALLOCS; i++) {
        if (!s_allocs[i].active) {
            s_allocs[i] = (struct mock_alloc_rec){
                .cpu = cpu, .phys = phys, .bus = bus, .size = bytes, .active = true
            };
            s_active_allocs++;
            break;
        }
    }

    out->cpu = cpu;
    out->phys = phys;
    out->bus = bus;
    out->size = bytes;
    out->cookie = cpu;
    return 0;
}

static void mock_dma_free(void *ctx, struct xhci_dma_buf *buf)
{
    (void)ctx;
    if (!buf || !buf->cpu) return;
    s_last_freed_phys = buf->phys;
    for (size_t i = 0; i < MAX_DMA_ALLOCS; i++) {
        if (s_allocs[i].active && s_allocs[i].cpu == buf->cpu) {
            assert_eq(s_allocs[i].phys, buf->phys);
            s_allocs[i].active = false;
            s_active_allocs--;
            break;
        }
    }
    free(buf->cpu);
}

static void mock_dma_sync(void *ctx, void *cpu, size_t size, enum xhci_dma_sync_op op)
{
    (void)ctx;
    enum trace_op top = TRACE_NONE;
    if (op == XHCI_DMA_TO_DEVICE) top = TRACE_CLEAN;
    else if (op == XHCI_DMA_PREPARE_FROM_DEVICE) top = TRACE_PREPARE_IN;
    else if (op == XHCI_DMA_COMPLETE_FROM_DEVICE) top = TRACE_INVALIDATE_IN;
    trace_push(top, (uintptr_t)cpu, size, 0);
}

static const struct xhci_dma_ops s_mock_dma_ops = {
    .alloc = mock_dma_alloc,
    .free = mock_dma_free,
    .sync = mock_dma_sync,
};

uint64_t alloc_4k_page(void)
{
    void *p = aligned_alloc(4096, 4096);
    if (p) memset(p, 0, 4096);
    return (uint64_t)(uintptr_t)p;
}
void free_4k_page(uint64_t phys) { free((void *)(uintptr_t)phys); }
void *kmalloc(size_t size) { return malloc(size ? size : 1); }
size_t kfree(void *ptr) { free(ptr); return 1; }

/* Mock PCI backend */
static uint32_t s_mock_cmd_sts = 0xFFFF0000U; /* Non-zero W1C Status bits */
static uint32_t s_cmd_writes[32];
static size_t s_cmd_write_count = 0;

static int mock_pci_read32(uint16_t d, uint8_t b, uint8_t s, uint8_t f, uint16_t off,
                           uint32_t *val, enum pci_error_scope *scope)
{
    (void)d; (void)b; (void)s; (void)f; (void)scope;
    if (off == 0x04) *val = s_mock_cmd_sts;
    else *val = 0;
    return 0;
}

static int mock_pci_write32(uint16_t d, uint8_t b, uint8_t s, uint8_t f, uint16_t off,
                            uint32_t val, enum pci_error_scope *scope)
{
    (void)d; (void)b; (void)s; (void)f; (void)scope;
    if (off == 0x04) {
        if (s_cmd_write_count < 32) s_cmd_writes[s_cmd_write_count++] = val;
        assert_eq(0U, val >> 16); /* High 16 Status W1C bits must be zero */
        s_mock_cmd_sts = (s_mock_cmd_sts & 0xFFFF0000U) | (val & 0xFFFFU);
    }
    return 0;
}

static int mock_pci_map_mmio(uint16_t domain, uint64_t cpu_phys, uint64_t length, void **out_virt)
{
    (void)domain; (void)cpu_phys;
    if (length > MOCK_MMIO_SIZE) return -ENOENT;
    *out_virt = s_mock_mmio;
    return 0;
}

static const struct pci_backend s_mock_backend = {
    .read32 = mock_pci_read32,
    .write32 = mock_pci_write32,
    .map_mmio = mock_pci_map_mmio,
};

static bool s_inject_bot_runaway = false;
static struct xhci_controller *s_bot_ctrl = NULL;

int usb_msc_probe(const struct usb_msc_transport *transport, uint8_t iface_num,
                  uint8_t bulk_in_ep, uint8_t bulk_out_ep,
                  const char *disk_name, block_device_t **out_bdev)
{
    (void)transport; (void)iface_num; (void)bulk_in_ep; (void)bulk_out_ep;
    (void)disk_name; (void)out_bdev;
    if (s_inject_bot_runaway && s_bot_ctrl) {
        s_bot_ctrl->poisoned = true;
        s_bot_ctrl->dma_retained = true;
        return -ETIMEDOUT;
    }
    return 0;
}

const struct pci_backend *arch_pci_backend(void) { return &s_mock_backend; }
bool aarch64_pcie_xhci_bar0_ready(const struct pci_device *pdev) { (void)pdev; return true; }

static void init_mmio(void)
{
    memset(s_mock_mmio, 0, MOCK_MMIO_SIZE);
    s_inject_hcrst_hang = false;
    s_inject_hch_hang = false;
    s_inject_run_hang = false;
    s_trace_len = 0;
    s_next_phys = MOCK_PHYS_BASE;
    s_forced_bus = 0;
    s_active_allocs = 0;
    s_last_freed_phys = 0;
    memset(s_allocs, 0, sizeof(s_allocs));
    s_mock_cmd_sts = 0xFFFF0000U;
    s_cmd_write_count = 0;

    s_mock_mmio[XHCI_CAP_CAPLENGTH] = 0x20;
    *(uint16_t *)(s_mock_mmio + XHCI_CAP_HCIVERSION) = 0x0100;
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_HCSPARAMS1) = 4 | (1 << 8) | (2 << 24);
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_HCCPARAMS1) = 1U;
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_DBOFF) = 0x400;
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_RTSOFF) = 0x600;
    *(uint32_t *)(s_mock_mmio + 0x20 + XHCI_OP_PAGESIZE) = 1U;
    *(uint32_t *)(s_mock_mmio + 0x20 + XHCI_OP_USBSTS) = XHCI_STS_HCH;
}

static void test_non_identity_bus_and_free_phys(void)
{
    init_mmio();
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.mmio_virt = s_mock_mmio;
    ctrl.mmio_len = 0x4000;
    ctrl.dma_ops = &s_mock_dma_ops;

    assert_eq(0, xhci_parse_capabilities(&ctrl));
    assert_eq(0, xhci_alloc_dma(&ctrl));
    assert_eq(0, xhci_program_rings(&ctrl));

    /* phys=0x100000 -> bus=0x2100000 */
    assert_eq(MOCK_PHYS_BASE, ctrl.dcbaa_dma.phys);
    assert_eq(MOCK_PHYS_BASE + MOCK_BUS_OFFSET, ctrl.dcbaa_dma.bus);

    uint64_t dcbaap = *(volatile uint64_t *)(ctrl.op_regs + XHCI_OP_DCBAAP);
    uint64_t crcr = *(volatile uint64_t *)(ctrl.op_regs + XHCI_OP_CRCR);
    uint64_t erstba = *(volatile uint64_t *)(ctrl.rt_regs + 0x20 + XHCI_INTR_ERSTBA);
    assert_eq(ctrl.dcbaa_dma.bus, dcbaap);
    assert_eq(ctrl.cmd_ring_dma.bus | 1ULL, crcr);
    assert_eq(ctrl.erst_dma.bus, erstba);
    assert_eq(ctrl.event_ring_dma.bus, ((struct xhci_erst_entry *)ctrl.erst_dma.cpu)[0].ring_segment_base_address);
    assert_eq(ctrl.cmd_ring_dma.bus, ((struct xhci_trb *)ctrl.cmd_ring_dma.cpu)[255].parameter);

    ctrl.halted_confirmed = true;
    xhci_free_dma(&ctrl);
    assert_eq(0U, s_active_allocs);
    assert_true(s_last_freed_phys >= MOCK_PHYS_BASE && s_last_freed_phys < MOCK_PHYS_BASE + 0x10000ULL);
}

static void test_dma_bounds_and_mmio_spans(void)
{
    init_mmio();
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.mmio_virt = s_mock_mmio;
    ctrl.mmio_len = 0x4000;
    ctrl.dma_ops = &s_mock_dma_ops;
    assert_eq(0, xhci_parse_capabilities(&ctrl));

    /* AC64=0 rejects bus >= 4 GiB */
    ctrl.ac64 = false;
    s_forced_bus = 0x100000000ULL;
    struct xhci_dma_buf buf;
    assert_true(xhci_dma_alloc(&ctrl, 4096, 4096, 0, &buf) < 0);
    assert_eq(0U, s_active_allocs);

    /* Crossing 64 KiB boundary rejected */
    ctrl.ac64 = true;
    s_forced_bus = 0x20FF000ULL;
    assert_true(xhci_dma_alloc(&ctrl, 0x10000, 4096, 0x10000, &buf) < 0);
    assert_eq(0U, s_active_allocs);

    /* Tail overflow rejected */
    s_forced_bus = 0xFFFFFFFFFFFFF000ULL;
    assert_true(xhci_dma_alloc(&ctrl, 0x2000, 4096, 0, &buf) < 0);
    assert_eq(0U, s_active_allocs);

    /* Illegal doorbell span rejected */
    s_forced_bus = 0;
    *(uint32_t *)(s_mock_mmio + XHCI_CAP_DBOFF) = 0x3FF8; /* 2 slots + slot 0 = 12 bytes > 0x4000 */
    assert_true(xhci_parse_capabilities(&ctrl) < 0);
}

static void test_aarch64_pci_probe_safety(void)
{
    init_mmio();
    struct pci_device pdev;
    memset(&pdev, 0, sizeof(pdev));
    pdev.vendor = 0x1b36;
    pdev.device = 0x000d;
    pdev.class_code = 0x0c0330U;
    pdev.bars[0].valid = true;
    pdev.bars[0].kind = PCI_BAR_MMIO64;
    pdev.bars[0].address = 0x8000000000ULL;

    /* Running controller before reset -> DEVICE_UNSAFE, BME off */
    *(uint32_t *)(s_mock_mmio + 0x20 + XHCI_OP_USBCMD) = XHCI_CMD_RS;
    *(uint32_t *)(s_mock_mmio + 0x20 + XHCI_OP_USBSTS) = 0;
    s_inject_hch_hang = true;
    assert_eq(DEVICE_UNSAFE, xhci_pci_probe_aarch64(&pdev));
    assert_eq(0U, s_mock_cmd_sts & 0x04U);
    assert_true(s_cmd_write_count > 0);

    /* Start timeout with HCH=1 (controller never started) -> -ETIMEDOUT, BME off */
    init_mmio();
    s_inject_run_hang = true;
    assert_eq(-ETIMEDOUT, xhci_pci_probe_aarch64(&pdev));
    assert_eq(0U, s_mock_cmd_sts & 0x04U);

    /* Healthy start, then runaway during BOT probe */
    init_mmio();
    assert_eq(0, xhci_pci_probe_aarch64(&pdev));
    assert_true((s_mock_cmd_sts & 0x04U) != 0);
    struct xhci_controller *ctrl = (struct xhci_controller *)pdev.driver_data;
    assert_true(ctrl != NULL);
    struct xhci_slot *slot = calloc(1, sizeof(*slot));
    slot->ctrl = ctrl;
    slot->slot_id = 1;
    slot->active = true;
    slot->configured = true;
    slot->msc_iface.active = true;
    slot->msc_iface.in_addr = 0x81;
    slot->msc_iface.out_addr = 0x02;
    slot->msc_iface.in_dci = 3;
    slot->msc_iface.out_dci = 4;
    ctrl->slots[1] = slot;
    s_inject_bot_runaway = true;
    s_bot_ctrl = ctrl;
    uint8_t next_disk = 0;
    xhci_msc_probe_controller(ctrl, &next_disk);
    assert_true(ctrl->poisoned && ctrl->dma_retained);
    assert_eq(0U, next_disk);
    s_inject_bot_runaway = false;
    s_bot_ctrl = NULL;
    xhci_remove(&pdev);
}

static void test_trace_sync_ordering(void)
{
    init_mmio();
    struct xhci_controller ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.mmio_virt = s_mock_mmio;
    ctrl.mmio_len = 0x4000;
    ctrl.dma_ops = &s_mock_dma_ops;
    assert_eq(0, xhci_parse_capabilities(&ctrl));
    assert_eq(0, xhci_alloc_dma(&ctrl));
    assert_eq(0, xhci_program_rings(&ctrl));
    assert_eq(0, xhci_run(&ctrl));

    /* Setup slot 1 with bulk IN (DCI 3) and bulk OUT (DCI 2) */
    struct xhci_slot *slot = calloc(1, sizeof(*slot));
    assert_true(slot != NULL);
    slot->ctrl = &ctrl;
    slot->slot_id = 1;
    slot->active = true;
    slot->configured = true;
    slot->max_dci = 3;
    slot->ep_rings = calloc(4, sizeof(struct xhci_ep_ring *));
    assert_eq(0, xhci_dma_alloc(&ctrl, 4096U, 4096U, 0, &slot->dev_ctx_dma));
    slot->ep_rings[2] = calloc(1, sizeof(struct xhci_ep_ring));
    slot->ep_rings[2]->active = true;
    slot->ep_rings[2]->ep_type = XHCI_EP_TYPE_BULK_OUT;
    slot->ep_rings[2]->cycle = 1;
    assert_eq(0, xhci_dma_alloc(&ctrl, 4096U, 4096U, 65536U, &slot->ep_rings[2]->ring_dma));

    slot->ep_rings[3] = calloc(1, sizeof(struct xhci_ep_ring));
    slot->ep_rings[3]->active = true;
    slot->ep_rings[3]->ep_type = XHCI_EP_TYPE_BULK_IN;
    slot->ep_rings[3]->cycle = 1;
    assert_eq(0, xhci_dma_alloc(&ctrl, 4096U, 4096U, 65536U, &slot->ep_rings[3]->ring_dma));

    ctrl.slots[1] = slot;

    /* Pre-populate event ring with completion for OUT and short IN */
    uint8_t out_buf[512];
    memset(out_buf, 0xA5, sizeof(out_buf));
    struct xhci_trb *ev_ring = (struct xhci_trb *)ctrl.event_ring_dma.cpu;
    ev_ring[0].parameter = slot->ep_rings[2]->ring_dma.bus;
    ev_ring[0].status = (XHCI_COMP_SUCCESS << 24);
    ev_ring[0].control = (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | (1U << 24) | (2U << 16) | 1U;

    s_trace_len = 0;
    size_t actual = 0;
    assert_eq(0, xhci_bulk_transfer(&ctrl, 1, 2, out_buf, sizeof(out_buf), &actual));
    assert_eq(512U, actual);

    /* Verify clean payload -> clean TRB -> clean CYCLE -> doorbell */
    bool saw_payload = false, saw_cycle = false, saw_db = false;
    for (size_t i = 0; i < s_trace_len; i++) {
        if (s_trace[i].op == TRACE_CLEAN && s_trace[i].cpu == (uintptr_t)slot->bulk_staging_dma.cpu) {
            assert_true(!saw_cycle && !saw_db);
            saw_payload = true;
        } else if (s_trace[i].op == TRACE_CLEAN && s_trace[i].size == sizeof(uint32_t)) {
            assert_true(saw_payload && !saw_db);
            saw_cycle = true;
        } else if (s_trace[i].op == TRACE_DOORBELL) {
            assert_true(saw_payload && saw_cycle);
            saw_db = true;
        }
    }
    assert_true(saw_payload && saw_cycle && saw_db);

    /* Short IN transfer: prepare IN -> doorbell -> invalidate event -> invalidate IN */
    uint8_t in_buf[512];
    memset(in_buf, 0, sizeof(in_buf));
    memset(slot->bulk_staging_dma.cpu, 0x5A, 256);
    ev_ring[1].parameter = slot->ep_rings[3]->ring_dma.bus;
    ev_ring[1].status = (XHCI_COMP_SHORT_TX << 24) | 256U; /* 256 bytes residual -> 256 xferred */
    ev_ring[1].control = (XHCI_TRB_TYPE_TRANSFER_EVT << 10) | (1U << 24) | (3U << 16) | 1U;

    s_trace_len = 0;
    actual = 0;
    assert_eq(0, xhci_bulk_transfer(&ctrl, 1, 3, in_buf, sizeof(in_buf), &actual));
    assert_eq(256U, actual);
    assert_eq(0x5AU, in_buf[0]);

    bool saw_prep = false, saw_in_db = false, saw_inv_ev = false, saw_inv_in = false;
    for (size_t i = 0; i < s_trace_len; i++) {
        if (s_trace[i].op == TRACE_PREPARE_IN && s_trace[i].cpu == (uintptr_t)slot->bulk_staging_dma.cpu) {
            assert_true(!saw_in_db);
            saw_prep = true;
        } else if (s_trace[i].op == TRACE_DOORBELL) {
            assert_true(saw_prep);
            saw_in_db = true;
        } else if (s_trace[i].op == TRACE_INVALIDATE_IN && s_trace[i].size == sizeof(struct xhci_trb)) {
            assert_true(saw_in_db);
            saw_inv_ev = true;
        } else if (s_trace[i].op == TRACE_INVALIDATE_IN && s_trace[i].cpu == (uintptr_t)slot->bulk_staging_dma.cpu) {
            assert_true(saw_inv_ev);
            saw_inv_in = true;
        }
    }
    assert_true(saw_prep && saw_in_db && saw_inv_ev && saw_inv_in);

    assert_eq(0, xhci_stop(&ctrl));
    xhci_free_slot(&ctrl, 1);
    xhci_free_dma(&ctrl);
    assert_eq(0U, s_active_allocs);
}

int main(void)
{
    TEST_SUITE("xhci_dma");
    test_non_identity_bus_and_free_phys();
    test_dma_bounds_and_mmio_spans();
    test_trace_sync_ordering();
    test_aarch64_pci_probe_safety();
    TEST_RESULTS();
    return __test_stats.failed ? 1 : 0;
}
