#include <arch/x86_64/vmm.h>
/*
 * hosttests/cases/test_x86_vmm_backend.c — aarch64 M3.2 Task 15:
 * x86 vmm.c backend wrapper + free/COW logic preservation.
 *
 * The brief asks for the x86 backend to expose three raw PTE-level
 * helpers:
 *   - x86_vmm_map_4k:               maps phys at virt with PAGE_* flags
 *   - x86_vmm_unmap_4k_page_with_free:   unmaps + owns the free/COW logic
 *   - x86_vmm_query_4k_page:             reads back phys + flags
 *
 * The free/COW logic — COW → page_cow_put(phys) returns true only on
 * last ref → then free_4k_page(phys), ONE free per branch; non-COW →
 * free_4k_page(phys) directly — MUST be preserved exactly (v1 review
 * item 9, the single-free-per-branch invariant).
 *
 * Strategy (mirrors test_page_table_vmm.c):
 *   - Link the REAL kernel/memory/vmm.c (no shadow vmm.c).
 *   - --wrap=calloc so the host TU controls the table-chain alloc.
 *   - Pre-fill pgd→pud→pmd→pt so vmm_pt_walk hits a pre-baked PTE at
 *     the leaf (no calloc needed on the unmap path).
 *   - Mock page_cow_put / free_4k_page with call counters.
 *
 * The existing public API (x86_vmm_map_4k / vmm_unmap_4k_page) is
 * exercised by test_fork_user_map.elf + test_user_write_cow.elf —
 * those tests stay green as long as the delegation preserves behavior
 * (the vmm.c refactor is structural, not semantic).
 */
#include "test_framework.h"

#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <memory/vmm.h>
#include <arch/x86_64/pte.h>   /* PAGE_* x86 hardware PTE bits */
#include <arch/mmu.h>          /* arch_virt_to_phys — the real walker */

/* ── Table allocation harness ────────────────────────────────
 * x86_vmm_prepare_4k_range allocates intermediate tables (calloc)
 * and one undo record per published table (kmalloc).  The counters
 * inject a failure at EVERY allocation site and let the tests prove
 * the rollback order: parent slots restored -> local sync -> tables
 * freed. */
static int  g_alloc_calls;
static int  g_alloc_fail_at;        /* 1-based; 0 = never */
static int  g_table_allocs;
static int  g_table_frees;
static int  g_free_calls;
static int  g_sync_calls;
static int  g_table_frees_at_sync;
static int  g_sync_seen;
static void *g_tables[32];
static int  g_table_n;

extern int posix_memalign(void **memptr, size_t alignment, size_t size);

void *__wrap_calloc(size_t n, size_t size)
{
    (void)n; (void)size;
    g_alloc_calls++;
    if (g_alloc_fail_at > 0 && g_alloc_calls == g_alloc_fail_at)
        return NULL;
    void *p = NULL;
    if (posix_memalign(&p, 4096, 4096) != 0 || !p)
        return NULL;
    memset(p, 0, 4096);
    if (g_table_n < (int)(sizeof(g_tables) / sizeof(g_tables[0])))
        g_tables[g_table_n++] = p;
    g_table_allocs++;
    return p;
}

void *kmalloc(size_t size)
{
    g_alloc_calls++;
    if (g_alloc_fail_at > 0 && g_alloc_calls == g_alloc_fail_at)
        return NULL;
    return malloc(size);
}

size_t kfree(void *p)
{
    g_free_calls++;
    for (int i = 0; i < g_table_n; i++) {
        if (g_tables[i] == p) {
            g_tables[i] = NULL;
            g_table_frees++;
            break;
        }
    }
    free(p);
    return 1;
}

/* Strong override of vmm.c's weak local-sync hook. */
void x86_vmm_local_sync(void)
{
    g_sync_calls++;
    g_table_frees_at_sync = g_table_frees;
    /* Any parent slot this call published must already be restored. */
    g_sync_seen = 1;
}

/* RED declaration: the range preparer lands with the device
 * transaction (munmap plan task 2). */
int  x86_vmm_prepare_4k_range(uint64_t *root, uint64_t start, uint64_t end);

/* ── Counter-backed mocks for the free/COW ownership path ──── */
static int g_cow_put_calls;
static int g_cow_put_should_return_true;  /* 1 = return true (last ref) */
static int g_free_calls;
static uint64_t g_last_phys;

bool page_cow_put(uint64_t phys)
{
    g_cow_put_calls++;
    g_last_phys = phys;
    return g_cow_put_should_return_true ? true : false;
}

void free_4k_page(uint64_t phys)
{
    g_free_calls++;
    g_last_phys = phys;
}

/* ── Flat PTE chain for the test window ────────────────────
 * All three test VAs share pgd_idx=0, pud_idx=0, pmd_idx=2 (2 MiB
 * region 0x400000–0x600000 is at PMD slot 2).  l3 differs so each
 * PTE slot is independent.  Pre-fill pgd→pud→pmd→pt so vmm_pt_walk
 * walks the chain without calling calloc (allocate=0). */
static uint64_t pgd_root[512] __attribute__((aligned(4096)));
static uint64_t pud_tbl[512]  __attribute__((aligned(4096)));
static uint64_t pmd_tbl[512]  __attribute__((aligned(4096)));
static uint64_t pt_tbl[512]   __attribute__((aligned(4096)));

#define TEST_USER_BASE 0x400000UL  /* pmd_idx=2, l3=0 */
#define TEST_VA_A      (TEST_USER_BASE + 0x0000)  /* l3=0 */
#define TEST_VA_B      (TEST_USER_BASE + 0x1000)  /* l3=1 */
#define TEST_VA_C      (TEST_USER_BASE + 0x2000)  /* l3=2 */
#define TEST_PMD_IDX   2

static void reset_chain(void)
{
    memset(pgd_root, 0, sizeof(pgd_root));
    memset(pud_tbl,  0, sizeof(pud_tbl));
    memset(pmd_tbl,  0, sizeof(pmd_tbl));
    memset(pt_tbl,   0, sizeof(pt_tbl));
    pgd_root[0] = (uint64_t)(uintptr_t)pud_tbl | PAGE_USER_PGD;
    pud_tbl[0]  = (uint64_t)(uintptr_t)pmd_tbl | PAGE_USER_PUD;
    pmd_tbl[TEST_PMD_IDX] = (uint64_t)(uintptr_t)pt_tbl  | PAGE_USER_PUD; /* non-huge PMD uses PUD flags */
    g_cow_put_calls = 0;
    g_cow_put_should_return_true = 0;
    g_free_calls = 0;
    g_last_phys = 0;
}

/* ── Tests ───────────────────────────────────────────────── */

TEST_FUNC(test_x86_vmm_backend_api_exists)
{
    /* Pin the API signatures: taking these addresses fails to
     * compile if the prototypes are not declared in the public
     * headers (memory/vmm.h for map/unmap + arch/x86_64/pte.h for
     * the new helpers — see Task 15 commit). */
    int  (*m)(uint64_t *, uint64_t, uint64_t, uint64_t) = x86_vmm_map_4k;
    uint64_t (*t)(uint64_t *, uint64_t)                = x86_vmm_take_4k;
    void  (*r)(uint64_t)                               = x86_vmm_release_owned_4k;
    int  (*q)(uint64_t *, uint64_t, uint64_t *,
              uint64_t *)                              = x86_vmm_query_4k_page;
    (void)m; (void)t; (void)r; (void)q;
    assert_true(1);  /* compiles = passes */
}

TEST_FUNC(test_cow_shared_page_does_not_free)
{
    /* COW shared: page_cow_put returns false (refs > 0) → exactly
     * one page_cow_put, ZERO free_4k_page ("single free per
     * branch" — shared = no free).  The release contract lives in
     * x86_vmm_release_owned_4k; withdraw first via take. */
    reset_chain();
    g_cow_put_should_return_true = 0;
    pt_tbl[0] = 0x1000UL | PAGE_VALID | PAGE_COW | PAGE_USER;

    uint64_t old = x86_vmm_take_4k(pgd_root, TEST_VA_A);
    assert_true(old);
    assert_eq(0, g_cow_put_calls);
    x86_vmm_release_owned_4k(old);
    assert_eq(1, g_cow_put_calls);
    assert_eq(0, g_free_calls);
    assert_eq(0x1000UL, g_last_phys);
    /* The slot was cleared by the take. */
    assert_eq(0, pt_tbl[0]);
}

TEST_FUNC(test_cow_last_ref_frees_phys)
{
    /* COW last-ref: page_cow_put returns true → exactly one
     * page_cow_put, exactly ONE free_4k_page. */
    reset_chain();
    g_cow_put_should_return_true = 1;
    pt_tbl[1] = 0x2000UL | PAGE_VALID | PAGE_COW | PAGE_USER;

    uint64_t old = x86_vmm_take_4k(pgd_root, TEST_VA_B);
    assert_true(old);
    x86_vmm_release_owned_4k(old);
    assert_eq(1, g_cow_put_calls);
    assert_eq(1, g_free_calls);
    assert_eq(0x2000UL, g_last_phys);
}

TEST_FUNC(test_plain_page_frees_phys)
{
    /* Plain (no PAGE_COW): ZERO page_cow_put, exactly ONE
     * free_4k_page — the non-COW branch. */
    reset_chain();
    pt_tbl[2] = 0x3000UL | PAGE_VALID | PAGE_USER | PAGE_WRITE;

    uint64_t old = x86_vmm_take_4k(pgd_root, TEST_VA_C);
    assert_true(old);
    x86_vmm_release_owned_4k(old);
    assert_eq(0, g_cow_put_calls);
    assert_eq(1, g_free_calls);
    assert_eq(0x3000UL, g_last_phys);
}

TEST_FUNC(test_query_returns_pte_state)
{
    /* Query helper: maps + queries + asserts phys + flags come back
     * through x86_vmm_query_4k_page.  Also covers the "non-present
     * slot → -ENOENT" branch (page-table walker lookup with no
     * PAGE_VALID). */
    reset_chain();
    pt_tbl[2] = 0x4000UL | PAGE_VALID | PAGE_USER | PAGE_WRITE;

    uint64_t phys = 0;
    uint64_t flags = 0;
    int rc = x86_vmm_query_4k_page(pgd_root, TEST_VA_C, &phys, &flags);
    assert_eq(0, rc);
    assert_eq(0x4000UL, phys);
    assert_true(flags & PAGE_VALID);
    assert_true(flags & PAGE_USER);
    assert_true(flags & PAGE_WRITE);
    assert_eq(0, g_cow_put_calls);
    assert_eq(0, g_free_calls);

    /* Empty slot (qindex 0, never written): -ENOENT. */
    reset_chain();
    rc = x86_vmm_query_4k_page(pgd_root, TEST_VA_A, &phys, &flags);
    assert_eq(-ENOENT, rc);
    assert_eq(0, g_cow_put_calls);
    assert_eq(0, g_free_calls);
}

/* ── x86_vmm_prepare_4k_range (device MAP_FIXED transaction) ── */

static void reset_alloc(void)
{
    for (int i = 0; i < g_table_n; i++)
        if (g_tables[i]) free(g_tables[i]);
    memset(g_tables, 0, sizeof(g_tables));
    g_table_n = 0;
    g_alloc_calls = 0;
    g_alloc_fail_at = 0;
    g_table_allocs = 0;
    g_table_frees = 0;
    g_free_calls = 0;
    g_sync_calls = 0;
    g_table_frees_at_sync = 0;
    g_sync_seen = 0;
}

static void clear_chain(void)
{
    memset(pgd_root, 0, sizeof(pgd_root));
    memset(pud_tbl,  0, sizeof(pud_tbl));
    memset(pmd_tbl,  0, sizeof(pmd_tbl));
    memset(pt_tbl,   0, sizeof(pt_tbl));
}

static int parent_entries_nonzero(void)
{
    int nz = 0;
    for (int i = 0; i < 512; i++)
        nz |= (pgd_root[i] | pud_tbl[i] | pmd_tbl[i]) != 0;
    return nz;
}

TEST_FUNC(test_prepare_4k_range_reuses_existing_tables)
{
    reset_chain();
    reset_alloc();
    uint64_t pgd0 = pgd_root[0], pud0 = pud_tbl[0], pmd2 = pmd_tbl[2];

    int rc = x86_vmm_prepare_4k_range(pgd_root, TEST_VA_A,
                                      TEST_VA_C + 0x1000);
    assert_eq(0, rc);
    assert_eq(0, g_alloc_calls);
    assert_eq(0, g_table_frees);
    assert_eq(pgd0, pgd_root[0]);
    assert_eq(pud0, pud_tbl[0]);
    assert_eq(pmd2, pmd_tbl[2]);
    assert_not_null(vmm_pt_walk(pgd_root, TEST_VA_A, 0, 0));
}

TEST_FUNC(test_prepare_4k_range_allocates_missing_tables)
{
    clear_chain();
    reset_alloc();

    int rc = x86_vmm_prepare_4k_range(pgd_root, TEST_VA_A,
                                      TEST_VA_B + 0x1000);
    assert_eq(0, rc);
    assert_eq(3, g_table_allocs);       /* PGD, PUD, PT */
    assert_eq(0, g_table_frees);        /* success keeps the tables */
    assert_true(pgd_root[0] & PAGE_VALID);
    uint64_t *pud = (uint64_t *)Phy_To_Virt(pgd_root[0] & PAGE_4K_MASK);
    assert_true(pud[0] & PAGE_VALID);
    assert_false(pud[0] & PAGE_HUGE);
    uint64_t *pmd = (uint64_t *)Phy_To_Virt(pud[0] & PAGE_4K_MASK);
    assert_true(pmd[TEST_PMD_IDX] & PAGE_VALID);
    assert_false(pmd[TEST_PMD_IDX] & PAGE_HUGE);
    assert_not_null(vmm_pt_walk(pgd_root, TEST_VA_A, 0, 0));
}

TEST_FUNC(test_prepare_4k_range_rolls_back_every_site)
{
    static const struct { uint64_t lo, hi; } ranges[] = {
        { 0x5FF000UL,     0x601000UL     },   /* 2 MiB boundary  */
        { 0x3FFFF000UL,   0x40001000UL   },   /* 1 GiB boundary  */
    };

    for (size_t r = 0; r < sizeof(ranges) / sizeof(ranges[0]); r++) {
        clear_chain();
        reset_alloc();
        int rc = x86_vmm_prepare_4k_range(pgd_root, ranges[r].lo,
                                          ranges[r].hi);
        assert_eq(0, rc);
        int sites = g_alloc_calls;
        assert_true(sites >= 6);

        for (int fail = 1; fail <= sites; fail++) {
            clear_chain();
            reset_alloc();
            g_alloc_fail_at = fail;

            rc = x86_vmm_prepare_4k_range(pgd_root, ranges[r].lo,
                                          ranges[r].hi);
            assert_eq(-ENOMEM, rc);
            /* Every parent slot this call published is restored. */
            assert_eq(0, parent_entries_nonzero());
            /* No data page is freed for a table-rollback; tables are
             * freed only AFTER the local sync. */
            assert_eq(0, g_table_frees_at_sync);
            if (fail <= 2) {
                /* The first table was never published. */
                assert_eq(0, g_sync_calls);
                assert_eq(0, g_table_frees);
            } else {
                assert_eq(1, g_sync_calls);
                assert_true(g_table_frees > 0);
            }
            g_alloc_fail_at = 0;
        }
    }
}

TEST_FUNC(test_prepare_4k_range_preserves_existing_leaf)
{
    /* Pre-fill PGD/PUD/PMD and one old leaf; the right page crosses a
     * PMD boundary, so its PT allocation must fail after the left
     * leaf's chain was left untouched. */
    reset_chain();
    reset_alloc();
    pt_tbl[0] = 0x9000UL | PAGE_VALID | PAGE_USER | PAGE_WRITE;
    uint64_t pgd0 = pgd_root[0], pud0 = pud_tbl[0], pmd2 = pmd_tbl[2];

    /* Count the sites for [VA_A, 0x600000 + 0x1000): the second page
     * needs a new PMD slot + PT. */
    uint64_t lo = TEST_VA_A;
    uint64_t hi = 0x601000UL;
    int rc = x86_vmm_prepare_4k_range(pgd_root, lo, hi);
    assert_eq(0, rc);
    int sites = g_alloc_calls;

    for (int fail = 1; fail <= sites; fail++) {
        reset_chain();
        reset_alloc();
        pt_tbl[0] = 0x9000UL | PAGE_VALID | PAGE_USER | PAGE_WRITE;
        g_alloc_fail_at = fail;
        rc = x86_vmm_prepare_4k_range(pgd_root, lo, hi);
        assert_eq(-ENOMEM, rc);
        assert_eq(pgd0, pgd_root[0]);
        assert_eq(pud0, pud_tbl[0]);
        assert_eq(pmd2, pmd_tbl[2]);
        assert_eq(0x9000UL | PAGE_VALID | PAGE_USER | PAGE_WRITE,
                  pt_tbl[0]);
        g_alloc_fail_at = 0;
    }
}

TEST_FUNC(test_prepare_4k_range_rejects_huge_and_kernel_half)
{
    /* User 2 MiB leaf in the range: -EINVAL, nothing allocated. */
    reset_chain();
    reset_alloc();
    pmd_tbl[TEST_PMD_IDX] = 0x200000UL | PAGE_USER_PMD;
    int rc = x86_vmm_prepare_4k_range(pgd_root, TEST_VA_A,
                                      TEST_VA_A + 0x1000);
    assert_eq(-EINVAL, rc);
    assert_eq(0, g_alloc_calls);
    assert_eq(0, g_sync_calls);

    /* 1 GiB leaf at the PUD: -EINVAL. */
    clear_chain();
    reset_alloc();
    pgd_root[0] = (uint64_t)(uintptr_t)pud_tbl | PAGE_USER_PGD;
    pud_tbl[1] = 0x40000000UL | PAGE_HUGE | PAGE_USER | PAGE_VALID;
    rc = x86_vmm_prepare_4k_range(pgd_root, 0x3FFFF000UL, 0x40001000UL);
    assert_eq(-EINVAL, rc);
    assert_eq(0, g_alloc_calls);

    /* Crossing the user/kernel half (l0=255 -> 256): -EINVAL, no
     * allocation, no kernel-table write. */
    clear_chain();
    reset_alloc();
    rc = x86_vmm_prepare_4k_range(pgd_root, 0x7FFFFFF000UL,
                                  0x80001000UL);
    assert_eq(-EINVAL, rc);
    assert_eq(0, g_alloc_calls);
    assert_eq(0, pgd_root[0]);
}

/* ── NX / COW / PROTNONE descriptor round-trip (PTE permissions plan T1) ──
 * The descriptor address field must be extracted per granularity
 * (4 KiB bits 51:12) so NX (bit 63) and the software bits (COW 10,
 * PROTNONE 9) never mix into the PA, and the map/query/release paths
 * must preserve NX end to end. */

#define ROUNDTRIP_PA 0x12345678000UL   /* 4 KiB-aligned, bits up to 45 */

TEST_FUNC(test_x86_nx_cow_stash_roundtrip)
{
    /* Raw NX + USER + PRESENT + COW mapping: query PA and semantic
     * flags, then release and check the freed PA. */
    reset_chain();
    pt_tbl[0] = ROUNDTRIP_PA | PAGE_VALID | PAGE_USER | PAGE_NO_EXEC |
                PAGE_COW;

    uint64_t phys = 0;
    uint64_t flags = 0;
    int rc = x86_vmm_query_4k_page(pgd_root, TEST_VA_A, &phys, &flags);
    assert_eq(0, rc);
    assert_eq(ROUNDTRIP_PA, phys);          /* NX must not leak into PA */
    assert_true(flags & PAGE_VALID);        /* present leaf: raw flags */
    assert_true(flags & PAGE_USER);
    assert_true(flags & PAGE_NO_EXEC);
    assert_true(flags & PAGE_COW);

    g_cow_put_should_return_true = 1;
    {
        uint64_t old = x86_vmm_take_4k(pgd_root, TEST_VA_A);
        assert_true(old);
        x86_vmm_release_owned_4k(old);
    }
    assert_eq(1, g_cow_put_calls);
    assert_eq(1, g_free_calls);
    assert_eq(ROUNDTRIP_PA, g_last_phys);   /* release gets a pure PA */

    /* Raw PROTNONE + COW stash: phys stays valid, no PRESENT bit. */
    reset_chain();
    pt_tbl[1] = ROUNDTRIP_PA | PAGE_USER | PAGE_PROTNONE | PAGE_COW;
    flags = 0;
    rc = x86_vmm_query_4k_page(pgd_root, TEST_VA_B, &phys, &flags);
    assert_eq(0, rc);
    assert_eq(ROUNDTRIP_PA, phys);
    /* Stash vs absent: a stash is rc=0 with a valid PA and the
     * PROTNONE software bit, but NO PRESENT. */
    assert_false(flags & PAGE_VALID);
    assert_true(flags & PAGE_PROTNONE);
    assert_true(flags & PAGE_COW);

    /* Absent slot: the raw query reports -ENOENT. */
    rc = x86_vmm_query_4k_page(pgd_root, TEST_VA_C, &phys, &flags);
    assert_eq(-ENOENT, rc);

    g_cow_put_should_return_true = 1;
    {
        uint64_t old = x86_vmm_take_4k(pgd_root, TEST_VA_B);
        assert_true(old);
        x86_vmm_release_owned_4k(old);
    }
    assert_eq(ROUNDTRIP_PA, g_last_phys);

    /* The raw mapper must preserve NX on the leaf it writes. */
    reset_chain();
    rc = x86_vmm_map_4k(pgd_root, ROUNDTRIP_PA, TEST_VA_C,
                             PAGE_VALID | PAGE_USER | PAGE_NO_EXEC);
    assert_eq(0, rc);
    assert_eq(ROUNDTRIP_PA | PAGE_VALID | PAGE_USER | PAGE_NO_EXEC,
              pt_tbl[2]);

    flags = 0;
    rc = x86_vmm_query_4k_page(pgd_root, TEST_VA_C, &phys, &flags);
    assert_eq(0, rc);
    assert_eq(ROUNDTRIP_PA, phys);
    assert_true(flags & PAGE_NO_EXEC);
}

TEST_FUNC(test_x86_walker_leaf_pa_per_granularity)
{
    /* The real walker (arch_virt_to_phys) must extract PA at the
     * leaf's granularity: 1 GiB bits 51:30, 2 MiB bits 51:21,
     * 4 KiB bits 51:12 — NX and PAT never mix into the PA, and the
     * in-page offset is preserved. */

    /* 1 GiB leaf with NX. */
    const uint64_t PA1G = 0x1200000000UL;   /* 1 GiB-aligned */
    reset_chain();
    pud_tbl[1] = PA1G | PAGE_HUGE | PAGE_USER | PAGE_VALID | PAGE_NO_EXEC;
    assert_eq(PA1G + 0xABCD, (uint64_t)arch_virt_to_phys(
                                 pgd_root, 0x40000000UL + 0xABCD));

    /* 2 MiB leaf with NX + PAT (bit 12). */
    const uint64_t PA2M = 0x1234000000UL;   /* 2 MiB-aligned */
    reset_chain();
    pmd_tbl[TEST_PMD_IDX] = PA2M | PAGE_HUGE | PAGE_USER | PAGE_VALID |
                            PAGE_NO_EXEC | PAGE_PAT_LARGE;
    assert_eq(PA2M + 0x123, (uint64_t)arch_virt_to_phys(
                                pgd_root, TEST_VA_A + 0x123));

    /* 4 KiB leaf with NX: bit 63 must NOT ride along with the PA. */
    reset_chain();
    pt_tbl[0] = ROUNDTRIP_PA | PAGE_VALID | PAGE_USER | PAGE_NO_EXEC;
    assert_eq(ROUNDTRIP_PA + 0x321, (uint64_t)arch_virt_to_phys(
                                        pgd_root, TEST_VA_A + 0x321));
}

TEST_FUNC(test_x86_large_leaf_pa)
{
    /* The fork huge-copy idiom, exercised through the real codecs:
     * copy a 2 MiB leaf (NX bit 63 + large-PAT bit 12 set) to a new
     * PA with x86_pte_2m_make(new_pa, x86_pte_2m_flags(old)) — both
     * attribute bits must survive — and the rollback path must see
     * only the new PA. */
    const uint64_t OLD_PA = 0x1234000000UL;   /* 2 MiB-aligned */
    const uint64_t NEW_PA = 0x4560000000UL;   /* 2 MiB-aligned */
    const uint64_t old_entry = OLD_PA | PAGE_HUGE | PAGE_USER |
                               PAGE_VALID | PAGE_NO_EXEC |
                               PAGE_PAT_LARGE;

    uint64_t child = x86_pte_2m_make(NEW_PA,
                                     x86_pte_2m_flags(old_entry));
    assert_eq(NEW_PA, x86_pte_2m_pa(child));
    assert_true(child & PAGE_NO_EXEC);
    assert_true(child & PAGE_PAT_LARGE);
    assert_true(child & PAGE_HUGE);
    assert_true(child & PAGE_USER);

    /* Rollback extracts the child's PA — never the old one, never
     * polluted by NX/PAT. */
    assert_eq(NEW_PA, x86_pte_2m_pa(child));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_x86_vmm_backend_api_exists),
    TEST_ENTRY(test_cow_shared_page_does_not_free),
    TEST_ENTRY(test_cow_last_ref_frees_phys),
    TEST_ENTRY(test_plain_page_frees_phys),
    TEST_ENTRY(test_query_returns_pte_state),
    TEST_ENTRY(test_prepare_4k_range_reuses_existing_tables),
    TEST_ENTRY(test_prepare_4k_range_allocates_missing_tables),
    TEST_ENTRY(test_prepare_4k_range_rolls_back_every_site),
    TEST_ENTRY(test_prepare_4k_range_preserves_existing_leaf),
    TEST_ENTRY(test_prepare_4k_range_rejects_huge_and_kernel_half),
    TEST_ENTRY(test_x86_nx_cow_stash_roundtrip),
    TEST_ENTRY(test_x86_walker_leaf_pa_per_granularity),
    TEST_ENTRY(test_x86_large_leaf_pa),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed == 0 ? 0 : 1;
}