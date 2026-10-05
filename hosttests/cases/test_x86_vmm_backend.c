/*
 * hosttests/cases/test_x86_vmm_backend.c — aarch64 M3.2 Task 15:
 * x86 vmm.c backend wrapper + free/COW logic preservation.
 *
 * The brief asks for the x86 backend to expose three raw PTE-level
 * helpers:
 *   - x86_vmm_map_4k_page:               maps phys at virt with PAGE_* flags
 *   - x86_vmm_unmap_4k_page_with_free:   unmaps + owns the free/COW logic
 *   - x86_vmm_query_4k_page:             reads back phys + flags
 *
 * The free/COW logic — COW → page_cow_put(phys) returns true only on
 * last ref → then free_4k_page(phys), ONE free per branch; non-COW →
 * free_4k_page(phys) directly — MUST be preserved exactly (v1 review
 * item 9, the single-free-per-branch invariant).
 *
 * Strategy (mirrors test_m1_vmm.c):
 *   - Link the REAL kernel/memory/vmm.c (no shadow vmm.c).
 *   - --wrap=calloc so the host TU controls the table-chain alloc.
 *   - Pre-fill pgd→pud→pmd→pt so vmm_pt_walk hits a pre-baked PTE at
 *     the leaf (no calloc needed on the unmap path).
 *   - Mock page_cow_put / free_4k_page with call counters.
 *
 * The existing public API (vmm_map_4k_page / vmm_unmap_4k_page) is
 * exercised by test_fork_user_map.elf + test_user_write_cow.elf —
 * those tests stay green as long as the delegation preserves behavior
 * (the vmm.c refactor is structural, not semantic).
 */
#include "test_framework.h"

#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <memory/vmm.h>
#include <arch/x86_64/pte.h>   /* PAGE_* x86 hardware PTE bits */

/* ── calloc wrap (--wrap=calloc) ────────────────────────────
 * Allocated once per intermediate table.  The test pre-fills the
 * chain outside calloc, so this wrap is only here so vmm.c link
 * resolves.  Returns NULL — any unexpected calloc means a real path
 * is missing the pre-fill (assert via NULL-OOM). */
void *__wrap_calloc(size_t n, size_t size)
{
    (void)n; (void)size;
    return NULL;
}

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
    int  (*m)(uint64_t *, uint64_t, uint64_t, uint64_t) = x86_vmm_map_4k_page;
    int  (*u)(uint64_t *, uint64_t)                    = x86_vmm_unmap_4k_page_with_free;
    int  (*q)(uint64_t *, uint64_t, uint64_t *,
              uint64_t *)                              = x86_vmm_query_4k_page;
    (void)m; (void)u; (void)q;
    assert_true(1);  /* compiles = passes */
}

TEST_FUNC(test_cow_shared_page_does_not_free)
{
    /* COW shared: page_cow_put returns false (refs > 0) → exactly
     * one page_cow_put, ZERO free_4k_page (Task 15 v1 review item 9
     * "single free per branch" — shared = no free). */
    reset_chain();
    g_cow_put_should_return_true = 0;
    pt_tbl[0] = 0x1000UL | PAGE_VALID | PAGE_COW | PAGE_USER;

    int rc = x86_vmm_unmap_4k_page_with_free(pgd_root, TEST_VA_A);
    assert_eq(0, rc);
    assert_eq(1, g_cow_put_calls);
    assert_eq(0, g_free_calls);
    assert_eq(0x1000UL, g_last_phys);
}

TEST_FUNC(test_cow_last_ref_frees_phys)
{
    /* COW last-ref: page_cow_put returns true → exactly one
     * page_cow_put, exactly ONE free_4k_page. */
    reset_chain();
    g_cow_put_should_return_true = 1;
    pt_tbl[1] = 0x2000UL | PAGE_VALID | PAGE_COW | PAGE_USER;

    int rc = x86_vmm_unmap_4k_page_with_free(pgd_root, TEST_VA_B);
    assert_eq(0, rc);
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

    int rc = x86_vmm_unmap_4k_page_with_free(pgd_root, TEST_VA_C);
    assert_eq(0, rc);
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

TEST_LIST_BEGIN
    TEST_ENTRY(test_x86_vmm_backend_api_exists),
    TEST_ENTRY(test_cow_shared_page_does_not_free),
    TEST_ENTRY(test_cow_last_ref_frees_phys),
    TEST_ENTRY(test_plain_page_frees_phys),
    TEST_ENTRY(test_query_returns_pte_state),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed == 0 ? 0 : 1;
}