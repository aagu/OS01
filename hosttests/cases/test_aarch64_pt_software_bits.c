/*
 * hosttests/cases/test_aarch64_pt_software_bits.c — aarch64 M3.3 Task 16
 * Step 1 RED test: software-bit round-trip + PROT_NONE stash + block
 * encoding contract (spec §4.2 / §4.4.3 / §5.1).
 *
 * Six cases per the brief:
 *   1. map + sw=PROTNONE → query returns -EPROT_NONE 且 phys_out 有效
 *   2. unmap 带 PROTNONE → 返回 phys 且不释放（ownership 契约 §4.4.4）
 *   3. update 切到 VALID → query 正常
 *   4. update 切回 PROTNONE（保留 PA） → query -EPROT_NONE
 *   5. 拒绝组合 sw=PROTNONE|COW → -EINVAL
 *   6. encode_block_desc 输出 bit1=0、bit55/56 正确
 *
 * The brief allows tests that "compile the primitive logic into a
 * testable form" — page_table.c has inline aarch64 asm (tlbi / dsb) that
 * prevents host cross-compilation.  The two asm ops in page_table.c are
 * guarded with #ifdef __aarch64__ (their #else branch is a no-op for the
 * host); arch_get_page_table() in <arch/mmu.h> is already #elif-guarded
 * for __aarch64__ vs __x86_64__.  We link the REAL page_table.c against
 * mocks for aarch64_read_ttbr1() / alloc_4k_page() / free_4k_page() and
 * drive the production primitives directly.  is_active_root() returns
 * false on host (mock TTBR1 != test root, real CR3 != test root) so the
 * guarded TLBI path is never reached.
 *
 * Heap-backed mock pool: 16 host-aligned 4 KiB slots live in a static
 * array; alloc_4k_page returns monotonic PAs (0x1000, 0x2000, ...),
 * direct-mapped via the host's mmap of the kernel half window so the
 * production code's pa + ARCH_PAGE_OFFSET trick resolves cleanly.
 */
#include "test_framework.h"

#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>

#include <arch/mmu.h>
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/vmm_backend.h>

/* ── Mock PMM pool (host heap-backed, mmap'd into the kernel window) ──
 * page_table.c treats PA + ARCH_PAGE_OFFSET as a valid VA.  Map a
 * scratch region at ARCH_PAGE_OFFSET and slot the 16 mock 4 KiB
 * "physical" frames at fixed offsets inside it.  Each pool slot also
 * serves as the direct-mapped PTE table page that the production
 * walk_to_l3 / walk_to_l2 write into. */
#define MOCK_PA_BASE       0x10000ULL       /* first "physical" PA */
#define MOCK_PA_STRIDE     0x1000ULL        /* 4 KiB */
#define MOCK_POOL_SIZE     16
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

/* page_table.c calls vmm_gate_check() on every public entry.  The
 * production vmm_gate.c is not linked in this test (would pull in
 * percpu_data[] / dtb_cpu_count() mocks), so we stub it directly.
 * The stub is a no-op — pre-SMP gate is implicitly satisfied on
 * the single-host-thread of the test runner. */
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

/* ── Test VA bookkeeping ────────────────────────────────────────────
 * Each test allocates a fresh root PA from the pool, mmap's a VA
 * window for it, and exercises the primitive.  The root pointer is
 * the high-half direct-map window, which is what production expects. */
#define TEST_VA_BASE 0xffff800000100000ULL  /* far enough from M1 selftest VA */

static uint64_t *fresh_root_va(uint64_t *out_pa)
{
    uint64_t pa = alloc_4k_page();
    if (pa == 0) return NULL;
    uint64_t *va = (uint64_t *)(uintptr_t)(pa + ARCH_PAGE_OFFSET);
    memset(va, 0, MOCK_PA_STRIDE);
    if (out_pa) *out_pa = pa;
    return va;
}

/* ── Tests ────────────────────────────────────────────────────────── */

TEST_FUNC(test_map_protnone_query_returns_e_prot_none_with_pa)
{
    /* Case 1: map with software_bits=PROTNONE → query returns
     * -EPROT_NONE with valid phys_out.  The descriptor must have
     * VALID cleared but the PROTNONE software bit set. */
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    uint64_t data_pa = 0x5000ULL;       /* arbitrary but page-aligned */
    int rc = aarch64_pt_map_4k_ext(root, TEST_VA_BASE, data_pa,
                                   AARCH64_PT_KERNEL_RW,
                                   AARCH64_PT_SOFTWARE_PROTNONE);
    assert_eq(0, rc);

    uint64_t got_pa = 0;
    uint32_t got_perm = 0;
    uint64_t got_sw = 0;
    rc = aarch64_pt_query_4k_ext(root, TEST_VA_BASE, &got_pa, &got_perm, &got_sw);
    assert_eq(AARCH64_PT_EPROT_NONE, rc);
    assert_eq(data_pa, got_pa);
    /* sw bit MUST be present in the out param so callers can act on it */
    assert_true((got_sw & AARCH64_PT_SOFTWARE_PROTNONE) != 0);
    assert_eq(0, g_free_calls);
}

TEST_FUNC(test_unmap_protnone_returns_pa_does_not_free)
{
    /* Case 2: unmap a PROT_NONE-stashed descriptor returns the PA but
     * does NOT free it (spec §4.4.4: backend never frees data pages). */
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    uint64_t data_pa = 0x6000ULL;
    int rc = aarch64_pt_map_4k_ext(root, TEST_VA_BASE, data_pa,
                                   AARCH64_PT_KERNEL_RW,
                                   AARCH64_PT_SOFTWARE_PROTNONE);
    assert_eq(0, rc);

    uint64_t got_pa = 0;
    uint32_t got_perm = 0;
    uint64_t got_sw = 0;
    rc = aarch64_pt_unmap_4k_ext(root, TEST_VA_BASE, &got_pa, &got_perm, &got_sw);
    assert_eq(AARCH64_PT_EPROT_NONE, rc);
    assert_eq(data_pa, got_pa);
    assert_eq(0, g_free_calls);

    /* After unmap the slot is genuinely empty — a follow-up query
     * returns -ENOENT, not -EPROT_NONE. */
    rc = aarch64_pt_query_4k_ext(root, TEST_VA_BASE, &got_pa, &got_perm, &got_sw);
    assert_eq(-3 /* AARCH64_PT_ENOENT */, rc);
}

TEST_FUNC(test_update_to_valid_recovers_query)
{
    /* Case 3: starting from a PROT_NONE stash, update to a plain
     * VALID KERNEL_RW mapping → query returns 0 with the same PA
     * and no PROTNONE bit. */
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    uint64_t data_pa = 0x7000ULL;
    int rc = aarch64_pt_map_4k_ext(root, TEST_VA_BASE, data_pa,
                                   AARCH64_PT_KERNEL_RO,
                                   AARCH64_PT_SOFTWARE_PROTNONE);
    assert_eq(0, rc);

    uint64_t old_pa = 0;
    uint32_t old_perm_unused = 0;
    uint64_t old_sw = 0;
    rc = aarch64_pt_replace_4k(root, TEST_VA_BASE, data_pa,
                               AARCH64_PT_KERNEL_RW,
                               /* new sw: zero = drop PROTNONE */
                               0,
                               &old_pa, &old_perm_unused, &old_sw);
    assert_eq(0, rc);
    assert_eq(data_pa, old_pa);
    assert_true((old_sw & AARCH64_PT_SOFTWARE_PROTNONE) != 0);

    uint64_t qpa = 0;
    uint32_t qperm = 0;
    uint64_t qsw = 0;
    rc = aarch64_pt_query_4k_ext(root, TEST_VA_BASE, &qpa, &qperm, &qsw);
    assert_eq(0, rc);
    assert_eq(data_pa, qpa);
    assert_eq(AARCH64_PT_KERNEL_RW, qperm);
    assert_eq(0, qsw);
}

TEST_FUNC(test_update_to_protnone_keeps_pa_query_e_prot_none)
{
    /* Case 4: update from VALID to PROTNONE (same PA) → query returns
     * -EPROT_NONE with the SAME phys_out.  Old perm is also reported
     * so callers can reconstruct the prior VMA state. */
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    uint64_t data_pa = 0x8000ULL;
    int rc = aarch64_pt_map_4k_ext(root, TEST_VA_BASE, data_pa,
                                   AARCH64_PT_KERNEL_RW, 0);
    assert_eq(0, rc);

    uint64_t old_pa = 0;
    uint32_t old_perm_unused = 0;
    uint64_t old_sw = 0;
    rc = aarch64_pt_replace_4k(root, TEST_VA_BASE, data_pa,
                               /* perm out param unused; PROTNONE swap keeps the perm
                                * word of the original descriptor — pass KERNEL_RW to
                                * match the prior mapping.  replace_4k derives the
                                * PROTNONE stash directly from new sw bits. */
                               AARCH64_PT_KERNEL_RW,
                               AARCH64_PT_SOFTWARE_PROTNONE,
                               &old_pa, &old_perm_unused, &old_sw);
    assert_eq(0, rc);
    assert_eq(data_pa, old_pa);
    assert_eq(0, old_sw);

    uint64_t qpa = 0;
    uint32_t qperm = 0;
    uint64_t qsw = 0;
    rc = aarch64_pt_query_4k_ext(root, TEST_VA_BASE, &qpa, &qperm, &qsw);
    assert_eq(AARCH64_PT_EPROT_NONE, rc);
    assert_eq(data_pa, qpa);
    assert_true((qsw & AARCH64_PT_SOFTWARE_PROTNONE) != 0);
    assert_eq(0, g_free_calls);
}

TEST_FUNC(test_map_rejects_protnone_and_cow_combination)
{
    /* Case 5: software_bits = PROTNONE | COW is an illegal stash
     * combination — return -EINVAL.  Both bits represent distinct
     * page states; setting both is meaningless. */
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    int rc = aarch64_pt_map_4k_ext(root, TEST_VA_BASE, 0x9000,
                                   AARCH64_PT_KERNEL_RW,
                                   AARCH64_PT_SOFTWARE_PROTNONE |
                                   AARCH64_PT_SOFTWARE_COW);
    assert_eq(AARCH64_PT_EINVAL, rc);

    /* replace_4k should likewise reject */
    uint64_t old_pa = 0;
    uint32_t old_perm_unused = 0;
    uint64_t old_sw = 0;
    rc = aarch64_pt_replace_4k(root, TEST_VA_BASE, 0x9000,
                               AARCH64_PT_KERNEL_RW,
                               AARCH64_PT_SOFTWARE_PROTNONE |
                               AARCH64_PT_SOFTWARE_COW,
                               &old_pa, &old_perm_unused, &old_sw);
    assert_eq(AARCH64_PT_EINVAL, rc);
}

TEST_FUNC(test_encode_block_desc_produces_correct_bit_layout)
{
    /* Case 6: encode_block_desc outputs:
     *   - bit 0 (VALID)  = 1
     *   - bit 1 (TABLE)  = 0  ← block vs page-leaf distinction
     *   - PA bits [39:21] (2 MiB aligned)
     *   - AP[2:1] bits [7:6] mirror encode_perm's policy
     *   - software bits 55 / 56 reflect the sw input */
    uint64_t block_pa = 0x200000ULL;          /* 2 MiB aligned */
    uint64_t desc = aarch64_pt_encode_block_desc(block_pa,
                                                 AARCH64_PT_KERNEL_RW, 0);
    assert_true((desc & AARCH64_PT_DESC_VALID) != 0);
    assert_true((desc & AARCH64_PT_DESC_TABLE) == 0);  /* block */
    /* block OA bits [39:21] */
    assert_eq(block_pa, desc & UINT64_C(0xffffffe00000));
    assert_true((desc & AARCH64_PT_DESC_AF) != 0);
    /* KERNEL_RW without EXEC → PXN | UXN both set */
    assert_true((desc & AARCH64_PT_DESC_PXN) != 0);
    assert_true((desc & AARCH64_PT_DESC_UXN) != 0);
    /* no software bits */
    assert_eq(0, desc & (AARCH64_PT_SOFTWARE_PROTNONE |
                         AARCH64_PT_SOFTWARE_COW));

    /* NOCACHE forces PXN|UXN on aarch64 (spec §4.2 4-rejected-combos
     * accepted: VM_DEVICE = RO+RW × USER+KERNEL, all carry VM_NO_EXEC,
     * so the reject is encoded via the lower-level encode_perm which
     * already forces both XN bits for DEVICE.  Belt-and-braces assert.
     * DEVICE must be combined with a kernel/user access class — bare
     * DEVICE alone is invalid (no AP bits to encode), so pass
     * DEVICE | KERNEL_RW (the natural aarch64 NOCACHE combo). */
    uint64_t desc_dev = aarch64_pt_encode_block_desc(block_pa,
                                                    AARCH64_PT_DEVICE |
                                                    AARCH64_PT_KERNEL_RW,
                                                    0);
    assert_true((desc_dev & AARCH64_PT_DESC_PXN) != 0);
    assert_true((desc_dev & AARCH64_PT_DESC_UXN) != 0);
    /* DEVICE forces SH = non-shareable (bits [9:8] = 0; literal
     * 0x300 is the inner-shareable mask from page_table.c — page
     * table header doesn't expose it). */
    assert_eq(0, desc_dev & UINT64_C(0x300));

    /* software bits round-trip on blocks */
    uint64_t desc_sw = aarch64_pt_encode_block_desc(block_pa,
                                                   AARCH64_PT_KERNEL_RW,
                                                   AARCH64_PT_SOFTWARE_PROTNONE |
                                                   AARCH64_PT_SOFTWARE_COW);
    assert_true((desc_sw & AARCH64_PT_SOFTWARE_PROTNONE) != 0);
    assert_true((desc_sw & AARCH64_PT_SOFTWARE_COW) != 0);
    /* still a block (bit1=0), still valid, PA preserved */
    assert_true((desc_sw & AARCH64_PT_DESC_VALID) != 0);
    assert_true((desc_sw & AARCH64_PT_DESC_TABLE) == 0);
    assert_eq(block_pa, desc_sw & UINT64_C(0xffffffe00000));
}

TEST_FUNC(test_replace_returns_correct_old_perm_for_perm_only_update)
{
    /* Case 7 (review round 1 — Finding A): replace_4k must report the
     * PRIOR decoded perm via `*old_perm_out`, not the NEW perm.  The
     * arch_vmm_update_4k backend derives old_vm_out from this value;
     * a regression here silently breaks VMA-prot save/restore. */
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_va(&root_pa);
    assert_not_null(root);

    uint64_t data_pa = 0xa000ULL;
    /* Map KERNEL_RW → then downgrade to KERNEL_RO.  Same PA, same
     * memory type, same validity → perm-only fast path. */
    int rc = aarch64_pt_map_4k_ext(root, TEST_VA_BASE, data_pa,
                                   AARCH64_PT_KERNEL_RW, 0);
    assert_eq(0, rc);

    uint64_t old_pa = 0;
    uint32_t old_perm = 0xdeadbeef;
    uint64_t old_sw = 0xdeadbeef;
    rc = aarch64_pt_replace_4k(root, TEST_VA_BASE, data_pa,
                               AARCH64_PT_KERNEL_RO, 0,
                               &old_pa, &old_perm, &old_sw);
    assert_eq(0, rc);
    assert_eq(data_pa, old_pa);
    assert_eq((uint32_t)AARCH64_PT_KERNEL_RW, old_perm);
    assert_eq((uint64_t)0, old_sw);

    /* Reverse direction: RO → RW.  old_perm must be the previously-
     * stored KERNEL_RO, not the NEW KERNEL_RW. */
    old_pa = 0;
    old_perm = 0xdeadbeef;
    old_sw = 0xdeadbeef;
    rc = aarch64_pt_replace_4k(root, TEST_VA_BASE, data_pa,
                               AARCH64_PT_KERNEL_RW, 0,
                               &old_pa, &old_perm, &old_sw);
    assert_eq(0, rc);
    assert_eq((uint32_t)AARCH64_PT_KERNEL_RO, old_perm);

    /* After both updates the slot is queryable as the latest state. */
    uint64_t qpa = 0;
    uint32_t qperm = 0;
    uint64_t qsw = 0;
    rc = aarch64_pt_query_4k_ext(root, TEST_VA_BASE, &qpa, &qperm, &qsw);
    assert_eq(0, rc);
    assert_eq(data_pa, qpa);
    assert_eq((uint32_t)AARCH64_PT_KERNEL_RW, qperm);
    assert_eq((uint64_t)0, qsw);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_map_protnone_query_returns_e_prot_none_with_pa),
    TEST_ENTRY(test_unmap_protnone_returns_pa_does_not_free),
    TEST_ENTRY(test_update_to_valid_recovers_query),
    TEST_ENTRY(test_update_to_protnone_keeps_pa_query_e_prot_none),
    TEST_ENTRY(test_map_rejects_protnone_and_cow_combination),
    TEST_ENTRY(test_encode_block_desc_produces_correct_bit_layout),
    TEST_ENTRY(test_replace_returns_correct_old_perm_for_perm_only_update),
TEST_LIST_END

int main(void)
{
    /* mmap the kernel half window so PA + ARCH_PAGE_OFFSET resolves.
     * The pool lives here too — every "physical" page is just an
     * offset within this region.  ARCH_PAGE_OFFSET for the host
     * test is mocked to a low user-space address (mock/m1_a64_include/
     * arch/mmu.h) so MAP_FIXED succeeds on x86_64 Linux. */
    void *base = mmap((void *)(uintptr_t)ARCH_PAGE_OFFSET,
                      0x100000ULL,                          /* 1 MiB window */
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                      -1, 0);
    if (base == MAP_FAILED) return 2;
    memset(base, 0, 0x100000ULL);

    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
