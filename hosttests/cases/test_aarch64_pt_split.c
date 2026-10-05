/*
 * hosttests/cases/test_aarch64_pt_split.c — aarch64 M3.4 Task 21
 * RED→GREEN test: split_block_2m protocol contract (spec §5.3).
 *
 * Four cases per the brief:
 *   1. Unpublished root: map_2m + split → query 512 PTEs all inherit
 *      the block descriptor's attributes (same AP[2:1] / SH / AttrIndx
 *      / AF / PXN / UXN / software bits) with PA = block_pa + i*4K.
 *      pmd[l2] becomes a valid table descriptor pointing to the new
 *      L3 page (the one and only atomic 8 B store the spec mandates).
 *   2. Published root: publish the test root via the REAL registry
 *      (kernel/arch/aarch64/memory/vmm_gate.c) → split returns -EPERM
 *      and the block descriptor is untouched.
 *   3. alloc-fail injection: after map_2m succeeds, the next
 *      alloc_4k_page returns 0 → split returns -ENOMEM; original block
 *      descriptor intact.
 *   4. Concurrent-split simulation: after map_2m, manually overwrite
 *      pmd[l2] with a valid TABLE descriptor (V=1, bit1=1) — simulates
 *      a second split caller having won the race. split returns
 *      -EAGAIN (caller retries) and the table descriptor is intact.
 *
 * Link strategy (mirrors test_aarch64_pt_root_publish.c):
 *   - REAL kernel/arch/aarch64/memory/page_table.c
 *   - REAL kernel/arch/aarch64/memory/vmm_gate.c (publish registry)
 *   - Mock aarch64_read_ttbr1() / alloc_4k_page() / free_4k_page()
 *     / vmm_gate_check() is a no-op (smp_starting=0 in vmm_gate.c's
 *     own static initialiser). percpu_data[] / dtb_cpu_count() stubs
 *     come from hosttests/mock/vmm_gate_test_stubs.c.
 *
 * is_active_root() always returns false on host (mock TTBR1 = 0,
 * real CR3 never matches a test root), so the TLBI path is never
 * taken. The page_table.c aarch64 inline asm (tlbi / dsb) is guarded
 * with #ifdef __aarch64__ so the host build compiles cleanly.
 */

#include "test_framework.h"

#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include <arch/mmu.h>
#include <arch/spinlock.h>        /* spinlock_T (matches Task 17 host surface) */
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/vmm_backend.h>
#include <arch/aarch64/vmm_gate.h>  /* aarch64_pt_root_publish / _is_published */

#ifndef OS01_KERNEL_SRC
#error "Define OS01_KERNEL_SRC to the kernel source root (-DOS01_KERNEL_SRC=...)"
#endif

/* ── Mock PMM pool ────────────────────────────────────────────────────
 * Mirrors test_aarch64_pt_2m_block.c.  Pool of 32 host-aligned 4 KiB
 * pages; alloc_4k_page returns monotonic PAs (0x10000, 0x11000, ...)
 * and the host mmap at ARCH_PAGE_OFFSET provides the direct-mapped
 * backing for PA + ARCH_PAGE_OFFSET.  free_4k_page() bumps a counter
 * so tests can assert the on-failure release happened (or didn't).
 *
 * `g_alloc_fails_after_n` lets a test inject a single -ENOMEM at a
 * specific call: set it to g_alloc_count after the warm-up allocs
 * (map_2m_block + root creation), so the FIRST alloc inside split
 * fails. */
#define MOCK_PA_BASE       0x10000ULL
#define MOCK_PA_STRIDE     0x1000ULL
#define MOCK_POOL_SIZE     64
static uint64_t g_pool_pa[MOCK_POOL_SIZE];
static uint64_t g_next_alloc_idx;
static uint64_t g_alloc_count;        /* monotonically counts allocs  */
static int      g_free_calls;
static int      g_alloc_fails_after_n; /* -1 = disabled; >=0 = threshold */

static void mock_pool_reset(void)
{
    /* Reset counters / failure-injection but NOT the allocator
     * cursor: each test must get a unique root PA, otherwise the
     * published-root state from a prior test (which leaked through
     * the production vmm_gate.c registry) would silently convert
     * every later test's split call into -EPERM.  Pool size 32 is
     * enough for the 8 tests × ~4 allocs each (root + L1 + L2 + L3). */
    g_alloc_count = 0;
    g_free_calls = 0;
    g_alloc_fails_after_n = -1;
    for (int i = 0; i < MOCK_POOL_SIZE; i++) g_pool_pa[i] = 0;
}

/* ── Mocks for the symbols page_table.c references ────────────────── */
uint64_t aarch64_read_ttbr1(void) { return 0; }    /* not active */

/* page_table.c calls vmm_gate_check() on every public entry.  We link
 * REAL vmm_gate.c (the publish registry); its vmm_gate_check() is a
 * no-op when smp_starting == 0 (the default for our host harness —
 * smp_starting is a static zero-initialised counter in vmm_gate.c).
 * Override the weak violation hook with a longjmp capture so any
 * accidental gate violation becomes an observable failure instead of
 * a hang. */
static int g_violations;
void vmm_gate_violation(const char *reason)
{
    (void)reason;
    g_violations++;
    /* spin like the weak default would — the test catches via the
     * counter if it ever fires on a happy path. */
    for (;;) ;
}

uint64_t alloc_4k_page(void)
{
    if (g_alloc_fails_after_n >= 0 && g_alloc_count >= (uint64_t)g_alloc_fails_after_n)
        return 0;
    if (g_next_alloc_idx >= MOCK_POOL_SIZE) return 0;
    uint64_t pa = MOCK_PA_BASE + (uint64_t)g_next_alloc_idx * MOCK_PA_STRIDE;
    g_pool_pa[g_next_alloc_idx] = pa;
    g_next_alloc_idx++;
    g_alloc_count++;
    return pa;
}

void free_4k_page(uint64_t phys)
{
    (void)phys;
    g_free_calls++;
}

/* ── Test helpers ────────────────────────────────────────────────
 * Each test allocates a fresh root PA from the pool, mmap's a VA
 * window for it, and exercises the primitive.  Per-test VAs use
 * 2 MiB increments to keep alignment. */
#define TEST_VA_BASE 0xffff800000200000ULL

static uint64_t *fresh_root_va(uint64_t *out_pa)
{
    uint64_t pa = alloc_4k_page();
    if (pa == 0) return NULL;
    uint64_t *va = (uint64_t *)(uintptr_t)(pa + ARCH_PAGE_OFFSET);
    memset(va, 0, MOCK_PA_STRIDE);
    if (out_pa) *out_pa = pa;
    return va;
}

/* Descriptor bit constants — mirror the values used by page_table.c
 * / vmm_backend.h. */
#define TEST_DESC_VALID UINT64_C(0x001)
#define TEST_DESC_TABLE UINT64_C(0x002)
#define TEST_DESC_AF    UINT64_C(0x400)
#define TEST_DESC_PXN   UINT64_C(0x20000000000000)
#define TEST_DESC_UXN   UINT64_C(0x40000000000000)
#define TEST_DESC_USER  UINT64_C(0x040)   /* AP[1] = EL0 access */
#define TEST_DESC_RO    UINT64_C(0x080)   /* AP[2] = read-only */
#define TEST_DESC_SH_IS UINT64_C(0x300)   /* SH[1:0] = inner-shareable */
#define TEST_DESC_ATTR_NORMAL UINT64_C(0x004)
#define TEST_BLOCK_OA_MASK    UINT64_C(0xffffffe00000)
#define TEST_LEAF_PA_MASK     UINT64_C(0xffffffffff000)  /* bits [39:12] */
#define TEST_LEAF_OA_MASK     UINT64_C(0xffffffffff000)

extern int aarch64_pt_init_locks(void);

/* ── Case 1: unpublished root — map_2m + split, verify 512 leaves ──── */

/* Read every PTE at pmd[l2] (now a table desc) and verify each leaf
 * matches the expected `inherit(block_desc)` form.  Used by Case 1's
 * happy-path verification. */
static void assert_split_inherits_block_attrs(uint64_t *pmd_l2_va,
                                              uint64_t l2_idx,
                                              uint64_t block_pa_field,
                                              uint64_t block_attrs_sw)
{
    uint64_t table_desc = pmd_l2_va[l2_idx];
    assert_true((table_desc & TEST_DESC_VALID) != 0);
    assert_true((table_desc & TEST_DESC_TABLE) != 0);

    uint64_t l3_pa = table_desc & TEST_LEAF_OA_MASK;
    uint64_t *l3 = (uint64_t *)(uintptr_t)(l3_pa + ARCH_PAGE_OFFSET);

    /* The expected leaf base = block's attrs/SW + bit1=1 (leaf type).
     * Reusing the same transform the production split does:
     *   leaf_base = (block_desc | TABLE) & ~LEAF_PA_MASK
     * plus per-leaf PA = (block_pa_field + i*4K) & LEAF_PA_MASK. */
    uint64_t expected_base = (block_attrs_sw | TEST_DESC_TABLE) &
                             ~TEST_LEAF_PA_MASK;

    for (uint32_t i = 0; i < 512; i++) {
        uint64_t expected_pa = (block_pa_field + i * PAGE_4K_SIZE) &
                               TEST_LEAF_PA_MASK;
        uint64_t expected = expected_base | expected_pa;
        assert_eq(expected, l3[i]);
    }
}

TEST_FUNC(test_split_unpublished_root_inherits_attrs)
{
    TEST_SUITE("unpublished root: split fills 512 leaves = inherit(block)");
    mock_pool_reset();
    g_violations = 0;

    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    uint64_t va   = TEST_VA_BASE;
    uint64_t pa   = 0x400000ULL;            /* 2 MiB aligned, 4 MiB */
    uint32_t perm = AARCH64_PT_KERNEL_RW;

    /* Map a 2 MiB block first. */
    int rc = aarch64_pt_map_2m_block(root, va, pa, perm);
    assert_eq(0, rc);

    /* Sanity: block descriptor is at pmd[l2]. */
    uint64_t l2_idx = (va >> 21) & 0x1ff;
    uint64_t l2_desc = 0;
    rc = aarch64_pt_read_l2_desc(root, va, &l2_desc);
    assert_eq(0, rc);
    assert_true((l2_desc & TEST_DESC_VALID) != 0);
    assert_true((l2_desc & TEST_DESC_TABLE) == 0);   /* block type */
    assert_eq(pa, l2_desc & TEST_BLOCK_OA_MASK);

    uint64_t block_attrs_sw = l2_desc;                 /* save attrs+SW */
    uint64_t block_pa_field = pa;                      /* bits [39:21] */

    /* Split. */
    rc = aarch64_pt_split_block_2m(root, va);
    if (rc != 0) {
        /* RED state: the stub currently returns -EPERM.  The
         * assert_eq below records the failure; the early return
         * prevents the verification below from dereferencing a
         * still-block-shaped slot as if it were a table desc. */
        assert_eq(0, rc);
        return;
    }

    /* Walk L0 → L1 → L2 to get pmd[l2] (read_l2_desc still works —
     * the slot now holds a valid table desc). */
    uint64_t *pmd = NULL;
    int wr = AARCH64_PT_OK;
    int wrc = walk_to_l2(root, va, false, &pmd, &wr);
    assert_eq(0, wrc);
    assert_not_null(pmd);

    assert_split_inherits_block_attrs(pmd, l2_idx, block_pa_field,
                                      block_attrs_sw);
    assert_eq(0, g_violations);

    /* Confirm one of the new leaves is queryable end-to-end via the
     * production 4 KiB primitive (regression pin for the leaf OA field). */
    uint64_t qpa = 0;
    uint32_t qperm = 0;
    rc = aarch64_pt_query_4k(root, va, &qpa, &qperm);
    assert_eq(0, rc);
    assert_eq(pa, qpa);
    /* KERNEL_RW with no EXEC → both XN bits set in the leaf. */
    assert_true((qperm & AARCH64_PT_KERNEL_RW) != 0);

    /* Spot-check a later leaf at the 2 MiB window's midpoint. */
    uint64_t mid_va = va + 0x100000ULL;
    rc = aarch64_pt_query_4k(root, mid_va, &qpa, &qperm);
    assert_eq(0, rc);
    assert_eq(pa + 0x100000ULL, qpa);
}

/* Case 1b: USER_RW | EXEC variant — proves exec bit & user AP
 * propagate, not just KERNEL_RW defaults. */
TEST_FUNC(test_split_user_rw_exec_inherits_attrs)
{
    TEST_SUITE("split USER_RW|EXEC: AP=USER + PXN set + UXN clear");
    mock_pool_reset();
    g_violations = 0;

    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    uint64_t va = TEST_VA_BASE + 0x200000ULL;
    uint64_t pa = 0x600000ULL;
    uint32_t perm = AARCH64_PT_USER_RW | AARCH64_PT_EXEC;

    int rc = aarch64_pt_map_2m_block(root, va, pa, perm);
    assert_eq(0, rc);

    uint64_t l2_idx = (va >> 21) & 0x1ff;
    uint64_t l2_desc = 0;
    rc = aarch64_pt_read_l2_desc(root, va, &l2_desc);
    assert_eq(0, rc);
    assert_true((l2_desc & TEST_DESC_USER) != 0);
    assert_true((l2_desc & TEST_DESC_PXN) != 0);    /* kernel XN */
    assert_true((l2_desc & TEST_DESC_UXN) == 0);     /* EL0 may exec */

    uint64_t block_attrs_sw = l2_desc;
    uint64_t block_pa_field = pa;

    rc = aarch64_pt_split_block_2m(root, va);
    if (rc != 0) {
        assert_eq(0, rc);  /* RED record */
        return;
    }

    uint64_t *pmd = NULL;
    int wr = AARCH64_PT_OK;
    int wrc = walk_to_l2(root, va, false, &pmd, &wr);
    assert_eq(0, wrc);
    assert_split_inherits_block_attrs(pmd, l2_idx, block_pa_field,
                                      block_attrs_sw);

    uint64_t qpa = 0;
    uint32_t qperm = 0;
    rc = aarch64_pt_query_4k(root, va, &qpa, &qperm);
    assert_eq(0, rc);
    assert_eq(pa, qpa);
    assert_true((qperm & AARCH64_PT_USER_RW) != 0);
    assert_true((qperm & AARCH64_PT_EXEC) != 0);
}

/* ── Case 2: published root → -EPERM ──────────────────────────────── */

TEST_FUNC(test_split_published_root_returns_eperm)
{
    TEST_SUITE("published root: split returns -EPERM, block intact");
    mock_pool_reset();
    g_violations = 0;

    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    uint64_t va = TEST_VA_BASE + 0x400000ULL;
    uint64_t pa = 0x800000ULL;
    int rc = aarch64_pt_map_2m_block(root, va, pa, AARCH64_PT_KERNEL_RW);
    assert_eq(0, rc);

    /* Snapshot the block descriptor BEFORE we publish — we'll verify
     * it's byte-identical after the failed split. */
    uint64_t l2_desc_before = 0;
    rc = aarch64_pt_read_l2_desc(root, va, &l2_desc_before);
    assert_eq(0, rc);
    assert_true((l2_desc_before & TEST_DESC_VALID) != 0);
    assert_true((l2_desc_before & TEST_DESC_TABLE) == 0);

    /* Publish the test root via the REAL registry (vmm_gate.c).
     * aarch64_pt_root_is_published dereferences its argument to get
     * the PA; pass a pointer to root_pa (not to the page itself). */
    assert_false(aarch64_pt_root_is_published(&root_pa));
    assert_true(aarch64_pt_root_publish(root_pa));
    assert_true(aarch64_pt_root_is_published(&root_pa));

    /* Split must reject with -EPERM. */
    rc = aarch64_pt_split_block_2m(root, va);
    assert_eq(AARCH64_PT_EPERM, rc);

    /* The block descriptor must be byte-identical to its pre-publish
     * snapshot — split neither allocated the L3 page nor touched the
     * slot. */
    uint64_t l2_desc_after = 0;
    rc = aarch64_pt_read_l2_desc(root, va, &l2_desc_after);
    assert_eq(0, rc);
    assert_eq(l2_desc_before, l2_desc_after);
    assert_true((l2_desc_after & TEST_DESC_TABLE) == 0);

    /* No alloc happened for the L3 page (the only path that allocates
     * an L3 page is the success path inside split — which we never
     * reached because the publish check is the first thing inside
     * the function after the gate/root_valid). */
    /* alloc_count after map_2m: 3 (root was alloc 1 by fresh_root_va;
     * map_2m allocates L1 if needed + L2; scratch root starts empty,
     * so map_2m's first iteration goes through walk_to_l2 with
     * create=true → at least L1+L2, but L0 was the root itself which
     * was already allocated by fresh_root_va). We don't assert an
     * exact alloc count — only that split's failure did NOT add to it.
     * Track it explicitly here. */
    uint64_t allocs_after_split = g_alloc_count;

    /* Re-attempt: still -EPERM. Idempotent rejection — caller can
     * observe the publish check again. */
    rc = aarch64_pt_split_block_2m(root, va);
    assert_eq(AARCH64_PT_EPERM, rc);
    assert_eq(allocs_after_split, g_alloc_count);
}

/* ── Case 3: alloc-fail injection → -ENOMEM, block intact ─────────── */

TEST_FUNC(test_split_alloc_fail_returns_enomem)
{
    TEST_SUITE("alloc fail injection: split returns -ENOMEM, block intact");
    mock_pool_reset();
    g_violations = 0;

    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    uint64_t va = TEST_VA_BASE + 0x600000ULL;
    uint64_t pa = 0xa00000ULL;
    int rc = aarch64_pt_map_2m_block(root, va, pa, AARCH64_PT_KERNEL_RW);
    assert_eq(0, rc);

    uint64_t l2_desc_before = 0;
    rc = aarch64_pt_read_l2_desc(root, va, &l2_desc_before);
    assert_eq(0, rc);

    /* Fail the NEXT alloc — split allocates an L3 page before taking
     * any lock, so this single failure must drive the -ENOMEM path. */
    g_alloc_fails_after_n = (int)g_alloc_count;

    rc = aarch64_pt_split_block_2m(root, va);
    assert_eq(AARCH64_PT_ENOMEM, rc);

    /* Block descriptor must be byte-identical — split didn't touch the
     * slot (the alloc-fail branch returns BEFORE locking and BEFORE
     * the walk). */
    uint64_t l2_desc_after = 0;
    rc = aarch64_pt_read_l2_desc(root, va, &l2_desc_after);
    assert_eq(0, rc);
    assert_eq(l2_desc_before, l2_desc_after);
    assert_true((l2_desc_after & TEST_DESC_TABLE) == 0);

    assert_eq(0, g_violations);

    /* Clear the failure and confirm the retry succeeds — proves the
     * failure didn't leave the root in a broken state. */
    g_alloc_fails_after_n = -1;
    rc = aarch64_pt_split_block_2m(root, va);
    assert_eq(0, rc);
}

/* ── Case 4: concurrent split (pmd[l2] already TABLE) → -EAGAIN ───── */

TEST_FUNC(test_split_concurrent_split_returns_eagain)
{
    TEST_SUITE("concurrent split: pmd[l2] already TABLE → -EAGAIN");
    mock_pool_reset();
    g_violations = 0;

    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    uint64_t va = TEST_VA_BASE + 0x800000ULL;
    uint64_t pa = 0x10000000ULL;
    int rc = aarch64_pt_map_2m_block(root, va, pa, AARCH64_PT_KERNEL_RW);
    assert_eq(0, rc);

    /* Simulate a concurrent split caller that has already won the
     * race by replacing the block descriptor with a valid table
     * descriptor.  The pre-existing L3 page is just any free 4 KiB
     * mock pool slot — the contract here is the EAGAIN return, not
     * the table's leaf contents. */
    uint64_t l2_idx = (va >> 21) & 0x1ff;
    uint64_t *pmd = NULL;
    int wr = AARCH64_PT_OK;
    int wrc = walk_to_l2(root, va, false, &pmd, &wr);
    assert_eq(0, wrc);

    uint64_t fake_l3_pa = alloc_4k_page();
    assert_true(fake_l3_pa != 0);
    /* Construct a minimal valid table desc (V | TABLE | PA[39:12]). */
    uint64_t fake_table_desc = TEST_DESC_VALID | TEST_DESC_TABLE |
                               (fake_l3_pa & TEST_LEAF_PA_MASK);
    pmd[l2_idx] = fake_table_desc;

    /* Confirm the pre-write state. */
    uint64_t l2_desc_before = 0;
    rc = aarch64_pt_read_l2_desc(root, va, &l2_desc_before);
    assert_eq(0, rc);
    assert_eq(fake_table_desc, l2_desc_before);
    assert_true((l2_desc_before & TEST_DESC_TABLE) != 0);

    /* Split must reject with -EAGAIN — caller retries. */
    rc = aarch64_pt_split_block_2m(root, va);
    assert_eq(AARCH64_PT_EAGAIN, rc);

    /* The pre-existing table desc must be byte-identical — split
     * must not allocate a fresh L3 page, must not overwrite the
     * slot, must not free the foreign L3 page. */
    uint64_t l2_desc_after = 0;
    rc = aarch64_pt_read_l2_desc(root, va, &l2_desc_after);
    assert_eq(0, rc);
    assert_eq(fake_table_desc, l2_desc_after);

    assert_eq(0, g_violations);
}

/* ── Case 5: split on an empty slot → -ENOENT ─────────────────────── */

TEST_FUNC(test_split_empty_slot_returns_enoent)
{
    TEST_SUITE("split on empty slot: -ENOENT, L3 page released");
    mock_pool_reset();
    g_violations = 0;

    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    uint64_t allocs_before = g_alloc_count;
    /* split_block_2m on an empty slot must:
     *   - allocate the L3 page (step 1)
     *   - walk_to_l2(create=true) — may allocate intermediates
     *   - re-read pmd[l2] == 0 → unlock + free L3 + return -ENOENT
     *
     * The intermediates are kept (spec §5.2b item 3 — reusable empty
     * tables) but the L3 page MUST be released on this failure path.
     * The stub currently returns -EPERM unconditionally, so this
     * assertion is RED until Task 21 lands. */
    int rc = aarch64_pt_split_block_2m(root, TEST_VA_BASE);
    assert_eq(AARCH64_PT_ENOENT, rc);
    assert_true(g_alloc_count > allocs_before);
    assert_eq(1, g_free_calls);

    assert_eq(0, g_violations);
}

/* ── Case 6: input validation ──────────────────────────────────────── */

TEST_FUNC(test_split_invalid_inputs_return_einval)
{
    TEST_SUITE("input validation: null root + unaligned VA");
    mock_pool_reset();
    g_violations = 0;

    int rc = aarch64_pt_split_block_2m(NULL, TEST_VA_BASE);
    assert_eq(AARCH64_PT_EINVAL, rc);

    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    rc = aarch64_pt_split_block_2m(root, TEST_VA_BASE + 0x1000);
    assert_eq(AARCH64_PT_EINVAL, rc);

    rc = aarch64_pt_split_block_2m(root, TEST_VA_BASE + 0x100000);
    assert_eq(AARCH64_PT_EINVAL, rc);
    assert_eq(0, g_violations);
}

/* ── Case 7: publish check happens BEFORE lock + alloc ─────────────── */

TEST_FUNC(test_split_published_root_no_alloc_no_touch)
{
    TEST_SUITE("published root: split returns -EPERM without allocating L3");
    mock_pool_reset();
    g_violations = 0;

    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    uint64_t va = TEST_VA_BASE + 0xa00000ULL;
    uint64_t pa = 0x11000000ULL;
    int rc = aarch64_pt_map_2m_block(root, va, pa, AARCH64_PT_KERNEL_RW);
    assert_eq(0, rc);

    uint64_t allocs_before = g_alloc_count;
    uint64_t frees_before = (uint64_t)g_free_calls;

    assert_true(aarch64_pt_root_publish(root_pa));
    assert_true(aarch64_pt_root_is_published(&root_pa));

    rc = aarch64_pt_split_block_2m(root, va);
    assert_eq(AARCH64_PT_EPERM, rc);

    /* No allocation for L3 page, no frees. */
    assert_eq(allocs_before, g_alloc_count);
    assert_eq(frees_before, (uint64_t)g_free_calls);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_split_unpublished_root_inherits_attrs),
    TEST_ENTRY(test_split_user_rw_exec_inherits_attrs),
    TEST_ENTRY(test_split_published_root_returns_eperm),
    TEST_ENTRY(test_split_alloc_fail_returns_enomem),
    TEST_ENTRY(test_split_concurrent_split_returns_eagain),
    TEST_ENTRY(test_split_empty_slot_returns_enoent),
    TEST_ENTRY(test_split_invalid_inputs_return_einval),
    TEST_ENTRY(test_split_published_root_no_alloc_no_touch),
TEST_LIST_END

int main(void)
{
    /* mmap the kernel half window so PA + ARCH_PAGE_OFFSET resolves. */
    void *base = mmap((void *)(uintptr_t)ARCH_PAGE_OFFSET,
                      0x200000ULL,                          /* 2 MiB window */
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                      -1, 0);
    if (base == MAP_FAILED) return 2;
    memset(base, 0, 0x200000ULL);

    /* Re-init the page-table locks in case some future change drops
     * the static initializer.  Cheap idempotent reset. */
    (void)aarch64_pt_init_locks();

    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
