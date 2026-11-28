/* @os01-test
 * id: test_aarch64_bcm2712_mip
 * layer: host
 * subsystem: intr
 * timing: strict
 * tier: nightly
 * link: rp1-mip
 * scenarios: [intr.aarch64.bcm2712_mip]
 */
#include "test_framework.h"
#include <arch/aarch64/rp1_pcie.h>
#include <arch/aarch64/bcm2712_mip.h>
#include <arch/aarch64/gic.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ── Mock hardware & stubs ────────────────────────────────────────────── */
static uint32_t g_rc_regs[0x10000 / 4];
static uint32_t g_mip_regs[0x1000 / 4];
static uint32_t g_gicd[0x400];
static uint32_t g_gicc[0x20];
static struct gic_dev g_gic_dev;
static struct aarch64_rp1_irq_platform g_plat;

static uint8_t g_bsp_target = 0x01u;
static bool g_drop_rc_bar4_write;
static bool g_drop_rc_bar4_restore;
static bool g_drop_mip_cfg_write;
static bool g_drop_mip_hmask_set;
static bool g_drop_gic_icfgr_write;
static bool g_drop_gic_enable_write;
static bool g_drop_gic_restore_write;
static uint32_t g_rc_write_log[32];
static unsigned g_rc_write_count;

int g_log_level = 3;
void _log_info_impl(const char *fmt, ...) { (void)fmt; }
void _log_warn_impl(const char *fmt, ...) { (void)fmt; }
void _log_err_impl(const char *fmt, ...) { (void)fmt; }

struct gic_dev *gic_dev_current(void) { return &g_gic_dev; }
uint8_t gic_target_bit_get(uint32_t cpu) { return cpu == 0 ? g_bsp_target : 0u; }

int aarch64_rp1_mmio_read32(const struct aarch64_rp1_irq_platform *p,
                            enum aarch64_rp1_mmio_space space,
                            uint32_t off, uint32_t *value)
{
    if (!p || !p->valid || !value || (off & 3u) != 0u)
        return -EINVAL;
    if (space == RP1_MMIO_RC) {
        if (off >= sizeof(g_rc_regs))
            return -ERANGE;
        *value = g_rc_regs[off / 4u];
        return 0;
    }
    if (space == RP1_MMIO_MIP) {
        if (off >= sizeof(g_mip_regs))
            return -ERANGE;
        *value = g_mip_regs[off / 4u];
        return 0;
    }
    return -EINVAL;
}

int aarch64_rp1_mmio_write32(const struct aarch64_rp1_irq_platform *p,
                             enum aarch64_rp1_mmio_space space,
                             uint32_t off, uint32_t value)
{
    if (!p || !p->valid || (off & 3u) != 0u)
        return -EINVAL;
    if (space == RP1_MMIO_RC) {
        if (off >= sizeof(g_rc_regs))
            return -ERANGE;
        if (g_rc_write_count < 32u)
            g_rc_write_log[g_rc_write_count++] = off;
        if (g_drop_rc_bar4_write && off == 0x40d4u && (value & 0x1fu) != 0u)
            return 0;
        if (g_drop_rc_bar4_restore && off == 0x410cu && value == 0u)
            return 0;
        g_rc_regs[off / 4u] = value;
        return 0;
    }
    if (space == RP1_MMIO_MIP) {
        if (off >= sizeof(g_mip_regs))
            return -ERANGE;
        if (g_drop_mip_cfg_write && (off == 0x20u || off == 0x30u))
            return 0;
        if (g_drop_mip_hmask_set && off == 0x40u && value != 0u)
            return 0;
        g_mip_regs[off / 4u] = value;
        return 0;
    }
    return -EINVAL;
}

void gic_host_mmio_write(volatile uint32_t *address, uint32_t value)
{
    if (g_drop_gic_icfgr_write && address >= &g_gicd[0xC00u / 4u] &&
        address < &g_gicd[0xD00u / 4u])
        return;
    if (g_drop_gic_enable_write && address >= &g_gicd[0x100u / 4u] &&
        address < &g_gicd[0x180u / 4u])
        return;
    if (g_drop_gic_restore_write && address >= &g_gicd[0x400u / 4u] &&
        address < &g_gicd[0x480u / 4u])
        return;
    if (address >= &g_gicd[0x100u / 4u] && address < &g_gicd[0x180u / 4u]) {
        *address |= value;
        return;
    }
    if (address >= &g_gicd[0x180u / 4u] && address < &g_gicd[0x200u / 4u]) {
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

uint32_t gic_host_mmio_read(volatile uint32_t *address)
{
    return *address;
}

static void dummy_handler(uint32_t intid, uint64_t param, struct pt_regs *regs)
{
    (void)intid; (void)param; (void)regs;
}

static void reset_env(uint32_t nr_lines, bool security_extn)
{
    memset(g_rc_regs, 0, sizeof(g_rc_regs));
    memset(g_mip_regs, 0, sizeof(g_mip_regs));
    memset(g_gicd, 0, sizeof(g_gicd));
    memset(g_gicc, 0, sizeof(g_gicc));
    memset(&g_plat, 0, sizeof(g_plat));
    g_bsp_target = 0x01u;
    g_drop_rc_bar4_write = false;
    g_drop_rc_bar4_restore = false;
    g_drop_mip_cfg_write = false;
    g_drop_mip_hmask_set = false;
    g_drop_gic_icfgr_write = false;
    g_drop_gic_enable_write = false;
    g_drop_gic_restore_write = false;
    g_rc_write_count = 0;

    g_gicd[0x008u / 4u] = 0x0200043bu; /* IIDR */
    g_gicd[0x004u / 4u] = ((nr_lines - 1u) & 0x1fu) |
                           (security_extn ? (1u << 10) : 0u);
    assert_eq(0, gic_dev_init(&g_gic_dev, g_gicd, g_gicc));
    for (uint32_t i = 0; i < 1020u; i++)
        (void)gic_unregister_handler(i);

    g_plat.valid = true;
    g_plat.desc.valid = true;
    g_plat.desc.rc_reg_base = UINT64_C(0x1000120000);
    g_plat.desc.rc_reg_size = 0x9310u;
    g_plat.desc.mip_cpu_pa = UINT64_C(0x1000130000);
    g_plat.desc.mip_span = 0xc0u;
    g_plat.desc.msi_pci_addr = UINT64_C(0xfffffff000);
    g_plat.desc.base_spi = 128u;
    g_plat.desc.count = 64u;
    g_plat.desc.offset = 0u;
    g_plat.rc_va = (volatile uint8_t *)g_rc_regs;
    g_plat.mip_va = (volatile uint8_t *)g_mip_regs;
    g_plat.rc_span = 0x10000u;
    g_plat.mip_span = 0x1000u;

    /* Existing xHCI BAR2 DMA window [0..2 GiB) */
    g_rc_regs[0x4034u / 4u] = 16u;
}

static void test_inbound_window_borrow_program_and_conflicts(void)
{
    struct aarch64_rp1_msi_window win;

    /* 1. Borrow existing 4 KiB window in BAR4 with zero RC writes */
    reset_env(8u, false);
    g_rc_regs[0x40d4u / 4u] = 0xfffff000u | 0x1cu;
    g_rc_regs[0x40d8u / 4u] = 0x000000ffu;
    g_rc_regs[0x410cu / 4u] = 0x00130000u | 1u;
    g_rc_regs[0x4110u / 4u] = 0x00000010u;
    assert_eq(0, aarch64_rp1_msi_window_claim(&g_plat, &win));
    assert_false(win.programmed);
    assert_eq(4u, win.slot);
    assert_eq(0u, g_rc_write_count);
    assert_eq(0, aarch64_rp1_msi_window_restore(&g_plat, &win));
    assert_eq(0u, g_rc_write_count);

    /* 2. Program free BAR4, verify order (size last; disable size first on restore) */
    reset_env(8u, false);
    assert_eq(0, aarch64_rp1_msi_window_claim(&g_plat, &win));
    assert_true(win.programmed);
    assert_eq(4u, win.slot);
    assert_eq(0xfffff000u | 0x1cu, g_rc_regs[0x40d4u / 4u]);
    assert_eq(0x000000ffu, g_rc_regs[0x40d8u / 4u]);
    assert_eq(0x00130000u | 1u, g_rc_regs[0x410cu / 4u]);
    assert_eq(0x00000010u, g_rc_regs[0x4110u / 4u]);
    assert_true(g_rc_write_count >= 5u);
    assert_eq(0x40d4u, g_rc_write_log[g_rc_write_count - 1u]);

    g_rc_write_count = 0;
    assert_eq(0, aarch64_rp1_msi_window_restore(&g_plat, &win));
    assert_eq(0x40d4u, g_rc_write_log[0]); /* size cleared first */
    assert_eq(0u, g_rc_regs[0x40d4u / 4u]);
    assert_eq(0u, g_rc_regs[0x410cu / 4u]);
    assert_eq(16u, g_rc_regs[0x4034u / 4u]); /* BAR2 DMA untouched */

    /* 3. Wide window mismatch (> 4 KiB) conflicts */
    reset_env(8u, false);
    g_rc_regs[0x40d4u / 4u] = 0xffff0000u | 1u; /* 64 KiB */
    g_rc_regs[0x40d8u / 4u] = 0x000000ffu;
    g_rc_regs[0x410cu / 4u] = 0x00130000u | 1u;
    g_rc_regs[0x4110u / 4u] = 0x00000010u;
    assert_true(aarch64_rp1_msi_window_claim(&g_plat, &win) < 0);

    /* 4. Internal MSI address match conflicts */
    reset_env(8u, false);
    g_rc_regs[0x4044u / 4u] = 0xfffff000u | 1u;
    g_rc_regs[0x4048u / 4u] = 0x000000ffu;
    assert_true(aarch64_rp1_msi_window_claim(&g_plat, &win) < 0);

    /* 5. Unknown size encoding conflicts */
    reset_env(8u, false);
    g_rc_regs[0x40d4u / 4u] = 25u; /* unknown */
    assert_true(aarch64_rp1_msi_window_claim(&g_plat, &win) < 0);

    /* 6. No free slot (all BAR4..10 busy with non-overlapping ranges) */
    reset_env(8u, false);
    for (unsigned i = 0; i < 7u; i++) {
        g_rc_regs[(0x40d4u + 8u * i) / 4u] = (i * 0x10000u) | 0x1cu;
        g_rc_regs[(0x410cu + 8u * i) / 4u] = 1u;
    }
    assert_true(aarch64_rp1_msi_window_claim(&g_plat, &win) < 0);

    /* 7. Readback failure on BAR4 + restore failure quarantines slot */
    reset_env(8u, false);
    g_drop_rc_bar4_write = true;
    g_drop_rc_bar4_restore = true;
    assert_true(aarch64_rp1_msi_window_claim(&g_plat, &win) < 0);
    assert_true(win.quarantined);
    g_drop_rc_bar4_write = false;
    g_drop_rc_bar4_restore = false;
    assert_eq(0, aarch64_rp1_msi_window_claim(&g_plat, &win));
    assert_eq(5u, win.slot); /* skipped quarantined slot 4 */
}

static void test_mip_slots_and_gic_spi_state(void)
{
    struct bcm2712_mip_route r0, r1, r2;
    struct gic_spi_state saved;

    /* 1. base_spi=128, offset=8, count=8, skip slot 0 (pending) & slot 1 (handler) -> slot 2 */
    reset_env(8u, false); /* nr_intids = 256 */
    g_plat.desc.base_spi = 128u;
    g_plat.desc.offset = 8u;
    g_plat.desc.count = 8u;
    g_bsp_target = 0x04u;

    /* INTID 168 (slot 0): pending in GIC -> must be skipped without touching */
    g_gicd[(0x200u + (168u / 32u) * 4u) / 4u] = (1u << (168u % 32u));
    /* INTID 169 (slot 1): existing handler -> must be skipped */
    assert_eq(0, gic_register_handler(169u, dummy_handler, 1u, "other"));

    /* Pre-populate INTID 170 (slot 2) GIC state to verify full snapshot & restore */
    g_gicd[(0x400u + (170u / 4u) * 4u) / 4u] = (0xa0u << ((170u % 4u) * 8u));
    g_gicd[(0x800u + (170u / 4u) * 4u) / 4u] = (0x02u << ((170u % 4u) * 8u));
    assert_eq(0, gic_spi_get_state(&g_gic_dev, 170u, &saved));
    assert_eq(0xa0u, saved.priority);
    assert_eq(0x02u, saved.target);
    assert_false(saved.edge);
    assert_true(saved.group_writable);

    assert_eq(0, bcm2712_mip_reserve(&g_plat, dummy_handler, 0x25u, &r0));
    assert_eq(10u, r0.data);
    assert_eq(170u, r0.intid);
    assert_eq(UINT64_C(0xfffffff000), r0.address);
    assert_eq(0, bcm2712_mip_enable(&r0));
    assert_eq(0, bcm2712_mip_disable(&r0));
    assert_eq(0, bcm2712_mip_clear_probe(&r0));
    assert_eq(0, bcm2712_mip_release(&r0, true));

    /* Verify GIC priority, target, edge restored */
    struct gic_spi_state after;
    assert_eq(0, gic_spi_get_state(&g_gic_dev, 170u, &after));
    assert_eq(0xa0u, after.priority);
    assert_eq(0x02u, after.target);
    assert_false(after.edge);
    assert_true(gic_get_handler(170u, NULL) == NULL);

    /* 2. SecurityExtn=1 disables group_writable */
    reset_env(8u, true);
    assert_eq(0, gic_spi_get_state(&g_gic_dev, 160u, &saved));
    assert_false(saved.group_writable);

    /* 3. Span OOB beyond nr_intids rejected */
    reset_env(5u, false); /* nr_intids = 160 */
    g_plat.desc.base_spi = 128u;
    g_plat.desc.offset = 0u;
    g_plat.desc.count = 8u; /* 32+128+8 = 168 > 160 */
    assert_true(bcm2712_mip_reserve(&g_plat, dummy_handler, 0u, &r1) < 0);

    /* 4. Fault injection: GIC ICFGR write dropped + MIP hmask restore fails -> quarantined */
    reset_env(8u, false);
    g_plat.desc.base_spi = 128u;
    g_plat.desc.offset = 0u;
    g_plat.desc.count = 2u;
    assert_eq(0, bcm2712_mip_reserve(&g_plat, dummy_handler, 1u, &r1));
    g_mip_regs[0x40u / 4u] = (1u << 1u); /* slot 1 was masked */
    g_drop_gic_icfgr_write = true;
    g_drop_mip_hmask_set = true;
    assert_true(bcm2712_mip_reserve(&g_plat, dummy_handler, 2u, &r2) < 0);
    assert_true(r2.quarantined);
    g_drop_gic_icfgr_write = false;
    g_drop_mip_hmask_set = false;
    /* Exhausted: no more slots */
    assert_true(bcm2712_mip_reserve(&g_plat, dummy_handler, 3u, &r2) < 0);
    assert_eq(0, bcm2712_mip_release(&r1, true));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_inbound_window_borrow_program_and_conflicts),
    TEST_ENTRY(test_mip_slots_and_gic_spi_state),
TEST_LIST_END

int main(void)
{
    TEST_SUITE("AArch64 BCM2712 MIP and inbound window");
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}
