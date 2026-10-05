/*
 * test_arena_table_base_shift.c — aarch64 M2/M3 plan Task 4.
 *
 * Asserts that the runtime page-table pool base has been shifted past
 * the slab pages (spec §3.2):
 *
 *   1. table_base_pa == slab_page_end_pa (the pool starts after the
 *      8 reserved 2 MiB slab pages, NOT at the PMM metadata end as in
 *      the pre-Task-3 layout).
 *   2. The runtime_tree builder, driven through injected fake pool
 *      ops whose PA cursor starts at arena.table_base_pa, consumes
 *      every page from inside [table_base_pa, table_end_pa) — i.e.
 *      the builder only depends on the base/end fields and stays
 *      correct across the base shift (zero-change verification).
 *
 * Links the REAL early_arena.c (aarch64_m1_plan) and the REAL
 * runtime_tree.c (aarch64_runtime_tree_build) against this host TU.
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
#include <arch/aarch64/runtime_tree.h>

/* ── Host stub surface (same pattern as test_arena_layout_chain.c) ── */
int g_log_level = 3;
void _log_err_impl(const char *fmt, ...) { (void)fmt; }
int color_printk(unsigned int FRcolor, unsigned int BKcolor,
                 const char *fmt, ...)
{
    (void)FRcolor; (void)BKcolor; (void)fmt;
    return 0;
}
size_t slab_init(void) { return 0; }

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

#define T_PAGE_2M UINT64_C(0x200000)
#define T_PAGE_4K UINT64_C(0x1000)

#define T_ARENA_BASE UINT64_C(0x40200000)
#define T_ARENA_SPAN (32ULL * 1024ULL * 1024ULL)   /* R = 32 MiB window */

/* Largest table pool this fixture plans for: 32 MiB of R plus the
 * fixed B and D windows → well under AARCH64_M1_TABLE_PAGES_MAX. */
#define T_POOL_MAX 16u

static void build_single(uint64_t base, uint64_t end,
                         struct MEMORY_RANGE out[1])
{
    out[0].phys_start = base;
    out[0].phys_end   = end;
    out[0].type       = MEMORY_TYPE_RAM;
}

static int plan_32m(struct aarch64_m1_arena *out)
{
    struct MEMORY_RANGE ram[1];
    build_single(T_ARENA_BASE, T_ARENA_BASE + T_ARENA_SPAN, ram);
    return aarch64_m1_plan(ram, 1, out);
}

/* ── Step 1: table_base_pa shifted past the slab pages ──────── */

TEST_FUNC(test_shift_table_base_equals_slab_page_end)
{
    struct aarch64_m1_arena a;
    int rc = plan_32m(&a);
    assert_eq(0, rc);
    /* The pool starts where the 8 reserved 2 MiB slab pages end —
     * NOT at the PMM metadata end (pre-Task-3 layout). */
    assert_eq(a.slab_page_end_pa, a.table_base_pa);
    /* slab_page_end_pa is 2 MiB aligned, so the base is too. */
    assert_true((a.table_base_pa & (T_PAGE_2M - 1u)) == 0);
    /* And the base really is past the PMM metadata end. */
    assert_true(a.table_base_pa > a.base_pa + a.layout.end_of_struct_off);
}

TEST_FUNC(test_shift_table_end_inside_arena)
{
    struct aarch64_m1_arena a;
    int rc = plan_32m(&a);
    assert_eq(0, rc);
    assert_true(a.table_end_pa > a.table_base_pa);
    assert_eq(a.table_base_pa + (uint64_t)a.table_pages * T_PAGE_4K,
              a.table_end_pa);
    assert_true(a.table_end_pa <= a.end_pa);
    assert_true(a.table_pages > 0u);
}

/* ── Fake pool ops: PA cursor starts at arena.table_base_pa ───
 *
 * Each issued PA gets one 4 KiB host buffer so the builder can zero
 * and write descriptors through the resolve()d VA — exactly the
 * contract the production pool (boot_direct_map.c) satisfies. */
typedef struct {
    uint64_t next_pa;
    uint64_t end_pa;                  /* pool ceiling */
    uint64_t first_pa;                /* first PA ever issued */
    int issued;
    uint64_t pages[T_POOL_MAX][T_PAGE_4K / sizeof(uint64_t)];
} shift_pool_t;

static int shift_alloc(void *ctx, uint64_t *pa, uint64_t **va)
{
    shift_pool_t *p = (shift_pool_t *)ctx;
    if (p->next_pa + T_PAGE_4K > p->end_pa) return -ENOMEM;
    if ((size_t)p->issued >= T_POOL_MAX) return -ENOMEM;
    *pa = p->next_pa;
    if (p->issued == 0) p->first_pa = *pa;
    *va = p->pages[p->issued];
    p->next_pa += T_PAGE_4K;
    ++p->issued;
    return 0;
}

static uint64_t *shift_resolve(void *ctx, uint64_t pa)
{
    shift_pool_t *p = (shift_pool_t *)ctx;
    int i;
    if (pa < p->first_pa || pa >= p->next_pa) return NULL;
    if ((pa & (T_PAGE_4K - 1u)) != 0u) return NULL;
    i = (int)((pa - p->first_pa) / T_PAGE_4K);
    if (i < 0 || i >= T_POOL_MAX) return NULL;
    return p->pages[i];
}

/* ── Step 2: builder consumes pages from the shifted window ─── */

TEST_FUNC(test_shift_builder_consumes_table_window)
{
    struct aarch64_m1_arena a;
    struct MEMORY_RANGE ram[1];
    struct aarch64_runtime_tree tree;
    struct aarch64_tree_ops ops;
    shift_pool_t pool;
    int rc = plan_32m(&a);
    assert_eq(0, rc);

    /* The fake pool draws from the arena's table window only. */
    memset(&pool, 0, sizeof(pool));
    pool.next_pa = a.table_base_pa;
    pool.end_pa  = a.table_end_pa;

    memset(&ops, 0, sizeof(ops));
    ops.ctx = &pool;
    ops.alloc = shift_alloc;
    ops.resolve = shift_resolve;

    build_single(T_ARENA_BASE, T_ARENA_BASE + T_ARENA_SPAN, ram);
    rc = aarch64_runtime_tree_build(ram, 1, &a, &ops, &tree);
    assert_eq(0, rc);

    /* Every issued PA lies inside [table_base_pa, table_end_pa). */
    assert_true(pool.first_pa >= a.table_base_pa);
    assert_true(pool.first_pa < a.table_end_pa);
    assert_true(pool.next_pa <= a.table_end_pa);

    /* The published tree metadata matches the consumed window. */
    assert_eq(pool.first_pa, tree.root_pa);
    assert_eq(pool.first_pa, tree.table_base_pa);
    assert_eq(pool.next_pa, tree.table_used_end_pa);
    assert_eq((size_t)(pool.next_pa - pool.first_pa) / 4096u,
              tree.page_count);
    assert_true(tree.page_count <= a.table_pages);
}

TEST_FUNC(test_shift_builder_exhausts_at_table_end)
{
    struct aarch64_m1_arena a;
    struct MEMORY_RANGE ram[1];
    struct aarch64_runtime_tree tree;
    struct aarch64_tree_ops ops;
    shift_pool_t pool;
    int rc = plan_32m(&a);
    assert_eq(0, rc);

    /* Cap the pool one page short of what the tree needs: the builder
     * must report -ENOMEM instead of spilling past table_end_pa. */
    memset(&pool, 0, sizeof(pool));
    pool.next_pa = a.table_base_pa;
    pool.end_pa  = a.table_end_pa - T_PAGE_4K;

    memset(&ops, 0, sizeof(ops));
    ops.ctx = &pool;
    ops.alloc = shift_alloc;
    ops.resolve = shift_resolve;

    build_single(T_ARENA_BASE, T_ARENA_BASE + T_ARENA_SPAN, ram);
    rc = aarch64_runtime_tree_build(ram, 1, &a, &ops, &tree);
    assert_eq(-ENOMEM, rc);
    /* No page was ever issued past the real pool ceiling. */
    assert_true(pool.next_pa <= a.table_end_pa);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_shift_table_base_equals_slab_page_end),
    TEST_ENTRY(test_shift_table_end_inside_arena),
    TEST_ENTRY(test_shift_builder_consumes_table_window),
    TEST_ENTRY(test_shift_builder_exhausts_at_table_end),
TEST_LIST_END

int main(void)
{
    int failed = M1_RUN_ALL_TESTS();
    return failed;
}
