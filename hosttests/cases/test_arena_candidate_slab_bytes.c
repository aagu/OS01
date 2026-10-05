/*
 * test_arena_candidate_slab_bytes.c — hosttest for candidate scan
 * respecting the slab segment (aarch64 M2/M3 plan Task 3).
 *
 * v3 review item 14 fix: use LEGALLY-aligned ranges (both endpoints
 * 2 MiB aligned) so the input validator accepts them; the first
 * range is intentionally too small to hold metadata + slab_meta +
 * 8 * 2 MiB + table_pages * 4 KiB. The current implementation only
 * accounts for metadata + table_pages, so it accepts the 2 MiB first
 * range. After Task 3 lands the candidate scan uses the full formula
 * (slab_meta + 8 * 2 MiB + alignment slack) and selects the second
 * range.
 *
 * Inputs:
 *   range 0 = [0x40200000, 0x40400000)  (2 MiB, too small)
 *   range 1 = [0x50000000, 0x60000000)  (256 MiB, sufficient)
 *
 * RED state (pre-Task-3): planner picks range 0 (the first one whose
 * "metadata + tables" bytes fit in 2 MiB). After Task 3: planner
 * rejects range 0 (slab segment + alignment slack overflow) and
 * picks range 1.
 */
#include "page_table_test_runner.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include <memory/memory_map.h>
#include <memory/pmm.h>
#include <memory/pmm_boot.h>
#include <memory/slab.h>
#include <arch/aarch64/early_arena.h>
#include <arch/mmu.h>

int g_log_level = 3;
void _log_err_impl(const char *fmt, ...) { (void)fmt; }
int color_printk(unsigned int FRcolor, unsigned int BKcolor,
                 const char *fmt, ...)
{
    (void)FRcolor; (void)BKcolor; (void)fmt;
    return 0;
}
size_t slab_init(void) { return 0; }

/* Task 3: early_arena.c reads kmalloc_cache_size[].size via
 * slab_layout_compute(). Same production-size stub as
 * test_arena_layout_chain.c. */
struct Slab_Cache kmalloc_cache_size[16] = {
    {32,      0, 0, NULL, NULL, NULL, NULL},
    {64,      0, 0, NULL, NULL, NULL, NULL},
    {128,     0, 0, NULL, NULL, NULL, NULL},
    {256,     0, 0, NULL, NULL, NULL, NULL},
    {512,     0, 0, NULL, NULL, NULL, NULL},
    {1024,    0, 0, NULL, NULL, NULL, NULL},
    {2048,    0, 0, NULL, NULL, NULL, NULL},
    {4096,    0, 0, NULL, NULL, NULL, NULL},
    {8192,    0, 0, NULL, NULL, NULL, NULL},
    {16384,   0, 0, NULL, NULL, NULL, NULL},
    {32768,   0, 0, NULL, NULL, NULL, NULL},
    {65536,   0, 0, NULL, NULL, NULL, NULL},
    {131072,  0, 0, NULL, NULL, NULL, NULL},
    {262144,  0, 0, NULL, NULL, NULL, NULL},
    {524288,  0, 0, NULL, NULL, NULL, NULL},
    {1048576, 0, 0, NULL, NULL, NULL, NULL},
};

/* ── Candidate filter must include slab segment bytes ───────── */

TEST_FUNC(test_candidate_scan_includes_slab_bytes)
{
    /* Two ranges, both 2 MiB-aligned. Range 0 is intentionally too
     * small for the full arena; range 1 is more than sufficient. */
    struct MEMORY_RANGE ram[2] = {
        { .phys_start = 0x40200000ULL, .phys_end = 0x40400000ULL,
          .type = MEMORY_TYPE_RAM },
        { .phys_start = 0x50000000ULL, .phys_end = 0x60000000ULL,
          .type = MEMORY_TYPE_RAM },
    };
    struct aarch64_early_arena got;
    int rc = aarch64_early_arena_plan(ram, 2, &got);
    assert_eq(0, rc);
    /* The candidate MUST be inside range 1, not range 0. */
    assert_true(got.base_pa >= 0x50000000ULL);
    assert_true(got.end_pa   <= 0x60000000ULL);
    /* And specifically NOT inside range 0. */
    assert_true(got.end_pa > 0x40400000ULL || got.base_pa >= 0x50000000ULL);
}

TEST_FUNC(test_first_range_2mib_rejected)
{
    /* Negative control: a single 2 MiB range in the LOW window is
     * rejected by the candidate filter (slab segment alone needs
     * 16 MiB before table pages). The failure leaves PMMngr.start_brk
     * untouched via prepare(). */
    extern struct Physical_Memory_Manager PMMngr;
    const uint64_t canary = UINT64_C(0xcafebabecafebabe);
    PMMngr.start_brk = canary;
    struct MEMORY_RANGE ram[1] = {
        { .phys_start = 0x40200000ULL, .phys_end = 0x40400000ULL,
          .type = MEMORY_TYPE_RAM },
    };
    int rc = aarch64_early_arena_prepare(ram, 1);
    assert_true(rc < 0);
    assert_eq(canary, PMMngr.start_brk);
}

TEST_FUNC(test_candidate_with_full_room_succeeds)
{
    /* Sanity check: when the first range IS large enough, the planner
     * picks it (not a later range). This guards against a
     * regression where the new candidate filter accidentally rejects
     * the first legal candidate. */
    struct MEMORY_RANGE ram[2] = {
        { .phys_start = 0x40200000ULL, .phys_end = 0x60000000ULL,
          .type = MEMORY_TYPE_RAM },
        { .phys_start = 0x70000000ULL, .phys_end = 0x80000000ULL,
          .type = MEMORY_TYPE_RAM },
    };
    struct aarch64_early_arena got;
    int rc = aarch64_early_arena_plan(ram, 2, &got);
    assert_eq(0, rc);
    assert_true(got.base_pa >= 0x40200000ULL);
    assert_true(got.end_pa   <= 0x60000000ULL);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_candidate_scan_includes_slab_bytes),
    TEST_ENTRY(test_first_range_2mib_rejected),
    TEST_ENTRY(test_candidate_with_full_room_succeeds),
TEST_LIST_END

int main(void)
{
    int failed = PAGE_TABLE_RUN_ALL_TESTS();
    return failed;
}
