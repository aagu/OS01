/* @os01-test
 * id: test_aarch64_handoff_cache
 * layer: host
 * subsystem: boot
 * timing: strict
 * tier: pr
 * link: aarch64-uefi-boot-memory
 * scenarios: [boot.uefi.aarch64_handoff_cache]
 */
/*
 * Host unit test for boot/uefi/arch/aarch64/cache_handoff.h
 * (flush_handoff_caches() path in boot/uefi/arch/aarch64/boot.c).
 * Verifies CTR_EL0.DminLine decoding, unaligned range rounding, full
 * coverage of all handoff regions, skipping of absent optional regions,
 * and exact post-clean barrier ordering (dsb sy -> ic iallu -> dsb sy -> isb).
 */

#include "test_framework.h"
#include "../../boot/uefi/arch/aarch64/cache_handoff.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum cache_op_kind {
    OP_DC_CIVAC = 1,
    OP_DSB_SY   = 2,
    OP_IC_IALLU = 3,
    OP_ISB      = 4,
};

#define MAX_RECORDED_OPS 2048

struct cache_trace {
    uint64_t ctr_val;
    uint32_t dc_count;
    uint32_t total_ops;
    uint64_t dc_addrs[MAX_RECORDED_OPS];
    enum cache_op_kind seq[MAX_RECORDED_OPS];
    bool overflow;
};

static uint64_t mock_read_ctr(void *ctx)
{
    struct cache_trace *t = (struct cache_trace *)ctx;
    return t->ctr_val;
}

static void record_op(struct cache_trace *t, enum cache_op_kind k)
{
    if (t->total_ops < MAX_RECORDED_OPS)
        t->seq[t->total_ops++] = k;
    else
        t->overflow = true;
}

static void mock_dc_civac(void *ctx, uint64_t addr)
{
    struct cache_trace *t = (struct cache_trace *)ctx;
    if (t->dc_count < MAX_RECORDED_OPS)
        t->dc_addrs[t->dc_count++] = addr;
    else
        t->overflow = true;
    record_op(t, OP_DC_CIVAC);
}

static void mock_dsb_sy(void *ctx)
{
    record_op((struct cache_trace *)ctx, OP_DSB_SY);
}

static void mock_ic_iallu(void *ctx)
{
    record_op((struct cache_trace *)ctx, OP_IC_IALLU);
}

static void mock_isb(void *ctx)
{
    record_op((struct cache_trace *)ctx, OP_ISB);
}

static const struct aarch64_cache_ops g_mock_ops = {
    .read_ctr = mock_read_ctr,
    .dc_civac = mock_dc_civac,
    .dsb_sy   = mock_dsb_sy,
    .ic_iallu = mock_ic_iallu,
    .isb      = mock_isb,
};

static bool range_covered(const struct cache_trace *t, uint64_t base,
                          uint64_t size, uint32_t line)
{
    uint64_t start = base & ~(uint64_t)(line - 1u);
    uint64_t end = (base + size + line - 1u) & ~(uint64_t)(line - 1u);
    for (uint64_t a = start; a < end; a += line) {
        bool found = false;
        for (uint32_t i = 0; i < t->dc_count; i++) {
            if (t->dc_addrs[i] == a) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

static void test_ctr_dminline_decode(void)
{
    assert_eq(4u,   aarch64_ctr_dminline_bytes(UINT64_C(0) << 16));
    assert_eq(32u,  aarch64_ctr_dminline_bytes(UINT64_C(3) << 16));
    assert_eq(64u,  aarch64_ctr_dminline_bytes(UINT64_C(4) << 16));
    assert_eq(128u, aarch64_ctr_dminline_bytes(UINT64_C(5) << 16));
}

static void test_unaligned_range_rounding(void)
{
    struct cache_trace t;
    memset(&t, 0, sizeof(t));
    /* [0x1003, 0x1083) with 64-byte line must clean 0x1000, 0x1040, 0x1080. */
    aarch64_clean_invalidate_dcache_range(0x1003ULL, 128ULL, 64u, &g_mock_ops, &t);
    assert_false(t.overflow);
    assert_eq(3u, t.dc_count);
    assert_eq(0x1000ULL, t.dc_addrs[0]);
    assert_eq(0x1040ULL, t.dc_addrs[1]);
    assert_eq(0x1080ULL, t.dc_addrs[2]);
}

static void test_flush_all_handoff_regions_and_barriers(void)
{
    struct cache_trace t;
    memset(&t, 0, sizeof(t));
    t.ctr_val = UINT64_C(4) << 16; /* 64-byte line (RPi5 Cortex-A76) */

    const struct aarch64_handoff_cache_regions regions = {
        .boot_block_base = UINT64_C(0x40080000),
        .boot_block_size = UINT64_C(0x4000),     /* 16 KiB */
        .trampoline_base = UINT64_C(0x37f40000),
        .page_size       = UINT64_C(0x1000),
        .ctx_phys        = UINT64_C(0x37f43000),
        .fdt_phys        = UINT64_C(0x37f50000),
        .fdt_pages       = 2u,
        .mmap_phys       = UINT64_C(0x37f60000),
        .mmap_pages      = 1u,
    };

    aarch64_flush_handoff_caches(&regions, &g_mock_ops, &t);
    assert_false(t.overflow);

    /* Expected total lines: (16K + 4K + 4K + 8K + 4K) / 64 = 36K / 64 = 576 */
    assert_eq(576u, t.dc_count);
    assert_true(range_covered(&t, regions.boot_block_base, regions.boot_block_size, 64u));
    assert_true(range_covered(&t, regions.trampoline_base, regions.page_size, 64u));
    assert_true(range_covered(&t, regions.ctx_phys, regions.page_size, 64u));
    assert_true(range_covered(&t, regions.fdt_phys, regions.fdt_pages * regions.page_size, 64u));
    assert_true(range_covered(&t, regions.mmap_phys, regions.mmap_pages * regions.page_size, 64u));

    /* All dc_civac ops must precede the 4-op barrier sequence:
     * dsb sy -> ic iallu -> dsb sy -> isb */
    assert_eq(t.dc_count + 4u, t.total_ops);
    for (uint32_t i = 0; i < t.dc_count; i++)
        assert_eq(OP_DC_CIVAC, t.seq[i]);
    assert_eq(OP_DSB_SY,   t.seq[t.dc_count + 0u]);
    assert_eq(OP_IC_IALLU, t.seq[t.dc_count + 1u]);
    assert_eq(OP_DSB_SY,   t.seq[t.dc_count + 2u]);
    assert_eq(OP_ISB,      t.seq[t.dc_count + 3u]);
}

static void test_skip_absent_optional_regions(void)
{
    struct cache_trace t;
    memset(&t, 0, sizeof(t));
    t.ctr_val = UINT64_C(4) << 16; /* 64-byte line */

    const struct aarch64_handoff_cache_regions regions = {
        .boot_block_base = UINT64_C(0x40080000),
        .boot_block_size = UINT64_C(0x1000),
        .trampoline_base = UINT64_C(0x37f40000),
        .page_size       = UINT64_C(0x1000),
        .ctx_phys        = 0u,
        .fdt_phys        = UINT64_C(0x37f50000),
        .fdt_pages       = 0u,
        .mmap_phys       = 0u,
        .mmap_pages      = 3u,
    };

    aarch64_flush_handoff_caches(&regions, &g_mock_ops, &t);
    assert_false(t.overflow);
    /* Only boot_block (4K) + trampoline (4K) = 8K / 64 = 128 lines */
    assert_eq(128u, t.dc_count);
    assert_eq(132u, t.total_ops);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_ctr_dminline_decode),
    TEST_ENTRY(test_unaligned_range_rounding),
    TEST_ENTRY(test_flush_all_handoff_regions_and_barriers),
    TEST_ENTRY(test_skip_absent_optional_regions),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return (__test_stats.failed == 0) ? 0 : 1;
}
