/* @os01-test
 * id: test_aarch64_rp1_irq
 * layer: host
 * subsystem: intr
 * timing: strict
 * tier: nightly
 * link: rp1-irq
 * scenarios: [intr.aarch64.rp1_msix]
 */
#include "test_framework.h"
#include <arch/aarch64/rp1_irq.h>
#include <arch/aarch64/rp1_pcie.h>
#include <arch/aarch64/bcm2712_mip.h>
#include <arch/aarch64/gic.h>
#include <device/boot.h>
#include <errno.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ── Mock hardware & stubs ────────────────────────────────────────────── */
static uint32_t g_rc_regs[0x10000 / 4];
static uint32_t g_mip_regs[0x1000 / 4];
static uint32_t g_apb_regs[0x1000 / 4];
static uint32_t g_msix_table[64 * 4];
static uint32_t g_msix_pba[4];
static uint32_t g_gicd[0x400];
static uint32_t g_gicc[0x20];
static struct gic_dev g_gic_dev;
static struct aarch64_rp1_irq_platform g_plat;

static uint64_t g_now_ticks;
static uint64_t g_freq_hz = 54000000ULL;
static uint64_t g_ticks_per_poll = 1000ULL;

static bool g_deliver_test_msi = true;
static bool g_late_pba_after_timeout;
static bool g_drop_gic_disable;
static bool g_drop_mip_mask;
static bool g_drop_rp1_mask;
static bool g_drop_table_write;
static bool g_drop_final_enable_rb;
static bool g_drop_win_claim;
static unsigned g_unsafe_calls;
static jmp_buf g_unsafe_jmp;
static bool g_catch_unsafe;

static unsigned g_usb_apb_writes;
static unsigned g_pba_writes;
static unsigned g_msgctl_writes;
static unsigned g_mask_rb_before_enable;
static unsigned g_cb25_calls;
static unsigned g_cb31_calls;
static unsigned g_order_seq[16];
static unsigned g_order_len;

int g_log_level = 3;
void _log_info_impl(const char *fmt, ...) { (void)fmt; }
void _log_warn_impl(const char *fmt, ...) { (void)fmt; }
void _log_err_impl(const char *fmt, ...) { (void)fmt; }

void device_fail_unsafe(struct device *dev, const char *reason)
{
    (void)dev; (void)reason;
    g_unsafe_calls++;
    if (g_catch_unsafe)
        longjmp(g_unsafe_jmp, 1);
    while (1) {}
}

uint64_t rp1_irq_test_now(void)
{
    g_now_ticks += g_ticks_per_poll;
    return g_now_ticks;
}

uint64_t rp1_irq_test_freq(void)
{
    return g_freq_hz;
}

struct gic_dev *gic_dev_current(void) { return &g_gic_dev; }
uint8_t gic_target_bit_get(uint32_t cpu) { return cpu == 0 ? 0x01u : 0u; }
const struct aarch64_rp1_irq_platform *aarch64_rp1_irq_platform_get(void) { return &g_plat; }

int aarch64_rp1_mmio_read32(const struct aarch64_rp1_irq_platform *p,
                            enum aarch64_rp1_mmio_space space,
                            uint32_t off, uint32_t *value)
{
    if (!p || !p->valid || !value || (off & 3u) != 0u)
        return -EINVAL;
    switch (space) {
    case RP1_MMIO_RC:
        *value = g_rc_regs[off / 4u];
        return 0;
    case RP1_MMIO_MIP:
        *value = g_mip_regs[off / 4u];
        return 0;
    case RP1_MMIO_APB:
        if (g_drop_final_enable_rb && off == 0x08u + 4u * 25u) {
            *value = 0u;
            return 0;
        }
        *value = g_apb_regs[off / 4u];
        return 0;
    case RP1_MMIO_TABLE:
        *value = g_msix_table[off / 4u];
        return 0;
    case RP1_MMIO_PBA:
        if (g_late_pba_after_timeout && off == 0u)
            *value = (1u << 25);
        else
            *value = g_msix_pba[off / 4u];
        return 0;
    default:
        return -EINVAL;
    }
}

int aarch64_rp1_mmio_write32(const struct aarch64_rp1_irq_platform *p,
                             enum aarch64_rp1_mmio_space space,
                             uint32_t off, uint32_t value)
{
    if (!p || !p->valid || (off & 3u) != 0u)
        return -EINVAL;
    switch (space) {
    case RP1_MMIO_RC:
        if (g_drop_win_claim && off == 0x40d4u)
            return 0;
        g_rc_regs[off / 4u] = value;
        return 0;
    case RP1_MMIO_MIP:
        if (g_drop_mip_mask && (off == 0x40u || off == 0x50u))
            return 0;
        g_mip_regs[off / 4u] = value;
        return 0;
    case RP1_MMIO_APB: {
        uint32_t base_off = off;
        bool is_set = false, is_clr = false;
        if (off >= 0x800u && off < 0xc00u) { base_off = off - 0x800u; is_set = true; }
        else if (off >= 0xc00u) { base_off = off - 0xc00u; is_clr = true; }
        if (base_off == 0x08u + 4u * 30u || base_off == 0x08u + 4u * 31u)
            g_usb_apb_writes++;
        if (g_drop_rp1_mask && is_clr && (value & 1u))
            return 0;
        if (is_set) {
            g_apb_regs[base_off / 4u] |= (value & ~4u);
            if ((value & 2u) && g_deliver_test_msi) {
                /* Simulate RP1 MSI-X delivery to GIC via table entry */
                uint32_t src = (base_off - 0x08u) / 4u;
                uint32_t slot = g_msix_table[src * 4u + 2u];
                uint32_t intid = 32u + g_plat.desc.base_spi + slot;
                g_gicd[(0x200u + (intid / 32u) * 4u) / 4u] |= (1u << (intid % 32u));
            }
            if ((value & 4u) && g_order_len < 16u)
                g_order_seq[g_order_len++] = 2u; /* IACK */
        } else if (is_clr) {
            g_apb_regs[base_off / 4u] &= ~value;
        } else {
            g_apb_regs[base_off / 4u] = value;
        }
        return 0;
    }
    case RP1_MMIO_TABLE:
        if (g_drop_table_write)
            return 0;
        if (g_drop_rp1_mask && (off % 16u) == 12u && (value & 1u))
            return 0;
        g_msix_table[off / 4u] = value;
        if ((off % 16u) == 12u && (value & 1u) && (g_rc_regs[0x8052u / 4u] & 0x8000u) == 0u)
            g_mask_rb_before_enable++;
        return 0;
    case RP1_MMIO_PBA:
        g_pba_writes++;
        return -EACCES;
    default:
        return -EINVAL;
    }
}

int aarch64_rp1_cfg_read16(const struct aarch64_rp1_irq_platform *p, uint16_t off, uint16_t *value)
{
    (void)p;
    uint32_t dw = g_rc_regs[(0x8000u + (off & ~3u)) / 4u];
    *value = (uint16_t)((off & 2u) ? (dw >> 16) : (dw & 0xffffu));
    return 0;
}

int aarch64_rp1_cfg_write16(const struct aarch64_rp1_irq_platform *p, uint16_t off, uint16_t value)
{
    (void)p;
    if (off == 0x52u)
        g_msgctl_writes++;
    uint32_t idx = (0x8000u + (off & ~3u)) / 4u;
    if (off & 2u)
        g_rc_regs[idx] = (g_rc_regs[idx] & 0x0000ffffu) | ((uint32_t)value << 16);
    else
        g_rc_regs[idx] = (g_rc_regs[idx] & 0xffff0000u) | value;
    return 0;
}

int aarch64_rp1_irq_flush(const struct aarch64_rp1_irq_platform *p, uint32_t source)
{
    (void)p; (void)source;
    if (g_order_len < 16u)
        g_order_seq[g_order_len++] = 3u; /* FLUSH */
    return 0;
}

void gic_host_mmio_write(volatile uint32_t *address, uint32_t value)
{
    if (address == &g_gicc[0x010u / 4u]) {
        if (g_order_len < 16u)
            g_order_seq[g_order_len++] = 4u; /* EOI */
    }
    if (address >= &g_gicd[0x100u / 4u] && address < &g_gicd[0x180u / 4u]) {
        *address |= value;
        return;
    }
    if (address >= &g_gicd[0x180u / 4u] && address < &g_gicd[0x200u / 4u]) {
        if (g_drop_gic_disable)
            return;
        uint32_t idx = (uint32_t)(address - &g_gicd[0x180u / 4u]);
        g_gicd[(0x100u / 4u) + idx] &= ~value;
        return;
    }
    if (address >= &g_gicd[0x200u / 4u] && address < &g_gicd[0x280u / 4u]) {
        *address |= value;
        return;
    }
    if (address >= &g_gicd[0x280u / 4u] && address < &g_gicd[0x300u / 4u]) {
        uint32_t idx = (uint32_t)(address - &g_gicd[0x280u / 4u]);
        g_gicd[(0x200u / 4u) + idx] &= ~value;
        return;
    }
    *address = value;
}

uint32_t gic_host_mmio_read(volatile uint32_t *address) { return *address; }

static void cb_uart25(void *arg)
{
    (void)arg;
    g_cb25_calls++;
    if (g_order_len < 16u)
        g_order_seq[g_order_len++] = 1u; /* CALLBACK */
}

static void cb_usb31(void *arg)
{
    (void)arg;
    g_cb31_calls++;
    if (g_order_len < 16u)
        g_order_seq[g_order_len++] = 1u; /* CALLBACK */
}

static void reset_env(void)
{
    memset(g_rc_regs, 0, sizeof(g_rc_regs));
    memset(g_mip_regs, 0, sizeof(g_mip_regs));
    memset(g_apb_regs, 0, sizeof(g_apb_regs));
    memset(g_msix_table, 0, sizeof(g_msix_table));
    memset(g_msix_pba, 0, sizeof(g_msix_pba));
    memset(g_gicd, 0, sizeof(g_gicd));
    memset(g_gicc, 0, sizeof(g_gicc));
    memset(&g_plat, 0, sizeof(g_plat));

    g_now_ticks = 0;
    g_deliver_test_msi = true;
    g_late_pba_after_timeout = false;
    g_drop_gic_disable = false;
    g_drop_mip_mask = false;
    g_drop_rp1_mask = false;
    g_drop_table_write = false;
    g_drop_final_enable_rb = false;
    g_drop_win_claim = false;
    g_unsafe_calls = 0;
    g_catch_unsafe = false;
    g_usb_apb_writes = 0;
    g_pba_writes = 0;
    g_msgctl_writes = 0;
    g_mask_rb_before_enable = 0;
    g_cb25_calls = 0;
    g_cb31_calls = 0;
    g_order_len = 0;

    g_gicd[0x008u / 4u] = 0x0200043bu;
    g_gicd[0x004u / 4u] = 7u; /* 256 INTIDs */
    assert_eq(0, gic_dev_init(&g_gic_dev, g_gicd, g_gicc));
    for (uint32_t i = 0; i < 1020u; i++)
        (void)gic_unregister_handler(i);

    g_plat.valid = true;
    g_plat.desc.valid = true;
    g_plat.desc.mip_cpu_pa = UINT64_C(0x1000130000);
    g_plat.desc.mip_span = 0xc0u;
    g_plat.desc.msi_pci_addr = UINT64_C(0xfffffff000);
    g_plat.desc.base_spi = 128u;
    g_plat.desc.count = 64u;
    g_plat.desc.offset = 0u;
    g_plat.bar0.entries = 61u;
    g_plat.bar0.msix_cap = 0x50u;
    g_plat.rc_va = (volatile uint8_t *)g_rc_regs;
    g_plat.mip_va = (volatile uint8_t *)g_mip_regs;
    g_plat.apb_va = (volatile uint8_t *)g_apb_regs;
    g_plat.table_va = (volatile uint8_t *)g_msix_table;
    g_plat.pba_va = (volatile uint8_t *)g_msix_pba;
    g_plat.rc_span = 0x10000u;
    g_plat.mip_span = 0x1000u;
    g_plat.apb_span = 0x1000u;
    g_plat.table_span = 61u * 16u;
    g_plat.pba_span = 8u;

    /* WoR state: MSI-X Enable=0, USBHOST APB already enabled */
    g_rc_regs[0x8050u / 4u] = (60u << 16) | 0x0011u;
    g_apb_regs[(0x08u + 4u * 30u) / 4u] = 1u;
    g_apb_regs[(0x08u + 4u * 31u) / 4u] = 0u;

    extern void rp1_irq_test_reset(void);
    rp1_irq_test_reset();
}

static void dispatch_intid(uint32_t intid)
{
    g_gicc[0x00Cu / 4u] = intid;
    gic_dev_dispatch(&g_gic_dev, NULL);
}

static void test_two_sources_and_dispatch_order(void)
{
    struct rp1_irq *irq25 = NULL, *irq31 = NULL, *dup = NULL;

    reset_env();

    /* Invalid sources / trigger types */
    assert_true(rp1_irq_prepare(61u, RP1_IRQ_LEVEL_HIGH, cb_uart25, NULL, &dup) < 0);
    assert_true(rp1_irq_prepare(25u, RP1_IRQ_EDGE_RISING, cb_uart25, NULL, &dup) < 0);
    assert_true(rp1_irq_prepare(31u, RP1_IRQ_LEVEL_HIGH, cb_usb31, NULL, &dup) < 0);

    /* Source 25 (level-high) */
    assert_eq(0, rp1_irq_prepare(25u, RP1_IRQ_LEVEL_HIGH, cb_uart25, NULL, &irq25));
    assert_true(g_mask_rb_before_enable >= 61u);
    assert_eq(0u, g_usb_apb_writes);
    assert_eq(0u, g_pba_writes);
    assert_eq(0u, g_cb25_calls); /* callback NOT called during probe */

    /* Prepare-state trampoline must ignore callback */
    dispatch_intid(rp1_irq_intid(irq25));
    assert_eq(0u, g_cb25_calls);

    assert_eq(0, rp1_irq_commit(irq25));
    /* Duplicate source 25 rejected */
    assert_true(rp1_irq_prepare(25u, RP1_IRQ_LEVEL_HIGH, cb_uart25, NULL, &dup) < 0);

    /* Source 31 (edge-rising) */
    assert_eq(0, rp1_irq_prepare(31u, RP1_IRQ_EDGE_RISING, cb_usb31, NULL, &irq31));
    assert_eq(0, rp1_irq_commit(irq31));
    assert_true(rp1_irq_intid(irq25) != rp1_irq_intid(irq31));

    /* Level dispatch order: callback(1), IACK(2), flush(3), EOI(4) */
    g_order_len = 0;
    dispatch_intid(rp1_irq_intid(irq25));
    assert_eq(1u, g_cb25_calls);
    assert_eq(4u, g_order_len);
    assert_eq(1u, g_order_seq[0]);
    assert_eq(2u, g_order_seq[1]);
    assert_eq(3u, g_order_seq[2]);
    assert_eq(4u, g_order_seq[3]);

    /* Edge dispatch order: callback(1), flush(3), EOI(4) — no IACK */
    g_order_len = 0;
    dispatch_intid(rp1_irq_intid(irq31));
    assert_eq(1u, g_cb31_calls);
    assert_eq(3u, g_order_len);
    assert_eq(1u, g_order_seq[0]);
    assert_eq(3u, g_order_seq[1]);
    assert_eq(4u, g_order_seq[2]);
}

static void test_firmware_owner_and_rollbacks(void)
{
    struct rp1_irq *irq25 = NULL, *irq26 = NULL;

    /* 1. Firmware Enable=1 rejected without touching global control */
    reset_env();
    g_rc_regs[0x8050u / 4u] |= (0x8000u << 16);
    assert_true(rp1_irq_prepare(25u, RP1_IRQ_LEVEL_HIGH, cb_uart25, NULL, &irq25) < 0);
    assert_eq(0u, g_msgctl_writes);

    /* 2. Clean abort restores everything */
    reset_env();
    assert_eq(0, rp1_irq_prepare(25u, RP1_IRQ_LEVEL_HIGH, cb_uart25, NULL, &irq25));
    assert_eq(0, rp1_irq_abort(irq25, true));
    assert_eq(0u, (g_rc_regs[0x8050u / 4u] >> 16) & 0x8000u);

    /* 3. Timeout with late PBA quarantines second source while first keeps delivering */
    reset_env();
    assert_eq(0, rp1_irq_prepare(25u, RP1_IRQ_LEVEL_HIGH, cb_uart25, NULL, &irq25));
    assert_eq(0, rp1_irq_commit(irq25));
    g_deliver_test_msi = false;
    g_late_pba_after_timeout = true;
    assert_true(rp1_irq_prepare(26u, RP1_IRQ_LEVEL_HIGH, cb_uart25, NULL, &irq26) < 0);
    /* First source still delivers */
    dispatch_intid(rp1_irq_intid(irq25));
    assert_eq(1u, g_cb25_calls);

    /* 4. Unverified device mask forces quarantine even on abort */
    reset_env();
    assert_eq(0, rp1_irq_prepare(25u, RP1_IRQ_LEVEL_HIGH, cb_uart25, NULL, &irq25));
    uint32_t intid25 = rp1_irq_intid(irq25);
    assert_eq(-EIO, rp1_irq_abort(irq25, false));
    dispatch_intid(intid25);
    assert_eq(0u, g_cb25_calls);

    /* 5. GIC disable fails, MIP mask verified -> quarantine allows poll (no unsafe) */
    reset_env();
    g_drop_gic_disable = true;
    assert_eq(0, rp1_irq_prepare(25u, RP1_IRQ_LEVEL_HIGH, cb_uart25, NULL, &irq25));
    assert_eq(-EIO, rp1_irq_abort(irq25, false));
    assert_eq(0u, g_unsafe_calls);

    /* 6. All 3 barriers fail -> device_fail_unsafe */
    reset_env();
    assert_eq(0, rp1_irq_prepare(25u, RP1_IRQ_LEVEL_HIGH, cb_uart25, NULL, &irq25));
    g_drop_gic_disable = true;
    g_drop_mip_mask = true;
    g_drop_rp1_mask = true;
    g_catch_unsafe = true;
    if (setjmp(g_unsafe_jmp) == 0) {
        (void)rp1_irq_abort(irq25, false);
        assert_true(false);
    }
    assert_eq(1u, g_unsafe_calls);

    /* 7. Fault coverage: window claim fail, table write fail, final enable readback fail */
    reset_env();
    g_drop_win_claim = true;
    assert_true(rp1_irq_prepare(25u, RP1_IRQ_LEVEL_HIGH, cb_uart25, NULL, &irq25) < 0);

    reset_env();
    g_drop_table_write = true;
    assert_true(rp1_irq_prepare(25u, RP1_IRQ_LEVEL_HIGH, cb_uart25, NULL, &irq25) < 0);

    reset_env();
    g_drop_final_enable_rb = true;
    assert_true(rp1_irq_prepare(25u, RP1_IRQ_LEVEL_HIGH, cb_uart25, NULL, &irq25) < 0);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_two_sources_and_dispatch_order),
    TEST_ENTRY(test_firmware_owner_and_rollbacks),
TEST_LIST_END

int main(void)
{
    TEST_SUITE("AArch64 RP1 MSI-X interrupt service");
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}
