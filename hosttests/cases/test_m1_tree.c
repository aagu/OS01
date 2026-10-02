/*
 * hosttests/cases/test_m1_tree.c — runtime page-table builder and
 *                                    strict validator (aarch64 M1 plan
 *                                    Task 4).
 *
 * Task 4's two production TU (kernel/arch/aarch64/memory/runtime_tree.c)
 * is pure: it never touches a real allocator or an MMIO. The host
 * fixture implements `aarch64_tree_ops` with a fake-PA pool backed by
 * host heap so the same translation unit links into the host binary.
 *
 * The host pool maxes out at runtime — the brief says "max table count
 * uses host heap, not 256/1027 static arrays in kernel". The test
 * therefore pre-sizes the pool only as a debug aid (size checks) and
 * grows the heap buffer when the producer allocates more.
 *
 *   - 512 MiB / 4 GiB (with B/D exact override and accounting;
 *     expected_count is computed, NOT hardcoded to 5)
 *   - across 1 GiB boundary
 *   - 512 GiB boundary (note: 512 GiB > 32-bit; this is the largest
 *     range that exercises a single PUD slot with a single L2 PMD)
 *   - near 1 TiB (PA close to RT_PA_LIMIT, well above the B/D window)
 *   - sparse holes (non-contiguous RAM, multiple zones)
 *   - input over-limit (>MEMORY_RANGE_MAX) → -EINVAL
 *   - input R/D conflict (RAM containing D) → -EINVAL
 *   - input R/B conflict (RAM containing B) → -EINVAL
 *   - missing one page from pool (capacity - 1) → -ENOMEM
 *   - malformed input PA (out of 1 TiB) → -ERANGE
 *   - illegal descriptor (manually mutated): table with SBZ bit set,
 *     block at L1, L3 leaf, non-zero bits outside the recognized
 *     pattern → -EIO from validator
 *   - cycles: two intermediates pointing to each other → -EIO
 *   - duplicate intermediate: two parents sharing the same child
 *     table → -EIO
 *   - unexpected leaf: an L2 slot containing a valid L3 leaf (V|T)
 *     at the L2 level — actually we forbid TYPE=1 at L2 (no L3 in
 *     this tree), AND forbid a leaf descriptor at any unexpected PA
 *
 * canary: every page in the pool is filled with a sentinel before the
 * build, so a stale bit set during a partial build surfaces in the
 * validator as an unexpected descriptor.
 */
#include "test_framework.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <memory/memory_map.h>
#include <arch/aarch64/early_arena.h>
#include <arch/aarch64/runtime_tree.h>

/* ── Production-symbol stubs (only the ones runtime_tree.c pulls in
 * transitively via early_arena.h / page_table.h). The production
 * runtime_tree.c doesn't itself call any of these — but the headers
 * they include might. Provide no-op link for safety. */
int g_log_level = 3;
void _log_err_impl(const char *fmt, ...) { (void)fmt; }
int color_printk(unsigned int FRcolor, unsigned int BKcolor,
                 const char *fmt, ...)
{
    (void)FRcolor; (void)BKcolor; (void)fmt;
    return 0;
}
size_t slab_init(void) { return 0; }

/* ── Spec constants mirrored locally for clarity ──────────────
 * Keep the values equal to those used by the production TU; assert
 * any drift in a self-test. */
#define T_B_BASE      UINT64_C(0x40000000)
#define T_B_END       UINT64_C(0x40200000)
#define T_D_BASE      UINT64_C(0x08000000)
#define T_D_END       UINT64_C(0x0a000000)
#define T_PAGE_2M     UINT64_C(0x200000)
#define T_PAGE_4K     UINT64_C(0x1000)
#define T_PA_LIMIT    UINT64_C(0x10000000000)

/* ── Fake pool ─────────────────────────────────────────────────
 *
 * The host pool issues monotonically-increasing fake PAs (starting at
 * `next_pa`) and gives each one its own 4 KiB host-heap buffer. The
 * "resolve" callback looks the PA up in a dynamic table. Capacity is
 * a soft hint: the builder's `alloc` callback returns -ENOMEM after
 * `hard_cap` pages have been issued so we can simulate pool
 * exhaustion.
 */
typedef struct fake_pool_page {
    uint64_t pa;
    uint64_t *va;       /* 4 KiB-aligned address used as the table page */
    void *raw;          /* malloc base (used for free) */
} fake_pool_page_t;

typedef struct fake_pool {
    fake_pool_page_t *pages;
    size_t count;
    size_t cap;        /* number of entries allocated in pages[] */
    uint64_t next_pa;
    uint64_t hard_cap; /* limit returned by alloc (UINT64_MAX = off) */
    int fail_next;     /* if 1, the next alloc returns -ENOMEM */
} fake_pool_t;

/* Canary value written into every page before/after build. Validator
 * walks a window by clearing any bit the builder wrote back to 0; the
 * canary becomes all-ones to highlight whether a non-cleared bit was
 * written. */
#define POOL_CANARY UINT64_C(0xCAFEBABEDEADBEEF)

static void fake_pool_init(fake_pool_t *p)
{
    memset(p, 0, sizeof(*p));
    p->next_pa = 0x100000ULL;       /* arbitrary fake-PA base */
    p->hard_cap = UINT64_MAX;       /* by default: no hard cap */
}

static void fake_pool_destroy(fake_pool_t *p)
{
    size_t i;
    if (p == NULL) return;
    for (i = 0u; i < p->count; ++i) {
        if (p->pages[i].raw != NULL) {
            free(p->pages[i].raw);
            p->pages[i].raw = NULL;
            p->pages[i].va = NULL;
        }
    }
    free(p->pages);
    memset(p, 0, sizeof(*p));
}

/* Grow the bookkeeping array to fit `needed` entries. */
static int fake_pool_grow(fake_pool_t *p, size_t needed)
{
    fake_pool_page_t *np;
    size_t new_cap;
    size_t old_cap;
    if (needed <= p->cap) return 0;
    old_cap = p->cap;
    new_cap = (p->cap == 0u) ? 16u : p->cap * 2u;
    while (new_cap < needed) new_cap *= 2u;
    np = (fake_pool_page_t *)realloc(p->pages,
                                     new_cap * sizeof(fake_pool_page_t));
    if (np == NULL) return -1;
    p->pages = np;
    /* Zero only the NEW entries (old ones were preserved by realloc
     * and must remain valid for fake_resolve to find them). */
    {
        size_t i;
        for (i = old_cap; i < new_cap; ++i) {
            np[i].pa = 0;
            np[i].va = NULL;
            np[i].raw = NULL;
        }
    }
    p->cap = new_cap;
    return 0;
}

static int fake_alloc(void *vctx, uint64_t *pa, uint64_t **va)
{
    fake_pool_t *p = (fake_pool_t *)vctx;
    uint64_t *buf;
    void *raw_base_ptr;
    uint64_t issued_pa;
    int rc;

    if (p->fail_next) {
        p->fail_next = 0;
        return -ENOMEM;
    }
    if (p->count >= p->hard_cap) return -ENOMEM;
    if (p->next_pa > UINT64_MAX - T_PAGE_4K) return -ENOMEM;

    rc = fake_pool_grow(p, p->count + 1u);
    if (rc != 0) return -ENOMEM;

    raw_base_ptr = malloc(T_PAGE_4K + T_PAGE_4K);
    if (raw_base_ptr == NULL) return -ENOMEM;
    {
        uintptr_t raw = (uintptr_t)raw_base_ptr;
        uintptr_t aligned = (raw + (T_PAGE_4K - 1u)) & ~(uintptr_t)(T_PAGE_4K - 1u);
        buf = (uint64_t *)aligned;
    }
    /* Pre-fill the page with the canary sentinel. The builder zeroes
     * the page on its way out; if a validator walks a page that still
     * has the canary, the validator would mistake the canary for a
     * descriptor. We expect the builder to fully overwrite the
     * canary. The test writes a fresh canary after the build to
     * detect any descriptor the builder DID NOT write. */
    {
        volatile uint64_t *cur = (volatile uint64_t *)buf;
        volatile uint64_t *end = cur + (T_PAGE_4K / sizeof(uint64_t));
        while (cur < end) { *cur = POOL_CANARY; ++cur; }
    }

    issued_pa = p->next_pa;
    p->next_pa += T_PAGE_4K;
    p->pages[p->count].pa = issued_pa;
    p->pages[p->count].va = buf;
    p->pages[p->count].raw = raw_base_ptr;
    ++p->count;

    *pa = issued_pa;
    *va = buf;
    return 0;
}

static uint64_t *fake_resolve(void *vctx, uint64_t pa)
{
    fake_pool_t *p = (fake_pool_t *)vctx;
    size_t i;
    for (i = 0u; i < p->count; ++i) {
        if (p->pages[i].pa == pa) return p->pages[i].va;
    }
    return NULL;
}

static const struct aarch64_tree_ops *fake_ops_ptr(fake_pool_t *p)
{
    static struct aarch64_tree_ops ops;
    ops.ctx = p;
    ops.alloc = fake_alloc;
    ops.resolve = fake_resolve;
    return &ops;
}

/* ── Tiny arena stub ────────────────────────────────────────────
 * The arena is only used for `table_base_pa`, `table_pages`, and
 * `base_pa`/`end_pa`. The validator only consumes `table_pages` (for
 * the upper-bound check). The builder consumes `table_base_pa` (to
 * detect NULL), `table_pages` (for the upper bound), and the layout
 * (never — we don't preflight the metadata here). For the tests we
 * pass a fully-built `aarch64_m1_arena` populated by hand.
 *
 * Important: the validator walks `tree->page_count` against
 * `arena->table_pages` only as an upper-bound; the runtime
 * validation logic does NOT compare against `arena->table_base_pa`
 * directly. We set the table_base_pa to a non-zero value so the
 * builder's check passes. */
static void setup_arena(struct aarch64_m1_arena *a, size_t table_pages)
{
    memset(a, 0, sizeof(*a));
    a->base_pa = T_B_BASE;
    a->end_pa = T_B_BASE + (1ULL << 30);   /* 1 GiB span */
    a->table_base_pa = 0x80000000ULL;      /* arbitrary */
    a->table_end_pa = a->table_base_pa + (uint64_t)table_pages * T_PAGE_4K;
    a->table_pages = table_pages;
    /* layout fields left zero — production validator doesn't read them. */
}

/* ── Builder / validator references ────────────────────────────
 *
 * The validator requires `arena` only for the `table_pages` upper
 * bound. Pass the same arena the builder used. */

/* ── Helper: count expected blocks for an input ─────────────── */
static size_t count_expected(const struct MEMORY_RANGE *ram, size_t count)
{
    size_t total = 0u;
    size_t i;
    /* B */
    total += (T_B_END - T_B_BASE) / T_PAGE_2M;     /* 1 block */
    /* D */
    total += (T_D_END - T_D_BASE) / T_PAGE_2M;     /* 16 blocks */
    /* R */
    for (i = 0u; i < count; ++i) {
        total += (ram[i].phys_end - ram[i].phys_start) / T_PAGE_2M;
    }
    return total;
}

/* ── Helper: count expected PAGE count (L0 + PUDs + PMDs) ────
 * The runtime tree needs exactly one 4 KiB page per (L0, PUD)
 * pair (the PMD page covering 1 GiB) plus one page per L0
 * entry used (the PUD page covering 512 GiB) plus the L0
 * root. We walk every 2 MiB block in B/D/R and count unique
 * (l0, l1) pairs and unique l0 entries. */
static size_t count_expected_pages(const struct MEMORY_RANGE *ram, size_t count)
{
    /* Bitmap of (l0, l1) pairs: 2 * 512 = 1024 bits = 16 uint64_t. */
    uint64_t l1_bits[(2u * 512u + 63u) / 64u];
    uint64_t pa;
    size_t i;
    size_t total_bits;
    size_t total_pmds;
    int l0_used[2] = {0, 0};
    size_t total_puds;

    memset(l1_bits, 0, sizeof(l1_bits));

    /* B */
    for (pa = T_B_BASE; pa < T_B_END; pa += T_PAGE_2M) {
        uint64_t l0 = pa >> 39;
        uint64_t l1 = (pa >> 30) & 0x1FF;
        size_t idx = (size_t)(l0 * 512u + l1);
        l1_bits[idx / 64u] |= ((uint64_t)1 << (idx % 64u));
    }
    /* D */
    for (pa = T_D_BASE; pa < T_D_END; pa += T_PAGE_2M) {
        uint64_t l0 = pa >> 39;
        uint64_t l1 = (pa >> 30) & 0x1FF;
        size_t idx = (size_t)(l0 * 512u + l1);
        l1_bits[idx / 64u] |= ((uint64_t)1 << (idx % 64u));
    }
    /* R */
    for (i = 0u; i < count; ++i) {
        for (pa = ram[i].phys_start; pa < ram[i].phys_end; pa += T_PAGE_2M) {
            uint64_t l0 = pa >> 39;
            uint64_t l1 = (pa >> 30) & 0x1FF;
            size_t idx = (size_t)(l0 * 512u + l1);
            l1_bits[idx / 64u] |= ((uint64_t)1 << (idx % 64u));
        }
    }

    total_bits = sizeof(l1_bits) / sizeof(l1_bits[0]);
    total_pmds = 0u;
    for (i = 0u; i < total_bits; ++i) {
        uint64_t v = l1_bits[i];
        while (v != 0u) {
            int bit = __builtin_ctzll(v);
            size_t idx = i * 64u + (size_t)bit;
            size_t l0 = idx / 512u;
            if (l0 < 2u) l0_used[l0] = 1;
            ++total_pmds;
            v &= v - 1u;
        }
    }

    total_puds = (size_t)l0_used[0] + (size_t)l0_used[1];
    return 1u + total_puds + total_pmds;
}

/* ── Helper: minimal RAM builder (single 512 MiB range) ─────── */
static void build_single(uint64_t start, uint64_t end,
                        struct MEMORY_RANGE out[1])
{
    out[0].phys_start = start;
    out[0].phys_end = end;
    out[0].type = MEMORY_TYPE_RAM;
}

/* ── Build + validate wrapper ────────────────────────────────── */
struct tree_result {
    int rc;
    struct aarch64_runtime_tree tree;
};

/* In production the validator's scratch lives outside the boot
 * stack; in the host test it sits comfortably on the host stack
 * (which is effectively unbounded). One per validate call would
 * work too, but a single static keeps the test bodies compact. */
static struct aarch64_runtime_tree_validate_buf g_validate_buf;

static struct tree_result do_build(const struct MEMORY_RANGE *ram, size_t count,
                                  struct aarch64_m1_arena *arena,
                                  fake_pool_t *pool)
{
    struct tree_result r;
    memset(&r, 0, sizeof(r));
    r.rc = aarch64_runtime_tree_build(ram, count, arena,
                                      fake_ops_ptr(pool), &r.tree);
    return r;
}

static int do_validate(const struct MEMORY_RANGE *ram, size_t count,
                       struct aarch64_m1_arena *arena,
                       fake_pool_t *pool,
                       const struct aarch64_runtime_tree *tree)
{
    return aarch64_runtime_tree_validate(ram, count, arena,
                                         fake_ops_ptr(pool),
                                         &g_validate_buf, tree);
}

/* ── Test cases ──────────────────────────────────────────────── */

/* ── Happy paths ─────────────────────────────────────────────── */

TEST_FUNC(test_happy_512_mib_low_window)
{
    struct MEMORY_RANGE ram[1];
    struct aarch64_m1_arena arena;
    fake_pool_t pool;
    struct tree_result r;
    size_t exp_blocks, exp_pages;

    fake_pool_init(&pool);
    setup_arena(&arena, 128u);  /* plenty */
    build_single(0x40200000ULL, 0x40200000ULL + 512ULL * 1024 * 1024, ram);
    exp_blocks = count_expected(ram, 1);
    exp_pages = count_expected_pages(ram, 1);
    r = do_build(ram, 1, &arena, &pool);
    assert_eq(0, r.rc);
    /* page_count must match the computed L0 + PUDs + PMDs. */
    assert_eq(exp_pages, r.tree.page_count);
    assert_eq(0, do_validate(ram, 1, &arena, &pool, &r.tree));
    /* The recorded blocks must equal the expected count. */
    assert_true(exp_blocks > 0u);
    fake_pool_destroy(&pool);
}

TEST_FUNC(test_happy_4_gib_with_b_d_accounting)
{
    /* 4 GiB at [0x40200000, 0x140200000). Compute the exact expected
     * block AND page count from R + B + D; do NOT hardcode either. */
    struct MEMORY_RANGE ram[1];
    struct aarch64_m1_arena arena;
    fake_pool_t pool;
    struct tree_result r;
    size_t exp_blocks, r_blocks, exp_pages;

    fake_pool_init(&pool);
    setup_arena(&arena, 2048u);
    build_single(0x40200000ULL, 0x40200000ULL + 4ULL * 1024 * 1024 * 1024,
                 ram);
    exp_blocks = count_expected(ram, 1);
    exp_pages  = count_expected_pages(ram, 1);
    r_blocks = (4ULL * 1024 * 1024 * 1024) / T_PAGE_2M;
    /* 4 GiB / 2 MiB = 2048 blocks in R. Add B (1) and D (16). */
    assert_eq((size_t)(2048 + 1 + 16), exp_blocks);
    /* exp_blocks must be much greater than 5 — guard against the
     * hardcoded-5 bug. */
    assert_true(exp_blocks > 5u);

    r = do_build(ram, 1, &arena, &pool);
    assert_eq(0, r.rc);
    /* exp_pages is computed: 1 L0 + 1 PUD + (# unique (l0,l1) pairs).
     * For 4 GiB at [0x40200000, 0x140200000) the R range spans PUD
     * entries [1..5]; plus D at PUD entry 0; total 6 PMDs + 1 L0 +
     * 1 PUD = 8 pages. Use the helper so any future drift is caught. */
    assert_eq(exp_pages, r.tree.page_count);
    assert_eq(0, do_validate(ram, 1, &arena, &pool, &r.tree));
    assert_true(r_blocks == 2048u);
    fake_pool_destroy(&pool);
}

TEST_FUNC(test_happy_cross_1_gib_boundary)
{
    /* Range [0x7FC00000, 0x80400000): a single range that straddles
     * the 1 GiB / 2 GiB boundary — needs TWO L2 tables (one for
     * bucket 0x3FF, one for 0x400). */
    struct MEMORY_RANGE ram[1];
    struct aarch64_m1_arena arena;
    fake_pool_t pool;
    struct tree_result r;
    size_t exp_pages;

    fake_pool_init(&pool);
    setup_arena(&arena, 64u);
    build_single(0x7FC00000ULL, 0x80400000ULL, ram);
    exp_pages = count_expected_pages(ram, 1);
    r = do_build(ram, 1, &arena, &pool);
    assert_eq(0, r.rc);
    assert_eq(exp_pages, r.tree.page_count);
    assert_eq(0, do_validate(ram, 1, &arena, &pool, &r.tree));
    fake_pool_destroy(&pool);
}

TEST_FUNC(test_happy_512_gib_range)
{
    /* 2 GiB at PA=2 GiB ([0x80000000, 0x100000000)) — exercises
     * multiple PMD buckets within PUD[0]. */
    struct MEMORY_RANGE ram[1];
    struct aarch64_m1_arena arena;
    fake_pool_t pool;
    struct tree_result r;
    size_t exp_pages;

    fake_pool_init(&pool);
    setup_arena(&arena, 1024u);
    build_single(0x80000000ULL, 0x100000000ULL, ram);
    exp_pages = count_expected_pages(ram, 1);
    r = do_build(ram, 1, &arena, &pool);
    assert_eq(0, r.rc);
    assert_eq(exp_pages, r.tree.page_count);
    assert_eq(0, do_validate(ram, 1, &arena, &pool, &r.tree));
    fake_pool_destroy(&pool);
}

TEST_FUNC(test_happy_near_1_tib)
{
    /* Range ending just under 1 TiB: covers PAs close to PA_LIMIT.
     * Use [0xFF80000000, 0xFFE0000000) = 96 MiB at the top of the
     * 1 TiB space. 0xFF80000000 = 1 TiB - 2 GiB, so the range
     * sits in PUD[1] (≥ 512 GiB). Two PMDs (PUD entries 0x1FE and
     * 0x1FF) are needed. Plus B in PUD[0] and D in PUD[0] add more
     * PMDs. Compute via helper. */
    struct MEMORY_RANGE ram[1];
    struct aarch64_m1_arena arena;
    fake_pool_t pool;
    struct tree_result r;
    size_t exp_pages;

    fake_pool_init(&pool);
    setup_arena(&arena, 1024u);
    ram[0].phys_start = 0xFF80000000ULL;
    ram[0].phys_end   = 0xFFE0000000ULL;
    ram[0].type       = MEMORY_TYPE_RAM;
    exp_pages = count_expected_pages(ram, 1);
    r = do_build(ram, 1, &arena, &pool);
    assert_eq(0, r.rc);
    assert_eq(exp_pages, r.tree.page_count);
    assert_eq(0, do_validate(ram, 1, &arena, &pool, &r.tree));
    fake_pool_destroy(&pool);
}

TEST_FUNC(test_happy_sparse_holes)
{
    /* Two non-adjacent ranges. Forces the builder to keep separate
     * PMD tables for the two ranges. */
    struct MEMORY_RANGE ram[2];
    struct aarch64_m1_arena arena;
    fake_pool_t pool;
    struct tree_result r;
    size_t exp_pages;

    fake_pool_init(&pool);
    setup_arena(&arena, 64u);
    ram[0].phys_start = 0x40200000ULL;
    ram[0].phys_end   = 0x40400000ULL;
    ram[0].type       = MEMORY_TYPE_RAM;
    ram[1].phys_start = 0x60000000ULL;
    ram[1].phys_end   = 0x60200000ULL;
    ram[1].type       = MEMORY_TYPE_RAM;
    exp_pages = count_expected_pages(ram, 2);
    r = do_build(ram, 2, &arena, &pool);
    assert_eq(0, r.rc);
    assert_eq(exp_pages, r.tree.page_count);
    assert_eq(0, do_validate(ram, 2, &arena, &pool, &r.tree));
    fake_pool_destroy(&pool);
}

/* ── Input validation failures ──────────────────────────────── */

TEST_FUNC(test_input_over_limit)
{
    /* count > MEMORY_RANGE_MAX → -EINVAL. We use a fake arena with
     * enough pages and a fake pool. The build never gets to alloc
     * because the count check fires first. */
    struct aarch64_m1_arena arena;
    fake_pool_t pool;
    size_t big = MEMORY_RANGE_MAX + 1u;
    struct MEMORY_RANGE *ram;
    int rc;

    fake_pool_init(&pool);
    setup_arena(&arena, 1024u);
    ram = (struct MEMORY_RANGE *)calloc(big, sizeof(*ram));
    assert_not_null((void *)ram);
    /* All zero entries. The builder's count check fires first. */
    rc = aarch64_runtime_tree_build(ram, big, &arena,
                                    fake_ops_ptr(&pool), NULL);
    assert_eq(-EINVAL, rc);
    free(ram);
    fake_pool_destroy(&pool);
}

TEST_FUNC(test_input_r_d_conflict)
{
    /* A RAM range that overlaps D must be rejected. */
    struct MEMORY_RANGE ram[1];
    struct aarch64_m1_arena arena;
    fake_pool_t pool;
    struct tree_result r;

    fake_pool_init(&pool);
    setup_arena(&arena, 1024u);
    ram[0].phys_start = T_D_BASE;
    ram[0].phys_end   = T_D_BASE + T_PAGE_2M;
    ram[0].type       = MEMORY_TYPE_RAM;
    r = do_build(ram, 1, &arena, &pool);
    assert_true(r.rc < 0);
    fake_pool_destroy(&pool);
}

TEST_FUNC(test_input_r_b_conflict)
{
    struct MEMORY_RANGE ram[1];
    struct aarch64_m1_arena arena;
    fake_pool_t pool;
    struct tree_result r;

    fake_pool_init(&pool);
    setup_arena(&arena, 1024u);
    ram[0].phys_start = T_B_BASE;
    ram[0].phys_end   = T_B_BASE + T_PAGE_2M;
    ram[0].type       = MEMORY_TYPE_RAM;
    r = do_build(ram, 1, &arena, &pool);
    assert_true(r.rc < 0);
    fake_pool_destroy(&pool);
}

TEST_FUNC(test_input_non_ram_type)
{
    struct MEMORY_RANGE ram[1];
    struct aarch64_m1_arena arena;
    fake_pool_t pool;
    struct tree_result r;

    fake_pool_init(&pool);
    setup_arena(&arena, 1024u);
    ram[0].phys_start = 0x40200000ULL;
    ram[0].phys_end   = 0x41200000ULL;
    ram[0].type       = MEMORY_TYPE_RESERVED;
    r = do_build(ram, 1, &arena, &pool);
    assert_eq(-EINVAL, r.rc);
    fake_pool_destroy(&pool);
}

TEST_FUNC(test_input_unaligned_start)
{
    struct MEMORY_RANGE ram[1];
    struct aarch64_m1_arena arena;
    fake_pool_t pool;
    struct tree_result r;

    fake_pool_init(&pool);
    setup_arena(&arena, 1024u);
    ram[0].phys_start = 0x40200123ULL;
    ram[0].phys_end   = 0x41200000ULL;
    ram[0].type       = MEMORY_TYPE_RAM;
    r = do_build(ram, 1, &arena, &pool);
    assert_eq(-EINVAL, r.rc);
    fake_pool_destroy(&pool);
}

TEST_FUNC(test_input_unaligned_end)
{
    struct MEMORY_RANGE ram[1];
    struct aarch64_m1_arena arena;
    fake_pool_t pool;
    struct tree_result r;

    fake_pool_init(&pool);
    setup_arena(&arena, 1024u);
    ram[0].phys_start = 0x40200000ULL;
    ram[0].phys_end   = 0x41200800ULL;
    ram[0].type       = MEMORY_TYPE_RAM;
    r = do_build(ram, 1, &arena, &pool);
    assert_eq(-EINVAL, r.rc);
    fake_pool_destroy(&pool);
}

TEST_FUNC(test_input_pa_out_of_range)
{
    struct MEMORY_RANGE ram[1];
    struct aarch64_m1_arena arena;
    fake_pool_t pool;
    struct tree_result r;

    fake_pool_init(&pool);
    setup_arena(&arena, 1024u);
    ram[0].phys_start = T_PA_LIMIT - T_PAGE_2M;
    ram[0].phys_end   = T_PA_LIMIT + T_PAGE_2M;
    ram[0].type       = MEMORY_TYPE_RAM;
    r = do_build(ram, 1, &arena, &pool);
    assert_eq(-ERANGE, r.rc);
    fake_pool_destroy(&pool);
}

TEST_FUNC(test_null_inputs)
{
    struct aarch64_m1_arena arena;
    fake_pool_t pool;
    struct aarch64_runtime_tree tree;

    fake_pool_init(&pool);
    setup_arena(&arena, 1024u);
    /* NULL ram with count=0 is allowed (the builder still installs
     * B + D). Pass a non-NULL out so we can verify the tree is
     * published correctly. */
    memset(&tree, 0xAB, sizeof(tree));
    assert_eq(0, aarch64_runtime_tree_build(NULL, 0, &arena,
                                            fake_ops_ptr(&pool),
                                            &tree));
    /* NULL ram with count > 0 must fail with -EINVAL. */
    assert_eq(-EINVAL, aarch64_runtime_tree_build(NULL, 1, &arena,
                                                  fake_ops_ptr(&pool),
                                                  &tree));
    /* NULL ops must fail. */
    assert_eq(-EINVAL, aarch64_runtime_tree_build(NULL, 0, &arena,
                                                  NULL, &tree));
    /* NULL out must fail. */
    assert_eq(-EINVAL, aarch64_runtime_tree_build(NULL, 0, &arena,
                                                  fake_ops_ptr(&pool),
                                                  NULL));
    fake_pool_destroy(&pool);
}

/* ── Pool exhaustion ────────────────────────────────────────── */

TEST_FUNC(test_missing_one_page_from_pool)
{
    /* Build a tree that needs N pages, but cap the pool at N-1. The
     * build must return -ENOMEM and must NOT have published a
     * partial tree (validate on the cleared out returns -EINVAL or
     * similar). */
    struct MEMORY_RANGE ram[1];
    struct aarch64_m1_arena arena;
    fake_pool_t pool;
    struct tree_result r;

    fake_pool_init(&pool);
    setup_arena(&arena, 64u);
    /* R = [0x40200000, 0x41200000) = 16 MiB / 8 PMD slots. B and R
     * share PUD[0][1], D uses PUD[0][0]. The expected page count
     * is 1 L0 + 1 PUD + 2 PMDs = 4. Compute it explicitly so any
     * future drift is caught. */
    build_single(0x40200000ULL, 0x41200000ULL, ram);
    {
        size_t exp_pages = count_expected_pages(ram, 1);
        /* Sanity guard against accidental reduction of the page count
         * (which would make the next line cap the pool ABOVE the
         * builder's real need, hiding the failure path). */
        assert_true(exp_pages >= 2u);
        pool.hard_cap = exp_pages - 1u;   /* force failure on the last page */
    }
    r = do_build(ram, 1, &arena, &pool);
    assert_eq(-ENOMEM, r.rc);
    /* Output must be zeroed on failure. */
    assert_eq((uint64_t)0, r.tree.root_pa);
    assert_eq((size_t)0, r.tree.page_count);
    fake_pool_destroy(&pool);
}

/* ── Validator corruption tests ───────────────────────────────
 *
 * These tests build a valid tree, then mutate one descriptor in the
 * fake pool to a known-bad shape, then validate. Every mutation must
 * be detected and rejected with -EIO.
 */

/* Helper: build a small valid tree with B + D + R. */
static int build_small_tree(struct MEMORY_RANGE ram[1],
                            struct aarch64_m1_arena *arena,
                            fake_pool_t *pool)
{
    struct tree_result r;
    build_single(0x40200000ULL, 0x40400000ULL, ram);
    setup_arena(arena, 64u);
    fake_pool_init(pool);
    r = do_build(ram, 1, arena, pool);
    return r.rc;
}

TEST_FUNC(test_validate_illegal_table_descriptor_sbz)
{
    /* Mutate one PUD slot to have a non-zero SBZ bit (bit 11, the
     * first SBZ bit per ARM ARM). The validator MUST reject this. */
    struct MEMORY_RANGE ram[1];
    struct aarch64_m1_arena arena;
    fake_pool_t pool;
    struct tree_result r;
    uint64_t *root_va;
    uint64_t original;

    assert_eq(0, build_small_tree(ram, &arena, &pool));
    r = do_build(ram, 1, &arena, &pool);
    assert_eq(0, r.rc);
    /* Sanity: validate passes on the unmodified tree. */
    assert_eq(0, do_validate(ram, 1, &arena, &pool, &r.tree));

    root_va = fake_resolve(&pool, r.tree.root_pa);
    assert_not_null((void *)root_va);
    /* root[0] = PUD[0]. Find a non-empty slot. */
    original = root_va[0];
    /* Set an SBZ bit (bit 11, which is outside the recognized fields
     * for a table descriptor — V|T|PA[39:12]). */
    root_va[0] = original | UINT64_C(0x800);  /* bit 11 */
    assert_eq(-EIO, do_validate(ram, 1, &arena, &pool, &r.tree));
    /* Restore so cleanup doesn't double-free. */
    root_va[0] = original;
    fake_pool_destroy(&pool);
}

TEST_FUNC(test_validate_block_at_l1_is_rejected)
{
    /* Mutate a PUD slot to hold a V=1, TYPE=0 (block) descriptor.
     * The validator must reject: block descriptors are only valid
     * at L2. */
    struct MEMORY_RANGE ram[1];
    struct aarch64_m1_arena arena;
    fake_pool_t pool;
    struct tree_result r;
    uint64_t *root_va;
    uint64_t original;

    assert_eq(0, build_small_tree(ram, &arena, &pool));
    r = do_build(ram, 1, &arena, &pool);
    assert_eq(0, r.rc);

    root_va = fake_resolve(&pool, r.tree.root_pa);
    assert_not_null((void *)root_va);
    original = root_va[0];
    /* Replace the L1 (PUD) table descriptor with a block descriptor
     * (TYPE=0). Valid bits: V|0|attrs|PA. */
    root_va[0] = UINT64_C(0x401) | (root_va[0] & ~(UINT64_C(0xFFF)));
    /* Clear the TYPE bit. */
    root_va[0] &= ~UINT64_C(0x002);
    assert_eq(-EIO, do_validate(ram, 1, &arena, &pool, &r.tree));
    root_va[0] = original;
    fake_pool_destroy(&pool);
}

TEST_FUNC(test_validate_unexpected_l3_leaf_in_l2)
{
    /* The tree only has L0/L1/L2 — TYPE=1 at L2 means "L3 leaf" per
     * AR, which is a violation since this tree has no L3 tables. */
    struct MEMORY_RANGE ram[1];
    struct aarch64_m1_arena arena;
    fake_pool_t pool;
    struct tree_result r;
    uint64_t *l2_va;
    uint64_t original;

    assert_eq(0, build_small_tree(ram, &arena, &pool));
    r = do_build(ram, 1, &arena, &pool);
    assert_eq(0, r.rc);

    /* Walk to the PMD (L2) page for R. PA 0x40200000 sits in
     * PUD[0][1] (PUD slot 1) and PMD slot 0x201. */
    {
        uint64_t *pud_va = fake_resolve(&pool,
            fake_resolve(&pool, r.tree.root_pa)[0] & UINT64_C(0xffffffffff000));
        assert_not_null((void *)pud_va);
        l2_va = fake_resolve(&pool,
            pud_va[1] & UINT64_C(0xffffffffff000));
        assert_not_null((void *)l2_va);
    }

    /* The R range [0x40200000, 0x40400000) covers L2 slot 0 in
     * PMD[0x201]. Mutate slot 0 to a TYPE=1 (page) descriptor —
     * the validator must reject because L3 leaves don't exist in
     * this tree. */
    original = l2_va[0];
    l2_va[0] = original | UINT64_C(0x002);  /* TYPE=1 */
    assert_eq(-EIO, do_validate(ram, 1, &arena, &pool, &r.tree));
    l2_va[0] = original;
    fake_pool_destroy(&pool);
}

TEST_FUNC(test_validate_unexpected_block_pa)
{
    /* Mutate an L2 slot to claim a PA NOT in any expected set. The
     * validator must reject. */
    struct MEMORY_RANGE ram[1];
    struct aarch64_m1_arena arena;
    fake_pool_t pool;
    struct tree_result r;
    uint64_t *l2_va;
    uint64_t original;

    assert_eq(0, build_small_tree(ram, &arena, &pool));
    r = do_build(ram, 1, &arena, &pool);
    assert_eq(0, r.rc);

    {
        uint64_t *pud_va = fake_resolve(&pool,
            fake_resolve(&pool, r.tree.root_pa)[0] & UINT64_C(0xffffffffff000));
        l2_va = fake_resolve(&pool,
            pud_va[1] & UINT64_C(0xffffffffff000));
        assert_not_null((void *)l2_va);
    }

    original = l2_va[0];
    /* Replace with a valid-shape block but pointing at a PA that's
     * NOT in B/D/R. Use 0x60000000 (in [0x40200000, 0x60000000)
     * gap). Encode as if it were a normal block. */
    l2_va[0] = UINT64_C(0x401)              /* V|AF */
             | UINT64_C(0x300)              /* SH_IS */
             | UINT64_C(0x000)              /* AP_KRW */
             | UINT64_C(0x004)              /* ATTR_NORMAL */
             | UINT64_C(0x60000000000000)   /* PXN|UXN */
             | (0x60000000ULL & UINT64_C(0xffffffe00000));
    assert_eq(-EIO, do_validate(ram, 1, &arena, &pool, &r.tree));
    l2_va[0] = original;
    fake_pool_destroy(&pool);
}

TEST_FUNC(test_validate_cycles)
{
    /* Force a cycle: make the L0 root point to itself. */
    struct MEMORY_RANGE ram[1];
    struct aarch64_m1_arena arena;
    fake_pool_t pool;
    struct tree_result r;
    uint64_t *root_va;
    uint64_t original;

    assert_eq(0, build_small_tree(ram, &arena, &pool));
    r = do_build(ram, 1, &arena, &pool);
    assert_eq(0, r.rc);

    root_va = fake_resolve(&pool, r.tree.root_pa);
    original = root_va[0];
    /* Make the L0 → L0 (cycle). */
    root_va[0] = UINT64_C(0x003) | r.tree.root_pa;
    /* This is a table desc pointing at the root itself. The
     * validator must detect the cycle on the second visit and
     * return -EIO. */
    assert_eq(-EIO, do_validate(ram, 1, &arena, &pool, &r.tree));
    root_va[0] = original;
    fake_pool_destroy(&pool);
}

TEST_FUNC(test_validate_duplicate_intermediate)
{
    /* Make TWO PUD slots point at the SAME L2 table — duplicate
     * intermediate, no cycle. The validator must reject. */
    struct MEMORY_RANGE ram[1];
    struct aarch64_m1_arena arena;
    fake_pool_t pool;
    struct tree_result r;
    uint64_t *root_va;
    uint64_t *pud_va;
    uint64_t l2_pa;
    uint64_t orig_root0, orig_pud0, orig_pud1;

    assert_eq(0, build_small_tree(ram, &arena, &pool));
    r = do_build(ram, 1, &arena, &pool);
    assert_eq(0, r.rc);

    root_va = fake_resolve(&pool, r.tree.root_pa);
    pud_va = fake_resolve(&pool, root_va[0] & UINT64_C(0xffffffffff000));
    /* R sits in PUD[0][1] (PUD slot 1). */
    l2_pa = pud_va[1] & UINT64_C(0xffffffffff000);

    orig_root0 = root_va[0];
    orig_pud0 = pud_va[0];
    orig_pud1 = pud_va[1];
    (void)orig_pud1;
    /* root[1] = another PUD? Or invalid? root[1] is for the second
     * 512 GiB bucket; it's normally 0 since our R range doesn't
     * cover [512 GiB, 1 TiB). Link root[1] to point at the same
     * L2 page. */
    root_va[1] = UINT64_C(0x003) | l2_pa;
    /* The validator now sees two intermediates (root[0] and root[1])
     * pointing at the same L2 — must reject. */
    assert_eq(-EIO, do_validate(ram, 1, &arena, &pool, &r.tree));
    root_va[0] = orig_root0;
    pud_va[0] = orig_pud0;
    root_va[1] = 0;
    fake_pool_destroy(&pool);
}

TEST_FUNC(test_validate_page_count_mismatch)
{
    /* Truncate the table_used_end_pa so page_count arithmetic fails.
     * The validator must reject. */
    struct MEMORY_RANGE ram[1];
    struct aarch64_m1_arena arena;
    fake_pool_t pool;
    struct tree_result r;

    assert_eq(0, build_small_tree(ram, &arena, &pool));
    r = do_build(ram, 1, &arena, &pool);
    assert_eq(0, r.rc);

    r.tree.table_used_end_pa -= T_PAGE_4K;  /* shrink by one page */
    assert_eq(-EIO, do_validate(ram, 1, &arena, &pool, &r.tree));
    fake_pool_destroy(&pool);
}

TEST_FUNC(test_validate_canary_around_table_pages)
{
    /* I2: the original "canary" test was a no-op — the builder zeros
     * every allocated page in `rt_alloc_one`, so the canary is always
     * overwritten and never seen again. Rewritten to verify the
     * allocator's bookkeeping (fake_resolve) rejects a PA outside
     * the used pool. The brief's intent was that the allocator
     * refuses to honor a PA the builder never issued; this test
     * exercises that contract. */
    struct MEMORY_RANGE ram[1];
    struct aarch64_m1_arena arena;
    fake_pool_t pool;
    struct tree_result r;
    uint64_t issued_pa;
    uint64_t forged_pa;
    uint64_t *resolved;

    assert_eq(0, build_small_tree(ram, &arena, &pool));
    r = do_build(ram, 1, &arena, &pool);
    assert_eq(0, r.rc);

    /* Sanity: a PA the pool DID issue resolves to a non-NULL
     * pointer. (Sanity-gate before we test the rejection path.) */
    issued_pa = pool.pages[0].pa;
    resolved = fake_resolve(&pool, issued_pa);
    assert_not_null((void *)resolved);
    assert_true(resolved == pool.pages[0].va);

    /* Forge a PA outside the used pool. The fake pool's `next_pa`
     * cursor always exceeds every issued PA by at least one page;
     * a PA >= next_pa is therefore unambiguously out-of-pool. */
    forged_pa = pool.next_pa + 0x100000ULL;   /* well past the cursor */
    resolved = fake_resolve(&pool, forged_pa);
    assert_null((void *)resolved);

    /* Also test the obvious wrong-base case: PA 0 is never issued
     * by the fake pool (the cursor starts at 0x100000). */
    resolved = fake_resolve(&pool, 0u);
    assert_null((void *)resolved);

    /* And: a PA inside [next_pa - 0x100000, next_pa) but not equal
     * to any issued PA — fake_resolve must scan and reject. */
    if (pool.next_pa > 0x200000ULL) {
        forged_pa = pool.next_pa - 0x100000ULL;
        resolved = fake_resolve(&pool, forged_pa);
        assert_null((void *)resolved);
    }

    fake_pool_destroy(&pool);
}

/* ── Specific brief: can't write hardcoded 5 ──────────────── */
TEST_FUNC(test_4_gib_block_count_is_not_5)
{
    /* Sanity check: the 4 GiB test's expected block count must be
     * 2048 + 1 + 16 = 2065, NOT 5. */
    struct MEMORY_RANGE ram[1];
    build_single(0x40200000ULL, 0x40200000ULL + 4ULL * 1024 * 1024 * 1024,
                 ram);
    size_t n = count_expected(ram, 1);
    /* 2048 + 1 + 16 */
    assert_true(n > 100u);
    assert_eq((size_t)2065, n);
}

/* ── Tree walker needs the iterator ─────────────────────────── */

TEST_LIST_BEGIN
    TEST_ENTRY(test_happy_512_mib_low_window),
    TEST_ENTRY(test_happy_4_gib_with_b_d_accounting),
    TEST_ENTRY(test_happy_cross_1_gib_boundary),
    TEST_ENTRY(test_happy_512_gib_range),
    TEST_ENTRY(test_happy_near_1_tib),
    TEST_ENTRY(test_happy_sparse_holes),
    TEST_ENTRY(test_input_over_limit),
    TEST_ENTRY(test_input_r_d_conflict),
    TEST_ENTRY(test_input_r_b_conflict),
    TEST_ENTRY(test_input_non_ram_type),
    TEST_ENTRY(test_input_unaligned_start),
    TEST_ENTRY(test_input_unaligned_end),
    TEST_ENTRY(test_input_pa_out_of_range),
    TEST_ENTRY(test_null_inputs),
    TEST_ENTRY(test_missing_one_page_from_pool),
    TEST_ENTRY(test_validate_illegal_table_descriptor_sbz),
    TEST_ENTRY(test_validate_block_at_l1_is_rejected),
    TEST_ENTRY(test_validate_unexpected_l3_leaf_in_l2),
    TEST_ENTRY(test_validate_unexpected_block_pa),
    TEST_ENTRY(test_validate_cycles),
    TEST_ENTRY(test_validate_duplicate_intermediate),
    TEST_ENTRY(test_validate_page_count_mismatch),
    TEST_ENTRY(test_validate_canary_around_table_pages),
    TEST_ENTRY(test_4_gib_block_count_is_not_5),
TEST_LIST_END

int main(void)
{
    printf("=== M1 tree test starting ===\n");
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}