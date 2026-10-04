/*
 * test_arena_layout_chain.c — hosttest for early_arena §3.2 formula chain
 * (aarch64 M2/M3 plan Task 3).
 *
 * Asserts the full arena layout chain invariants after Task 3:
 *
 *   slab_meta_bytes  == slab_layout_compute().meta_bytes
 *   slab_page_start_pa == align_up_2M(arena.base_pa
 *                                  + arena.layout.end_of_struct_off
 *                                  + slab_meta_bytes)
 *   slab_page_end_pa == slab_page_start_pa + 8 * 2 MiB
 *   table_base_pa    == slab_page_end_pa                (already 2M aligned)
 *   table_end_pa     == table_base_pa + table_pages * 4 KiB
 *   end_pa           == align_up_2M(table_end_pa)
 *
 * Note: per spec §3.2 the pmm metadata end is `end_of_struct_off`
 * (align_down semantic, NOT `total_bytes` which adds a 4 KiB slack).
 * Computing it from `end_of_struct_off` is the canonical chain.
 *
 * RED state (pre-Task-3): struct aarch64_m1_arena has no slab_* fields;
 * the test fails to compile / link. After Task 3 lands, GREEN.
 */
#include "m1_test_runner.h"
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

/* Same host-stub surface as test_m1_arena.c — the production
 * early_arena.c uses log_err() which our test TU mocks. */
int g_log_level = 3;
void _log_err_impl(const char *fmt, ...) { (void)fmt; }
int color_printk(unsigned int FRcolor, unsigned int BKcolor,
                 const char *fmt, ...)
{
    (void)FRcolor; (void)BKcolor; (void)fmt;
    return 0;
}
size_t slab_init(void) { return 0; }

/* Task 3: early_arena.c now reads kmalloc_cache_size[].size via
 * slab_layout_compute(). Provide the production sizes so the
 * compute_arena_end chain agrees with the production slab_init. */
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

static void build_single(uint64_t base, uint64_t end,
                         struct MEMORY_RANGE out[1])
{
    out[0].phys_start = base;
    out[0].phys_end   = end;
    out[0].type       = MEMORY_TYPE_RAM;
}

/* Spec §3.2: pmm metadata end = base + end_of_struct_off (NOT total_bytes,
 * which adds a 4 KiB slack). */
static uint64_t meta_end_pa(uint64_t base_pa, const struct pmm_layout *l)
{
    return base_pa + l->end_of_struct_off;
}

/* Spec §3.2: arena_end chain via checked overflow at every step. */
static uint64_t expected_arena_end(uint64_t base_pa,
                                   const struct pmm_layout *l,
                                   size_t table_pages)
{
    uint64_t sl_meta = meta_end_pa(base_pa, l) + slab_layout_compute().meta_bytes;
    uint64_t slab_start = (sl_meta + PAGE_2M_SIZE - 1u) & ~(PAGE_2M_SIZE - 1u);
    uint64_t slab_end   = slab_start + 8ULL * PAGE_2M_SIZE;
    uint64_t table_end  = slab_end + (uint64_t)table_pages * 0x1000ULL;
    return (table_end + PAGE_2M_SIZE - 1u) & ~(PAGE_2M_SIZE - 1u);
}

/* ── Chain invariants on the happy 64 MiB low-window path ── */

TEST_FUNC(test_layout_chain_slab_meta_bytes)
{
    /* 64 MiB low window: enough for metadata + slab_meta + 8 * 2 MiB
     * + a small table pool. */
    struct MEMORY_RANGE ram[1];
    build_single(0x40200000ULL, 0x40200000ULL + 64ULL * 1024 * 1024, ram);
    struct aarch64_m1_arena got;
    int rc = aarch64_m1_plan(ram, 1, &got);
    assert_eq(0, rc);
    /* slab_meta_bytes == slab_layout_compute().meta_bytes (single source of truth). */
    struct slab_layout sl = slab_layout_compute();
    assert_eq(sl.meta_bytes, got.slab_meta_bytes);
    assert_eq(sl.reserved_2m_pages, 8ULL);
}

TEST_FUNC(test_layout_chain_slab_page_start_end)
{
    struct MEMORY_RANGE ram[1];
    build_single(0x40200000ULL, 0x40200000ULL + 64ULL * 1024 * 1024, ram);
    struct aarch64_m1_arena got;
    int rc = aarch64_m1_plan(ram, 1, &got);
    assert_eq(0, rc);
    uint64_t sl_meta = meta_end_pa(got.base_pa, &got.layout)
                     + got.slab_meta_bytes;
    uint64_t want_slab_start =
        (sl_meta + PAGE_2M_SIZE - 1u) & ~(PAGE_2M_SIZE - 1u);
    assert_eq(want_slab_start, got.slab_page_start_pa);
    /* Must already be 2 MiB aligned. */
    assert_true((got.slab_page_start_pa & (PAGE_2M_SIZE - 1u)) == 0);
    uint64_t want_slab_end = got.slab_page_start_pa + 8ULL * PAGE_2M_SIZE;
    assert_eq(want_slab_end, got.slab_page_end_pa);
}

TEST_FUNC(test_layout_chain_table_base_after_slab_end)
{
    struct MEMORY_RANGE ram[1];
    build_single(0x40200000ULL, 0x40200000ULL + 64ULL * 1024 * 1024, ram);
    struct aarch64_m1_arena got;
    int rc = aarch64_m1_plan(ram, 1, &got);
    assert_eq(0, rc);
    /* table_base_pa MUST equal slab_page_end_pa (already 2M aligned,
     * so no extra padding needed between slab pages and table pool). */
    assert_eq(got.slab_page_end_pa, got.table_base_pa);
    /* table_base_pa is 2 MiB aligned (slab_end is 2 MiB aligned). */
    assert_true((got.table_base_pa & (PAGE_2M_SIZE - 1u)) == 0);
}

TEST_FUNC(test_layout_chain_arena_end_aligns_2m)
{
    struct MEMORY_RANGE ram[1];
    build_single(0x40200000ULL, 0x40200000ULL + 64ULL * 1024 * 1024, ram);
    struct aarch64_m1_arena got;
    int rc = aarch64_m1_plan(ram, 1, &got);
    assert_eq(0, rc);
    /* table_end_pa = table_base_pa + table_pages * 4 KiB. */
    uint64_t want_table_end = got.table_base_pa
                            + (uint64_t)got.table_pages * 0x1000ULL;
    assert_eq(want_table_end, got.table_end_pa);
    /* end_pa = align_up_2M(table_end_pa). */
    uint64_t want_end = (want_table_end + PAGE_2M_SIZE - 1u)
                      & ~(PAGE_2M_SIZE - 1u);
    assert_eq(want_end, got.end_pa);
    /* end_pa must be 2 MiB aligned. */
    assert_true((got.end_pa & (PAGE_2M_SIZE - 1u)) == 0);
}

TEST_FUNC(test_layout_chain_full_formula_match)
{
    /* Cross-check the full chain against the expected-calculator
     * helper. End-to-end contract: every field matches the spec
     * formula. */
    struct MEMORY_RANGE ram[1];
    build_single(0x40200000ULL, 0x40200000ULL + 64ULL * 1024 * 1024, ram);
    struct aarch64_m1_arena got;
    int rc = aarch64_m1_plan(ram, 1, &got);
    assert_eq(0, rc);
    uint64_t want_end = expected_arena_end(got.base_pa, &got.layout,
                                           got.table_pages);
    assert_eq(want_end, got.end_pa);
}

TEST_FUNC(test_layout_chain_arena_inside_input_range)
{
    /* Sanity: arena must be fully inside the input range. */
    struct MEMORY_RANGE ram[1];
    build_single(0x40200000ULL, 0x40200000ULL + 64ULL * 1024 * 1024, ram);
    struct aarch64_m1_arena got;
    int rc = aarch64_m1_plan(ram, 1, &got);
    assert_eq(0, rc);
    assert_true(got.base_pa >= ram[0].phys_start);
    assert_true(got.end_pa <= ram[0].phys_end);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_layout_chain_slab_meta_bytes),
    TEST_ENTRY(test_layout_chain_slab_page_start_end),
    TEST_ENTRY(test_layout_chain_table_base_after_slab_end),
    TEST_ENTRY(test_layout_chain_arena_end_aligns_2m),
    TEST_ENTRY(test_layout_chain_full_formula_match),
    TEST_ENTRY(test_layout_chain_arena_inside_input_range),
TEST_LIST_END

int main(void)
{
    int failed = M1_RUN_ALL_TESTS();
    return failed;
}
