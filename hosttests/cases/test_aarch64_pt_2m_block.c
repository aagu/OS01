/*
 * hosttests/cases/test_aarch64_pt_2m_block.c — aarch64 M3.3 Task 18
 * RED test: 2 MiB block primitive contract (spec §5.2 / §4.4.5).
 *
 * Five cases per the brief:
 *   1. map + query + unmap 2 MiB block across the brief's legal
 *      combos (5-8 USER/KERNEL × RO/RW Normal, 11-12 USER/KERNEL ×
 *      RO/RW Device).  Each combo produces a block descriptor at
 *      pmd[l2] (V=1, bit1=0, OA bits [39:21], AP/SH/AttrIndx/XN
 *      matching the perm), and unmap returns the prior PA.
 *   2. Alignment check: misaligned VA / misaligned PA / PA >= 1 TiB
 *      each return -EINVAL.
 *   3. L3 already present (4 KiB leaf mapped at the same VA first)
 *      → map_2m_block returns -EEXIST (the L2 slot is occupied by
 *      a valid table descriptor).
 *   4. unmap_2m_block on a slot that holds a 4 KiB table descriptor
 *      → -EINVAL (type mismatch — caller used the wrong API).
 *   5. unmap_2m_block on an empty slot → -ENOENT.
 *
 * Link strategy: same as test_aarch64_pt_locks.c / _software_bits.c —
 * compile the REAL kernel/arch/aarch64/memory/page_table.c against
 * host mocks (aarch64_read_ttbr1 returns 0, alloc_4k_page /
 * free_4k_page use a heap-backed pool with a fail-after counter for
 * the ENOMEM case, vmm_gate_check is a no-op).  is_active_root()
 * returns false on host (mock TTBR1 = 0, host CR3 != test root), so
 * the guarded TLBI path in map_2m_block / unmap_2m_block is never
 * reached — same shape as the other M3.3 RED tests.
 *
 * The test reads the raw descriptor at pmd[l2] via
 * aarch64_pt_read_l2_desc (the test-only helper Task 18 lands in
 * <arch/aarch64/page_table.h>).  This is how the brief's
 * "assert block descriptor (HUGE bit, valid bit, AttrIndx, etc.)"
 * is checked: a direct descriptor read rather than a textual scan.
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

#ifndef OS01_KERNEL_SRC
#error "Define OS01_KERNEL_SRC to the kernel source root (-DOS01_KERNEL_SRC=...)"
#endif

/* ── Mock PMM pool (host heap-backed, mmap'd into the kernel window) ──
 * Mirrors the pattern from test_aarch64_pt_software_bits.c.  Pool size
 * 32 leaves room for the test's repeated walk_to_l2 + retry pattern.
 * Stride 0x1000 mirrors real 4 KiB pages so pa + ARCH_PAGE_OFFSET
 * resolves cleanly through the mmap'd kernel-half window. */
#define MOCK_PA_BASE       0x10000ULL       /* first "physical" PA */
#define MOCK_PA_STRIDE     0x1000ULL        /* 4 KiB */
#define MOCK_POOL_SIZE     32
static uint64_t g_pool_pa[MOCK_POOL_SIZE];
static uint64_t g_next_alloc_idx;
static int      g_free_calls;

static void mock_pool_reset(void)
{
    g_next_alloc_idx = 0;
    g_free_calls = 0;
    for (int i = 0; i < MOCK_POOL_SIZE; i++) g_pool_pa[i] = 0;
}

/* ── Mocks for the symbols page_table.c references ────────────────── */
uint64_t aarch64_read_ttbr1(void) { return 0; }    /* not active */

/* page_table.c calls vmm_gate_check() on every public entry. */
void vmm_gate_check(void) { (void)0; }

uint64_t alloc_4k_page(void)
{
    if (g_next_alloc_idx >= MOCK_POOL_SIZE) return 0;
    uint64_t pa = MOCK_PA_BASE + (uint64_t)g_next_alloc_idx * MOCK_PA_STRIDE;
    g_pool_pa[g_next_alloc_idx] = pa;
    g_next_alloc_idx++;
    return pa;
}

void free_4k_page(uint64_t phys)
{
    (void)phys;
    g_free_calls++;
}

/* ── Test helpers ────────────────────────────────────────────────
 * Each test allocates a fresh root PA from the pool, mmap's a VA
 * window for it, and exercises the primitive.  The root pointer is
 * the high-half direct-map window, which is what production expects.
 *
 * TEST_VA_BASE is 2 MiB aligned (bits [20:0] = 0) so the 2 MiB
 * block API accepts it directly.  Per-test VAs use 2 MiB increments
 * to keep alignment. */
#define TEST_VA_BASE 0xffff800000200000ULL  /* far enough from M1 selftest VA */

static uint64_t *fresh_root_va(uint64_t *out_pa)
{
    uint64_t pa = alloc_4k_page();
    if (pa == 0) return NULL;
    uint64_t *va = (uint64_t *)(uintptr_t)(pa + ARCH_PAGE_OFFSET);
    memset(va, 0, MOCK_PA_STRIDE);
    if (out_pa) *out_pa = pa;
    return va;
}

/* Descriptor bit constants (same values used by page_table.c). */
#define TEST_DESC_VALID UINT64_C(0x001)
#define TEST_DESC_TABLE UINT64_C(0x002)
#define TEST_DESC_AF    UINT64_C(0x400)
#define TEST_DESC_PXN   UINT64_C(0x20000000000000)
#define TEST_DESC_UXN   UINT64_C(0x40000000000000)
#define TEST_DESC_USER  UINT64_C(0x040)   /* AP[1] = EL0 access */
#define TEST_DESC_RO    UINT64_C(0x080)   /* AP[2] = read-only */
#define TEST_DESC_SH_IS UINT64_C(0x300)   /* SH[1:0] = inner-shareable */
#define TEST_DESC_SH_NS UINT64_C(0x000)   /* SH[1:0] = non-shareable */
#define TEST_DESC_ATTR_NORMAL UINT64_C(0x004)
#define TEST_DESC_ATTR_DEVICE UINT64_C(0x000)
#define TEST_BLOCK_OA_MASK    UINT64_C(0xffffffe00000)

/* Forward decl for the spin_init walk_to_l2 helpers we expose for
 * testing — see page_table.c for the contract. */
extern int aarch64_pt_init_locks(void);

/* Helper: extract the AP[2:1] field from a block descriptor (same
 * shape as decode_perm uses for leaves; bit 7 = RO, bit 6 = USER). */
static void decode_block_ap(uint64_t desc, bool *is_user, bool *is_ro)
{
    *is_user = (desc & TEST_DESC_USER) != 0;
    *is_ro   = (desc & TEST_DESC_RO)   != 0;
}

/* ── Tests ────────────────────────────────────────────────────────── */

/* Case 1a: block map + query + unmap, KERNEL_RW Normal (combo 5).
 * Round-trip the brief's VM_KERNEL_RW|VM_HUGE = 0x11 via the
 * primitive directly (no VM_HUGE bit in perm — the perm word is
 * leaf-shaped; the block-vs-leaf decision is the call site, not
 * the perm encoding). */
TEST_FUNC(test_map_query_unmap_block_kernel_rw_normal)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    uint64_t pa   = 0x400000ULL;          /* 4 MiB, 2 MiB aligned */
    uint64_t va   = TEST_VA_BASE;
    uint32_t perm = AARCH64_PT_KERNEL_RW;

    int rc = aarch64_pt_map_2m_block(root, va, pa, perm);
    assert_eq(0, rc);

    /* Read the raw descriptor at pmd[l2].  Must be a valid block:
     *   - V=1, bit1=0 (table bit clear)
     *   - OA bits [39:21] match pa
     *   - AP = 00 (KERNEL_RW)
     *   - SH = inner-shareable, AttrIndx = Normal
     *   - PXN | UXN both set (KERNEL_RW without EXEC → no-execute
     *     for both privilege levels) */
    uint64_t desc = 0;
    rc = aarch64_pt_read_l2_desc(root, va, &desc);
    assert_eq(0, rc);
    assert_true((desc & TEST_DESC_VALID) != 0);
    assert_true((desc & TEST_DESC_TABLE) == 0);  /* block, not table */
    assert_eq(pa, desc & TEST_BLOCK_OA_MASK);
    assert_true((desc & TEST_DESC_AF) != 0);
    assert_true((desc & TEST_DESC_PXN) != 0);
    assert_true((desc & TEST_DESC_UXN) != 0);
    bool is_user, is_ro;
    decode_block_ap(desc, &is_user, &is_ro);
    assert_false(is_user);
    assert_false(is_ro);
    assert_eq(TEST_DESC_SH_IS, desc & TEST_DESC_SH_IS);
    assert_eq(TEST_DESC_ATTR_NORMAL, desc & TEST_DESC_ATTR_NORMAL);

    /* Unmap returns the prior PA. */
    uint64_t got_pa = 0;
    rc = aarch64_pt_unmap_2m_block(root, va, &got_pa);
    assert_eq(0, rc);
    assert_eq(pa, got_pa);

    /* Slot is now empty. */
    desc = 0;
    rc = aarch64_pt_read_l2_desc(root, va, &desc);
    assert_eq(0, rc);
    assert_eq(0, desc);
    assert_eq(0, g_free_calls);   /* backend never frees data pages */
}

/* Case 1b: KERNEL_RO Normal (combo 6).  AP[2] set, USER clear;
 * PXN|UXN both set. */
TEST_FUNC(test_map_block_kernel_ro_normal)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    uint64_t va = TEST_VA_BASE + 0x200000ULL;
    uint64_t pa = 0x600000ULL;
    int rc = aarch64_pt_map_2m_block(root, va, pa, AARCH64_PT_KERNEL_RO);
    assert_eq(0, rc);

    uint64_t desc = 0;
    rc = aarch64_pt_read_l2_desc(root, va, &desc);
    assert_eq(0, rc);
    assert_true((desc & TEST_DESC_VALID) != 0);
    assert_true((desc & TEST_DESC_TABLE) == 0);
    assert_eq(pa, desc & TEST_BLOCK_OA_MASK);
    bool is_user, is_ro;
    decode_block_ap(desc, &is_user, &is_ro);
    assert_false(is_user);
    assert_true(is_ro);
    assert_eq(TEST_DESC_ATTR_NORMAL, desc & TEST_DESC_ATTR_NORMAL);

    rc = aarch64_pt_unmap_2m_block(root, va, NULL);
    assert_eq(0, rc);
}

/* Case 1c: USER_RW Normal (combo 7).  AP = 01. */
TEST_FUNC(test_map_block_user_rw_normal)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    uint64_t va = TEST_VA_BASE + 0x400000ULL;
    uint64_t pa = 0x800000ULL;
    int rc = aarch64_pt_map_2m_block(root, va, pa,
                                     AARCH64_PT_USER_RW | AARCH64_PT_EXEC);
    assert_eq(0, rc);

    uint64_t desc = 0;
    rc = aarch64_pt_read_l2_desc(root, va, &desc);
    assert_eq(0, rc);
    assert_true((desc & TEST_DESC_TABLE) == 0);
    bool is_user, is_ro;
    decode_block_ap(desc, &is_user, &is_ro);
    assert_true(is_user);
    assert_false(is_ro);

    /* USER_RW + EXEC: PXN must be set (kernel never executes user),
     * UXN must be clear (EL0 may execute). */
    assert_true((desc & TEST_DESC_PXN) != 0);
    assert_true((desc & TEST_DESC_UXN) == 0);

    rc = aarch64_pt_unmap_2m_block(root, va, NULL);
    assert_eq(0, rc);
}

/* Case 1d: USER_RO Normal (combo 8).  AP = 11. */
TEST_FUNC(test_map_block_user_ro_normal)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    uint64_t va = TEST_VA_BASE + 0x600000ULL;
    uint64_t pa = 0xa00000ULL;
    int rc = aarch64_pt_map_2m_block(root, va, pa, AARCH64_PT_USER_RO);
    assert_eq(0, rc);

    uint64_t desc = 0;
    rc = aarch64_pt_read_l2_desc(root, va, &desc);
    assert_eq(0, rc);
    assert_true((desc & TEST_DESC_TABLE) == 0);
    bool is_user, is_ro;
    decode_block_ap(desc, &is_user, &is_ro);
    assert_true(is_user);
    assert_true(is_ro);

    rc = aarch64_pt_unmap_2m_block(root, va, NULL);
    assert_eq(0, rc);
}

/* Case 1e: KERNEL_RW Device (combo 11).  NOCACHE forces PXN|UXN,
 * SH = non-shareable, AttrIndx = Device. */
TEST_FUNC(test_map_block_kernel_device)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    uint64_t va = TEST_VA_BASE + 0x800000ULL;
    uint64_t pa = 0x10000000ULL;        /* device window */
    /* DEVICE is a perm-level flag; must be combined with a
     * kernel/user access class.  Use KERNEL_RW + DEVICE — the
     * natural aarch64 NOCACHE combo per vmm_backend.c's vm_to_perm. */
    int rc = aarch64_pt_map_2m_block(root, va, pa,
                                     AARCH64_PT_DEVICE | AARCH64_PT_KERNEL_RW);
    assert_eq(0, rc);

    uint64_t desc = 0;
    rc = aarch64_pt_read_l2_desc(root, va, &desc);
    assert_eq(0, rc);
    assert_true((desc & TEST_DESC_TABLE) == 0);
    assert_true((desc & TEST_DESC_PXN) != 0);
    assert_true((desc & TEST_DESC_UXN) != 0);
    assert_eq(TEST_DESC_SH_NS, desc & TEST_DESC_SH_IS);
    assert_eq(TEST_DESC_ATTR_DEVICE, desc & TEST_DESC_ATTR_NORMAL);

    rc = aarch64_pt_unmap_2m_block(root, va, NULL);
    assert_eq(0, rc);
}

/* Case 1f: USER_RW Device (combo 12). */
TEST_FUNC(test_map_block_user_device)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    uint64_t va = TEST_VA_BASE + 0xa00000ULL;
    uint64_t pa = 0x11000000ULL;
    int rc = aarch64_pt_map_2m_block(root, va, pa,
                                     AARCH64_PT_DEVICE | AARCH64_PT_USER_RW);
    assert_eq(0, rc);

    uint64_t desc = 0;
    rc = aarch64_pt_read_l2_desc(root, va, &desc);
    assert_eq(0, rc);
    bool is_user, is_ro;
    decode_block_ap(desc, &is_user, &is_ro);
    assert_true(is_user);
    assert_false(is_ro);
    /* DEVICE forces PXN | UXN. */
    assert_true((desc & TEST_DESC_PXN) != 0);
    assert_true((desc & TEST_DESC_UXN) != 0);
    assert_eq(TEST_DESC_SH_NS, desc & TEST_DESC_SH_IS);
}

/* Case 2a: misaligned VA → -EINVAL. */
TEST_FUNC(test_map_block_misaligned_va_returns_einval)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    int rc = aarch64_pt_map_2m_block(root, TEST_VA_BASE + 1,
                                     0x400000ULL, AARCH64_PT_KERNEL_RW);
    assert_eq(AARCH64_PT_EINVAL, rc);

    rc = aarch64_pt_map_2m_block(root, TEST_VA_BASE + 0x1000,
                                 0x400000ULL, AARCH64_PT_KERNEL_RW);
    assert_eq(AARCH64_PT_EINVAL, rc);

    rc = aarch64_pt_map_2m_block(root, TEST_VA_BASE + 0x100000,
                                 0x400000ULL, AARCH64_PT_KERNEL_RW);
    assert_eq(AARCH64_PT_EINVAL, rc);
}

/* Case 2b: misaligned PA → -EINVAL. */
TEST_FUNC(test_map_block_misaligned_pa_returns_einval)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    int rc = aarch64_pt_map_2m_block(root, TEST_VA_BASE,
                                     0x400001ULL, AARCH64_PT_KERNEL_RW);
    assert_eq(AARCH64_PT_EINVAL, rc);

    rc = aarch64_pt_map_2m_block(root, TEST_VA_BASE,
                                 0x401000ULL, AARCH64_PT_KERNEL_RW);
    assert_eq(AARCH64_PT_EINVAL, rc);
}

/* Case 2c: PA >= 1 TiB → -EINVAL (IPS=40 limit). */
TEST_FUNC(test_map_block_pa_above_limit_returns_einval)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    int rc = aarch64_pt_map_2m_block(root, TEST_VA_BASE,
                                     UINT64_C(0x10000000000),  /* 1 TiB exactly */
                                     AARCH64_PT_KERNEL_RW);
    assert_eq(AARCH64_PT_EINVAL, rc);

    rc = aarch64_pt_map_2m_block(root, TEST_VA_BASE,
                                 UINT64_C(0x20000000000),
                                 AARCH64_PT_KERNEL_RW);
    assert_eq(AARCH64_PT_EINVAL, rc);
}

/* Case 2d: bad perm (no kernel/user class) → -EINVAL. */
TEST_FUNC(test_map_block_bad_perm_returns_einval)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    /* Pass 0 — neither KERNEL nor USER selected, neither RO nor RW.
     * encode_perm rejects this with EINVAL. */
    int rc = aarch64_pt_map_2m_block(root, TEST_VA_BASE,
                                     0x400000ULL, 0);
    assert_eq(AARCH64_PT_EINVAL, rc);

    /* DEVICE | EXEC — the forbidden combination. */
    rc = aarch64_pt_map_2m_block(root, TEST_VA_BASE,
                                 0x400000ULL,
                                 AARCH64_PT_DEVICE | AARCH64_PT_EXEC |
                                 AARCH64_PT_KERNEL_RW);
    assert_eq(AARCH64_PT_EINVAL, rc);
}

/* Case 3: L3 leaf already mapped → map_2m_block returns -EEXIST.
 *
 * Map a 4 KiB leaf at TEST_VA_BASE first (allocates L0/L1/L2 tables
 * + an L3 page); then try to map a 2 MiB block at the same VA.  The
 * L2 slot now holds a valid table descriptor pointing to the L3
 * page; map_2m_block must reject with -EEXIST. */
TEST_FUNC(test_map_block_with_existing_leaf_returns_eexist)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    /* Step 1: map a 4K leaf at the same VA.  This creates the
     * intermediate table chain + an L3 page (the leaf). */
    int rc = aarch64_pt_map_4k_ext(root, TEST_VA_BASE, 0x5000ULL,
                                   AARCH64_PT_KERNEL_RW, 0);
    assert_eq(0, rc);

    /* Step 2: verify the L2 slot holds a table descriptor (V=1, bit1=1). */
    uint64_t l2_desc = 0;
    rc = aarch64_pt_read_l2_desc(root, TEST_VA_BASE, &l2_desc);
    assert_eq(0, rc);
    assert_true((l2_desc & TEST_DESC_VALID) != 0);
    assert_true((l2_desc & TEST_DESC_TABLE) != 0);

    /* Step 3: now try the 2M block at the same VA — must -EEXIST. */
    rc = aarch64_pt_map_2m_block(root, TEST_VA_BASE, 0x400000ULL,
                                 AARCH64_PT_KERNEL_RW);
    assert_eq(AARCH64_PT_EEXIST, rc);

    /* The original leaf must still be intact.  Read it back. */
    uint64_t qpa = 0;
    uint32_t qperm = 0;
    rc = aarch64_pt_query_4k_ext(root, TEST_VA_BASE, &qpa, &qperm, NULL);
    assert_eq(0, rc);
    assert_eq(0x5000ULL, qpa);
}

/* Case 4: unmap_2m_block on a slot that holds a table descriptor
 * (i.e. a 4 KiB leaf is mapped there) → -EINVAL. */
TEST_FUNC(test_unmap_block_on_leaf_slot_returns_einval)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    int rc = aarch64_pt_map_4k_ext(root, TEST_VA_BASE, 0x5000ULL,
                                   AARCH64_PT_KERNEL_RW, 0);
    assert_eq(0, rc);

    /* Slot is a table descriptor → unmap_2m_block must reject with
     * -EINVAL (caller used the wrong API; leaf unmap is
     * aarch64_pt_unmap_4k_ext). */
    uint64_t got_pa = 0;
    rc = aarch64_pt_unmap_2m_block(root, TEST_VA_BASE, &got_pa);
    assert_eq(AARCH64_PT_EINVAL, rc);

    /* Leaf must still be intact. */
    uint64_t qpa = 0;
    uint32_t qperm = 0;
    rc = aarch64_pt_query_4k_ext(root, TEST_VA_BASE, &qpa, &qperm, NULL);
    assert_eq(0, rc);
    assert_eq(0x5000ULL, qpa);
}

/* Case 5: unmap_2m_block on an empty slot → -ENOENT. */
TEST_FUNC(test_unmap_block_on_empty_slot_returns_enoent)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    uint64_t got_pa = 0xdeadbeefULL;
    int rc = aarch64_pt_unmap_2m_block(root, TEST_VA_BASE, &got_pa);
    assert_eq(AARCH64_PT_ENOENT, rc);
    /* *pa_out is NOT written on ENOENT (caller can't act on a PA
     * that doesn't exist).  Keep the sentinel value as a witness. */
    assert_eq(0xdeadbeefULL, got_pa);
}

/* Case 6 (review round 1 — sanity pin): map then unmap, then map
 * again at the same VA.  Second map must succeed (-EEXIST only
 * fires when the slot is occupied). */
TEST_FUNC(test_map_block_reuse_after_unmap)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    int rc = aarch64_pt_map_2m_block(root, TEST_VA_BASE, 0x400000ULL,
                                     AARCH64_PT_KERNEL_RW);
    assert_eq(0, rc);

    uint64_t pa_out = 0;
    rc = aarch64_pt_unmap_2m_block(root, TEST_VA_BASE, &pa_out);
    assert_eq(0, rc);
    assert_eq(0x400000ULL, pa_out);

    /* Slot is empty → a fresh map at the same VA succeeds. */
    rc = aarch64_pt_map_2m_block(root, TEST_VA_BASE, 0x800000ULL,
                                 AARCH64_PT_KERNEL_RW);
    assert_eq(0, rc);

    uint64_t desc = 0;
    rc = aarch64_pt_read_l2_desc(root, TEST_VA_BASE, &desc);
    assert_eq(0, rc);
    assert_eq(0x800000ULL, desc & TEST_BLOCK_OA_MASK);
}

/* Case 7: split_block_2m stub returns -EPERM (Task 21 implements). */
TEST_FUNC(test_split_block_2m_stub_returns_eperm)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    int rc = aarch64_pt_split_block_2m(root, TEST_VA_BASE);
    assert_eq(AARCH64_PT_EPERM, rc);
}

/* Case 8: read_l2_desc on a VA whose L0 slot is empty → -ENOENT
 * (no intermediates allocated).  Tests the helper's error path
 * which the production happy path doesn't exercise. */
TEST_FUNC(test_read_l2_desc_missing_intermediate_returns_enoent)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    uint64_t desc = 0xdeadbeefULL;
    int rc = aarch64_pt_read_l2_desc(root, TEST_VA_BASE, &desc);
    assert_eq(AARCH64_PT_ENOENT, rc);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_map_query_unmap_block_kernel_rw_normal),
    TEST_ENTRY(test_map_block_kernel_ro_normal),
    TEST_ENTRY(test_map_block_user_rw_normal),
    TEST_ENTRY(test_map_block_user_ro_normal),
    TEST_ENTRY(test_map_block_kernel_device),
    TEST_ENTRY(test_map_block_user_device),
    TEST_ENTRY(test_map_block_misaligned_va_returns_einval),
    TEST_ENTRY(test_map_block_misaligned_pa_returns_einval),
    TEST_ENTRY(test_map_block_pa_above_limit_returns_einval),
    TEST_ENTRY(test_map_block_bad_perm_returns_einval),
    TEST_ENTRY(test_map_block_with_existing_leaf_returns_eexist),
    TEST_ENTRY(test_unmap_block_on_leaf_slot_returns_einval),
    TEST_ENTRY(test_unmap_block_on_empty_slot_returns_enoent),
    TEST_ENTRY(test_map_block_reuse_after_unmap),
    TEST_ENTRY(test_split_block_2m_stub_returns_eperm),
    TEST_ENTRY(test_read_l2_desc_missing_intermediate_returns_enoent),
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
