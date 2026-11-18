/* @os01-test
 * id: test_aarch64_boot_layout
 * layer: host
 * subsystem: boot
 * timing: strict
 * tier: nightly
 * link: aarch64-boot-layout
 * scenarios: [boot.lma.reserve_ram]
 */
/* AArch64 boot-block ownership through the M1 pipeline (LMA Task 4).
 *
 * The production early_arena.c, runtime_tree.c and ram_core.c are
 * compiled from the same sources with the relocated layout
 * (AARCH64_BOOT_BLOCK_BASE=0x02000000). Every expected address in this
 * file is an independent literal — deriving them from the production
 * headers would make a stale default satisfy the test.
 *
 * Fixtures (relocated B = [0x02000000, 0x02200000)):
 *   R = [0x01000000,0x02000000) + [0x02200000,0x06200000)
 *       + [0x40000000,0x44000000)  (the third range CONTAINS the old
 *       fixed default block — it must map as plain RAM NX).
 *   - aarch64_ram_normalize_boot on a single EFI RAM descriptor
 *     spanning B excludes the whole block and keeps both sides;
 *   - the arena plan starts at 0x02200000 (B.end), never below;
 *   - M1 build+validate succeed; the built tree maps B Normal
 *     executable (UXN set, PXN clear), the low R and the old B as
 *     plain Normal NX and D as Device NX;
 *   - an R covering B is rejected by both the arena planner and the
 *     tree builder.
 */
#include "test_framework.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <core/bootinfo.h>
#include <memory/memory_map.h>
#include <memory/slab.h>
#include <arch/aarch64/ram_core.h>
#include <arch/aarch64/early_arena.h>
#include <arch/aarch64/runtime_tree.h>
#include <arch/aarch64/early_mmu.h>

/* ── Independent expected literals (relocated layout) ─────────── */
#define FIX_B_BASE   UINT64_C(0x02000000)
#define FIX_B_END    UINT64_C(0x02200000)
#define FIX_D_BASE   UINT64_C(0x08000000)
#define FIX_OLD_B    UINT64_C(0x40000000)
#define FIX_ARENA_BASE UINT64_C(0x02200000)

/* Raw descriptor shapes produced by runtime_tree.c (mirrored from the
 * AArch64 block descriptor contract; asserted, not assumed). */
#define DESC_VALID   UINT64_C(0x001)
#define DESC_TABLE   UINT64_C(0x002)
#define DESC_L2_PA_MASK UINT64_C(0x000000ffffe00000)
#define DESC_L_PA_MASK  UINT64_C(0x000000fffffff000)
#define DESC_UXN     UINT64_C(0x40000000000000)
#define DESC_PXN     UINT64_C(0x20000000000000)
#define DESC_NORMAL_BLOCK UINT64_C(0x705)
#define DESC_DEVICE_BLOCK UINT64_C(0x401)

/* ── Production-symbol stubs (host backend, same shape as
 * test_page_table_tree.c) ─────────────────────────────────────── */
int g_log_level = 3;
void _log_err_impl(const char *fmt, ...) { (void)fmt; }
int color_printk(unsigned int FRcolor, unsigned int BKcolor,
                 const char *fmt, ...)
{
    (void)FRcolor; (void)BKcolor; (void)fmt;
    return 0;
}
size_t slab_init(void) { return 0; }
/* Production sizes so the arena formula chain agrees with the
 * production slab_init (same table as test_arena_layout_chain.c). */
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

/* ── Fixture RAM map ──────────────────────────────────────────── */
#define FIX_R_COUNT 3

static size_t fill_ranges(struct MEMORY_RANGE *r)
{
    memset(r, 0, sizeof(*r) * FIX_R_COUNT);
    r[0].type = MEMORY_TYPE_RAM;
    r[0].phys_start = UINT64_C(0x01000000);
    r[0].phys_end = UINT64_C(0x02000000);
    r[1].type = MEMORY_TYPE_RAM;
    r[1].phys_start = UINT64_C(0x02200000);
    r[1].phys_end = UINT64_C(0x06200000);
    r[2].type = MEMORY_TYPE_RAM;
    r[2].phys_start = UINT64_C(0x40000000);
    r[2].phys_end = UINT64_C(0x44000000);
    return FIX_R_COUNT;
}

/* ── Fake table pool (test_page_table_tree.c host backend) ────── */
#define POOL_MAX_PAGES 1100u

struct fake_page {
    uint64_t pa;
    uint64_t *va;
};

struct fake_pool {
    uint64_t next_pa;
    struct fake_page pages[POOL_MAX_PAGES];
    size_t count;
};

static int pool_alloc(void *ctx, uint64_t *pa, uint64_t **va)
{
    struct fake_pool *p = (struct fake_pool *)ctx;

    if (p->count >= POOL_MAX_PAGES)
        return -ENOMEM;
    p->pages[p->count].pa = p->next_pa;
    p->pages[p->count].va = (uint64_t *)calloc(1u, 4096u);
    if (!p->pages[p->count].va)
        return -ENOMEM;
    *pa = p->next_pa;
    *va = p->pages[p->count].va;
    p->next_pa += 0x1000;
    ++p->count;
    return 0;
}

static uint64_t *pool_resolve(void *ctx, uint64_t pa)
{
    struct fake_pool *p = (struct fake_pool *)ctx;
    size_t i;

    for (i = 0; i < p->count; ++i)
        if (p->pages[i].pa == pa)
            return p->pages[i].va;
    return NULL;
}

static void pool_free_all(struct fake_pool *p)
{
    size_t i;

    for (i = 0; i < p->count; ++i)
        free(p->pages[i].va);
    p->count = 0;
    p->next_pa = 0;
}

static const struct aarch64_tree_ops *pool_ops(struct fake_pool *p)
{
    static struct aarch64_tree_ops ops;

    ops.ctx = p;
    ops.alloc = pool_alloc;
    ops.resolve = pool_resolve;
    return &ops;
}

/* ── Descriptor walk helpers ──────────────────────────────────── */
static uint64_t table_child(uint64_t *table, unsigned slot)
{
    uint64_t d = table[slot];

    assert_true((d & (DESC_VALID | DESC_TABLE)) == (DESC_VALID | DESC_TABLE));
    return d & DESC_L_PA_MASK;
}

static uint64_t block_desc(uint64_t *table, unsigned slot)
{
    uint64_t d = table[slot];

    assert_true((d & (DESC_VALID | DESC_TABLE)) == DESC_VALID);
    return d;
}

static void test_ram_normalize_boot_excludes_b(void)
{
    uint8_t bytes[64];
    struct aarch64_ram_map map;
    uint64_t start = UINT64_C(0x01000000);
    uint64_t end = UINT64_C(0x06200000);
    uint64_t pages = (end - start) / 4096u;
    int rc;

    memset(bytes, 0, sizeof(bytes));
    bytes[0] = 7;                       /* EfiConventionalMemory */
    for (int i = 0; i < 8; i++) {
        bytes[8 + i] = (uint8_t)(start >> (8 * i));
        bytes[24 + i] = (uint8_t)(pages >> (8 * i));
    }

    rc = aarch64_ram_normalize_boot(bytes, 1u, 48u, BOOT_MEMORY_FORMAT_UEFI_RAW,
                                    1u, &map);
    assert_eq(0, rc);
    assert_eq(2u, map.count);
    assert_eq(UINT64_C(0x01000000), map.ranges[0].start);
    assert_eq(FIX_B_BASE, map.ranges[0].end);
    assert_eq(FIX_B_END, map.ranges[1].start);
    assert_eq(UINT64_C(0x06200000), map.ranges[1].end);
}

static void test_arena_plan_starts_after_b(void)
{
    struct MEMORY_RANGE r[FIX_R_COUNT];
    struct aarch64_early_arena arena;

    fill_ranges(r);
    assert_eq(0, aarch64_early_arena_plan(r, FIX_R_COUNT, &arena));
    assert_eq(FIX_ARENA_BASE, arena.base_pa);
}

static void test_tree_build_validate_maps_b_exec(void)
{
    struct MEMORY_RANGE r[FIX_R_COUNT];
    struct aarch64_early_arena arena;
    struct fake_pool pool;
    struct aarch64_runtime_tree tree;
    struct aarch64_runtime_tree_validate_buf vbuf;
    uint64_t *root, *pud, *pmd_low, *pmd_high;
    uint64_t d;

    fill_ranges(r);
    assert_eq(0, aarch64_early_arena_plan(r, FIX_R_COUNT, &arena));
    assert_eq(FIX_ARENA_BASE, arena.base_pa);

    memset(&pool, 0, sizeof(pool));
    pool.next_pa = arena.table_base_pa;

    assert_eq(0, aarch64_runtime_tree_build(r, FIX_R_COUNT, &arena,
                                           pool_ops(&pool), &tree));
    assert_eq(0, aarch64_runtime_tree_validate(r, FIX_R_COUNT, &arena,
                                               pool_ops(&pool), &vbuf, &tree));

    /* Walk the actually built tree: root -> PUD -> the two PMDs. */
    root = pool_resolve(&pool, tree.root_pa);
    assert_true(root != NULL);
    pud = pool_resolve(&pool, table_child(root, 0));
    assert_true(pud != NULL);
    pmd_low = pool_resolve(&pool, table_child(pud, 0));
    pmd_high = pool_resolve(&pool, table_child(pud, 1));
    assert_true(pmd_low != NULL && pmd_high != NULL);

    /* New B: Normal executable at EL1 (UXN set, PXN clear). */
    d = block_desc(pmd_low, (unsigned)((FIX_B_BASE >> 21) & 511u));
    assert_eq(DESC_NORMAL_BLOCK | FIX_B_BASE | DESC_UXN, d);
    assert_eq(0u, (unsigned)((d & DESC_PXN) != 0));

    /* D: Device NX at the first D slot. */
    d = block_desc(pmd_low, 0x40u);
    assert_eq(DESC_DEVICE_BLOCK | FIX_D_BASE | DESC_PXN | DESC_UXN, d);

    /* Low R block: plain Normal NX. */
    d = block_desc(pmd_low, (unsigned)((UINT64_C(0x02200000) >> 21) & 511u));
    assert_eq(DESC_NORMAL_BLOCK | UINT64_C(0x02200000) | DESC_PXN | DESC_UXN, d);

    /* Old fixed default block: inside R, plain Normal NX. */
    d = block_desc(pmd_high, (unsigned)((FIX_OLD_B >> 21) & 511u));
    assert_eq(DESC_NORMAL_BLOCK | FIX_OLD_B | DESC_PXN | DESC_UXN, d);

    pool_free_all(&pool);
}

static void test_r_covering_b_rejected(void)
{
    struct MEMORY_RANGE r[1];
    struct aarch64_early_arena arena;
    struct fake_pool pool;
    struct aarch64_runtime_tree tree;

    memset(r, 0, sizeof(r));
    r[0].type = MEMORY_TYPE_RAM;
    r[0].phys_start = UINT64_C(0x01000000);
    r[0].phys_end = UINT64_C(0x02400000);

    assert_true(aarch64_early_arena_plan(r, 1, &arena) != 0);

    memset(&pool, 0, sizeof(pool));
    pool.next_pa = FIX_B_END + UINT64_C(0x1000);
    {
        struct aarch64_early_arena hand;
        memset(&hand, 0, sizeof(hand));
        hand.base_pa = FIX_B_END;
        hand.end_pa = FIX_B_END + UINT64_C(0x400000);
        hand.table_base_pa = pool.next_pa;
        hand.table_pages = 1027u;
        assert_true(aarch64_runtime_tree_build(r, 1, &hand, pool_ops(&pool),
                                               &tree) != 0);
    }
    pool_free_all(&pool);
}

static void test_bootstrap_table_descriptors(void)
{
    static _Alignas(4096) uint64_t tables[6][512];
    static const uint64_t boot_bases[] = {0x40000000, 0x42000000, 0};

    for (size_t layout = 0; layout < sizeof(boot_bases) / sizeof(boot_bases[0]);
         ++layout) {
        uint64_t boot_base = boot_bases[layout];
        memset(tables, 0, sizeof(tables));
        aarch64_build_boot_page_tables(tables, boot_base);

        assert_eq((uintptr_t)tables[1] | DESC_VALID | DESC_TABLE, tables[0][0]);
        assert_eq((uintptr_t)tables[2] | DESC_VALID | DESC_TABLE, tables[1][0]);
        assert_eq((uintptr_t)tables[3] | DESC_VALID | DESC_TABLE, tables[1][1]);
        for (size_t slot = 1; slot < 512; ++slot)
            assert_eq(0, tables[0][slot]);
        for (size_t slot = 2; slot < 512; ++slot)
            assert_eq(0, tables[1][slot]);
        for (size_t table = 2; table < 4; ++table) {
            for (size_t slot = 0; slot < 512; ++slot) {
                uint64_t pa = ((table - 2) * 512 + slot) * UINT64_C(0x200000);
                uint64_t attrs = pa >= 0x08000000 && pa < 0x0a000000
                    ? DESC_DEVICE_BLOCK : DESC_NORMAL_BLOCK;
                uint64_t xn = DESC_UXN | (pa == boot_base ? 0 : DESC_PXN);
                assert_eq(pa | attrs | xn, tables[table][slot]);
            }
        }
        for (size_t slot = 0; slot < 512; ++slot) {
            assert_eq(0, tables[4][slot]);
            assert_eq(0, tables[5][slot]);
        }
    }
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_bootstrap_table_descriptors),
    TEST_ENTRY(test_ram_normalize_boot_excludes_b),
    TEST_ENTRY(test_arena_plan_starts_after_b),
    TEST_ENTRY(test_tree_build_validate_maps_b_exec),
    TEST_ENTRY(test_r_covering_b_rejected),
TEST_LIST_END

int main(void)
{
    TEST_SUITE("AArch64 boot block ownership through M1");
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}
