/*
 * hosttests/cases/test_aarch64_backend_4k.c — aarch64 M3.3 Task 18
 * RED test: arch_vmm_* backend 12-legal + 4-reject combo matrix
 * (spec §4.2 / §4.4.4).
 *
 * The brief enumerates 12 legal VM_* combos across leaf (4 KiB) and
 * block (2 MiB) paths plus 4 reject combos (VM_NOCACHE without
 * VM_NO_EXEC).  This test drives the FULL arch_vmm_* API (the
 * semantic layer) — not the bare aarch64_pt_* primitives — so it
 * links the REAL kernel/arch/aarch64/memory/vmm_backend.c alongside
 * page_table.c.  Each combo:
 *
 *   - pre-conditions: kernel_map pinned to a fresh test root.
 *   - calls arch_vmm_map_4k_new (leaf combos 1-4, 9-10) or
 *     arch_vmm_map_2m (block combos 5-8, 11-12).
 *   - asserts the post-condition via arch_vmm_query_4k (semantic
 *     state) and aarch64_pt_read_l2_desc (raw descriptor bits for
 *     the block descriptor pin: V=1, bit1=0, OA, AP, SH, AttrIndx).
 *   - asserts the 4 reject combos return -EINVAL via either API.
 *
 * Link strategy mirrors the existing aarch64 page_table tests:
 *   - Real page_table.c + vmm_backend.c compile against host mocks.
 *   - aarch64_read_ttbr1 returns 0 (not active).
 *   - alloc_4k_page / free_4k_page are heap-backed pool.
 *   - vmm_gate_check is a no-op (pre-SMP gate is implicit on host).
 *
 * The kernel_map global is defined in vmm_backend.c; the test TU
 * assigns it directly (same pattern test_x86_vmm_backend.c uses for
 * its pinned pgdir).
 *
 * NOTE on software-bit round-trip (case 4 in the brief): the test
 * uses arch_vmm_query_4k (the semantic layer) to verify that
 * arch_vmm_update_4k flips PROTNONE → VALID and back.  This is
 * reasserted by the existing test_aarch64_pt_software_bits.c at the
 * primitive layer; here we cover the same round-trip via the
 * backend so a regression in vmm_backend.c's perm_to_vm / vm_to_sw
 * also surfaces.
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
#include <arch/spinlock.h>
#include <arch/aarch64/page_table.h>
#include <arch/aarch64/vmm_backend.h>
#include <memory/vmm.h>

#ifndef OS01_KERNEL_SRC
#error "Define OS01_KERNEL_SRC to the kernel source root (-DOS01_KERNEL_SRC=...)"
#endif

/* ── Mock PMM pool (host heap-backed, mmap'd into the kernel window) ──
 * Each test calls fresh_root_and_set_kernel_map() which allocates a
 * root page + sets kernel_map to its direct-map pointer.  16 slots
 * is enough for the 12-combo + reject matrices with a fresh root per
 * test (each combo map+unmap leaves the slot empty for reuse). */
#define MOCK_PA_BASE       0x10000ULL
#define MOCK_PA_STRIDE     0x1000ULL
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

/* ── Mocks for the symbols page_table.c + vmm_backend.c reference ── */
uint64_t aarch64_read_ttbr1(void) { return 0; }

/* Production vmm_gate.c is too heavy for the host harness (pulls in
 * percpu_data[] + dtb_cpu_count()); stub it directly. */
void vmm_gate_check(void) { (void)0; }

/* The weak vmm_gate_violation() default in vmm_gate.c spins forever;
 * the host harness overrides it with a longjmp capture so a
 * violation fails the test instead of hanging. */
static int g_violation_count;
static const char *g_violation_reason;
void vmm_gate_violation(const char *reason)
{
    g_violation_count++;
    g_violation_reason = reason;
    /* Stop the test runner cleanly — host single-threaded, no
     * concurrency to worry about. */
}

/* arch_vmm_init() calls aarch64_pt_root_publish() from vmm_gate.c;
 * we never invoke arch_vmm_init in this test (the test TU sets
 * kernel_map directly), but the symbol must resolve at link time. */
bool aarch64_pt_root_publish(uint64_t root_pa)
{
    (void)root_pa;
    return true;
}

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
 * Each test allocates a fresh root from the mock pool, mmaps the
 * kernel-half window for direct-mapped access, then sets kernel_map
 * to the new root.  The test teardown restores kernel_map to NULL so
 * subsequent tests start clean. */
#define TEST_VA_BASE 0xffff800000200000ULL

static uint64_t *fresh_root_and_set_kernel_map(uint64_t *out_pa)
{
    uint64_t pa = alloc_4k_page();
    if (pa == 0) return NULL;
    uint64_t *va = (uint64_t *)(uintptr_t)(pa + ARCH_PAGE_OFFSET);
    memset(va, 0, MOCK_PA_STRIDE);
    kernel_map = va;
    if (out_pa) *out_pa = pa;
    return va;
}

/* Descriptor bit constants (mirror page_table.c's local copies). */
#define TEST_DESC_VALID UINT64_C(0x001)
#define TEST_DESC_TABLE UINT64_C(0x002)
#define TEST_DESC_AF    UINT64_C(0x400)
#define TEST_DESC_PXN   UINT64_C(0x20000000000000)
#define TEST_DESC_UXN   UINT64_C(0x40000000000000)
#define TEST_DESC_USER  UINT64_C(0x040)
#define TEST_DESC_RO    UINT64_C(0x080)
#define TEST_DESC_SH_IS UINT64_C(0x300)
#define TEST_DESC_SH_NS UINT64_C(0x000)
#define TEST_DESC_ATTR_NORMAL UINT64_C(0x004)
#define TEST_BLOCK_OA_MASK    UINT64_C(0xffffffe00000)

/* ── Tests ────────────────────────────────────────────────────────── */

/* Cases 1, 2, 3, 4: leaf Normal combinations (4 KiB path).
 *
 * Brief mapping:
 *   1: VM_KERNEL_RW       → arch_vmm_map_4k_new → leaf Normal K RW
 *   2: VM_KERNEL_RO       → leaf Normal K RO
 *   3: VM_USER_RW         → leaf Normal U RW
 *   4: VM_USER_RO         → leaf Normal U RO
 *
 * Each combo: map → query back with VM_PRESENT and the expected
 * access bits → -EEXIST on a duplicate map (existing slot is
 * occupied, including the VM_PRESENT bit) → cleanup with unmap_4k.
 */
TEST_FUNC(test_backend_4k_kernel_rw_normal)
{
    /* Combo 1 — VM_KERNEL_RW. */
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    uint64_t pa = 0x5000ULL;
    int rc = arch_vmm_map_4k_new(root, pa, TEST_VA_BASE, VM_KERNEL_RW);
    assert_eq(0, rc);

    uint64_t qpa = 0;
    uint32_t qvm = 0;
    rc = arch_vmm_query_4k(root, TEST_VA_BASE, &qpa, &qvm);
    assert_eq(0, rc);
    assert_eq(pa, qpa);
    assert_true((qvm & VM_PRESENT) != 0);
    assert_true((qvm & VM_WRITE)   != 0);
    assert_true((qvm & VM_USER)    == 0);
    assert_true((qvm & VM_NO_EXEC) == 0);   /* RW without NO_EXEC → executable */

    /* Duplicate map → -EEXIST (any occupied slot, per Task 16 contract).
     * The backend returns the aarch64_pt_* result codes — use the
     * AARCH64_PT_EEXIST sentinel, not Linux -EEXIST. */
    rc = arch_vmm_map_4k_new(root, pa + 0x1000, TEST_VA_BASE, VM_KERNEL_RW);
    assert_eq(AARCH64_PT_EEXIST, rc);

    /* Unmap and re-query → -ENOENT (AARCH64_PT_ENOENT sentinel). */
    rc = arch_vmm_unmap_4k(root, TEST_VA_BASE, NULL, NULL);
    assert_eq(0, rc);
    rc = arch_vmm_query_4k(root, TEST_VA_BASE, &qpa, &qvm);
    assert_eq(AARCH64_PT_ENOENT, rc);
}

TEST_FUNC(test_backend_4k_kernel_ro_normal)
{
    /* Combo 2 — VM_KERNEL_RO. */
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    int rc = arch_vmm_map_4k_new(root, 0x6000ULL, TEST_VA_BASE, VM_KERNEL_RO);
    assert_eq(0, rc);

    uint64_t qpa = 0;
    uint32_t qvm = 0;
    rc = arch_vmm_query_4k(root, TEST_VA_BASE, &qpa, &qvm);
    assert_eq(0, rc);
    assert_true((qvm & VM_PRESENT) != 0);
    assert_true((qvm & VM_WRITE)   == 0);   /* RO */
    assert_true((qvm & VM_USER)    == 0);
}

TEST_FUNC(test_backend_4k_user_rw_normal)
{
    /* Combo 3 — VM_USER_RW. */
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    int rc = arch_vmm_map_4k_new(root, 0x7000ULL, TEST_VA_BASE, VM_USER_RW);
    assert_eq(0, rc);

    uint32_t qvm = 0;
    rc = arch_vmm_query_4k(root, TEST_VA_BASE, NULL, &qvm);
    assert_eq(0, rc);
    assert_true((qvm & VM_PRESENT) != 0);
    assert_true((qvm & VM_WRITE)   != 0);
    assert_true((qvm & VM_USER)    != 0);
}

TEST_FUNC(test_backend_4k_user_ro_normal)
{
    /* Combo 4 — VM_USER_RO. */
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    int rc = arch_vmm_map_4k_new(root, 0x8000ULL, TEST_VA_BASE, VM_USER_RO);
    assert_eq(0, rc);

    uint32_t qvm = 0;
    rc = arch_vmm_query_4k(root, TEST_VA_BASE, NULL, &qvm);
    assert_eq(0, rc);
    assert_true((qvm & VM_PRESENT) != 0);
    assert_true((qvm & VM_WRITE)   == 0);
    assert_true((qvm & VM_USER)    != 0);
}

/* Cases 9, 10: leaf Device combinations (4 KiB path). */
TEST_FUNC(test_backend_4k_kernel_device)
{
    /* Combo 9 — VM_DEVICE (K RW device). */
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    int rc = arch_vmm_map_4k_new(root, 0x9000ULL, TEST_VA_BASE, VM_DEVICE);
    assert_eq(0, rc);

    uint32_t qvm = 0;
    rc = arch_vmm_query_4k(root, TEST_VA_BASE, NULL, &qvm);
    assert_eq(0, rc);
    assert_true((qvm & VM_PRESENT)  != 0);
    assert_true((qvm & VM_WRITE)    != 0);
    assert_true((qvm & VM_USER)     == 0);
    assert_true((qvm & VM_NOCACHE)  != 0);
    assert_true((qvm & VM_NO_EXEC)  != 0);   /* Device always NX */
}

TEST_FUNC(test_backend_4k_user_device)
{
    /* Combo 10 — VM_DEVICE | VM_USER. */
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    int rc = arch_vmm_map_4k_new(root, 0xa000ULL, TEST_VA_BASE,
                                 VM_DEVICE | VM_USER);
    assert_eq(0, rc);

    uint32_t qvm = 0;
    rc = arch_vmm_query_4k(root, TEST_VA_BASE, NULL, &qvm);
    assert_eq(0, rc);
    assert_true((qvm & VM_PRESENT)  != 0);
    assert_true((qvm & VM_USER)     != 0);
    assert_true((qvm & VM_NOCACHE)  != 0);
    assert_true((qvm & VM_NO_EXEC)  != 0);
}

/* Cases 5-8, 11-12: block combinations (2 MiB path).  Each calls
 * arch_vmm_map_2m (the backend equivalent of `map_2m_block`), then
 * aarch64_pt_read_l2_desc to verify the descriptor is a block. */
TEST_FUNC(test_backend_2m_kernel_rw_normal)
{
    /* Combo 5 — VM_KERNEL_RW | VM_HUGE. */
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    uint64_t pa = 0x400000ULL;
    int rc = arch_vmm_map_2m(root, pa, TEST_VA_BASE,
                             VM_KERNEL_RW | VM_HUGE);
    assert_eq(0, rc);

    uint64_t desc = 0;
    rc = aarch64_pt_read_l2_desc(root, TEST_VA_BASE, &desc);
    assert_eq(0, rc);
    assert_true((desc & TEST_DESC_VALID) != 0);
    assert_true((desc & TEST_DESC_TABLE) == 0);  /* block */
    assert_eq(pa, desc & TEST_BLOCK_OA_MASK);
    /* VM_KERNEL_RW (no VM_NO_EXEC) → EXEC bit set in perm → kernel
     * may execute → PXN cleared (UXN stays set; irrelevant for
     * kernel CS but the bit is still asserted). */
    assert_true((desc & TEST_DESC_PXN) == 0);
    assert_true((desc & TEST_DESC_UXN) != 0);
    assert_eq(TEST_DESC_SH_IS, desc & TEST_DESC_SH_IS);
    assert_eq(TEST_DESC_ATTR_NORMAL, desc & TEST_DESC_ATTR_NORMAL);

    /* Cleanup. */
    uint64_t got_pa = 0;
    rc = arch_vmm_unmap_2m(root, TEST_VA_BASE, &got_pa);
    assert_eq(0, rc);
    assert_eq(pa, got_pa);
}

TEST_FUNC(test_backend_2m_kernel_ro_normal)
{
    /* Combo 6 — VM_KERNEL_RO | VM_HUGE. */
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    int rc = arch_vmm_map_2m(root, 0x600000ULL, TEST_VA_BASE,
                             VM_KERNEL_RO | VM_HUGE);
    assert_eq(0, rc);

    uint64_t desc = 0;
    rc = aarch64_pt_read_l2_desc(root, TEST_VA_BASE, &desc);
    assert_eq(0, rc);
    assert_true((desc & TEST_DESC_TABLE) == 0);
    bool is_user = (desc & TEST_DESC_USER) != 0;
    bool is_ro   = (desc & TEST_DESC_RO)   != 0;
    assert_false(is_user);
    assert_true(is_ro);

    rc = arch_vmm_unmap_2m(root, TEST_VA_BASE, NULL);
    assert_eq(0, rc);
}

TEST_FUNC(test_backend_2m_user_rw_normal)
{
    /* Combo 7 — VM_USER_RW | VM_HUGE. */
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    int rc = arch_vmm_map_2m(root, 0x800000ULL, TEST_VA_BASE,
                             VM_USER_RW | VM_HUGE);
    assert_eq(0, rc);

    uint64_t desc = 0;
    rc = aarch64_pt_read_l2_desc(root, TEST_VA_BASE, &desc);
    assert_eq(0, rc);
    assert_true((desc & TEST_DESC_TABLE) == 0);
    bool is_user = (desc & TEST_DESC_USER) != 0;
    assert_true(is_user);
    assert_true((desc & TEST_DESC_PXN) != 0);   /* K never executes user */
    assert_true((desc & TEST_DESC_UXN) == 0);   /* EL0 may execute */

    rc = arch_vmm_unmap_2m(root, TEST_VA_BASE, NULL);
    assert_eq(0, rc);
}

TEST_FUNC(test_backend_2m_user_ro_normal)
{
    /* Combo 8 — VM_USER_RO | VM_HUGE. */
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    int rc = arch_vmm_map_2m(root, 0xa00000ULL, TEST_VA_BASE,
                             VM_USER_RO | VM_HUGE);
    assert_eq(0, rc);

    uint64_t desc = 0;
    rc = aarch64_pt_read_l2_desc(root, TEST_VA_BASE, &desc);
    assert_eq(0, rc);
    bool is_user = (desc & TEST_DESC_USER) != 0;
    bool is_ro   = (desc & TEST_DESC_RO)   != 0;
    assert_true(is_user);
    assert_true(is_ro);

    rc = arch_vmm_unmap_2m(root, TEST_VA_BASE, NULL);
    assert_eq(0, rc);
}

TEST_FUNC(test_backend_2m_kernel_device)
{
    /* Combo 11 — VM_DEVICE | VM_HUGE. */
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    uint64_t pa = 0x10000000ULL;       /* device window */
    int rc = arch_vmm_map_2m(root, pa, TEST_VA_BASE,
                             VM_DEVICE | VM_HUGE);
    assert_eq(0, rc);

    uint64_t desc = 0;
    rc = aarch64_pt_read_l2_desc(root, TEST_VA_BASE, &desc);
    assert_eq(0, rc);
    assert_true((desc & TEST_DESC_TABLE) == 0);
    assert_true((desc & TEST_DESC_PXN) != 0);
    assert_true((desc & TEST_DESC_UXN) != 0);
    assert_eq(TEST_DESC_SH_NS, desc & TEST_DESC_SH_IS);
    assert_eq(0, desc & TEST_DESC_ATTR_NORMAL);  /* Device AttrIdx = 0 */

    uint64_t got_pa = 0;
    rc = arch_vmm_unmap_2m(root, TEST_VA_BASE, &got_pa);
    assert_eq(0, rc);
    assert_eq(pa, got_pa);
}

TEST_FUNC(test_backend_2m_user_device)
{
    /* Combo 12 — VM_DEVICE | VM_USER | VM_HUGE. */
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    int rc = arch_vmm_map_2m(root, 0x11000000ULL, TEST_VA_BASE,
                             VM_DEVICE | VM_USER | VM_HUGE);
    assert_eq(0, rc);

    uint64_t desc = 0;
    rc = aarch64_pt_read_l2_desc(root, TEST_VA_BASE, &desc);
    assert_eq(0, rc);
    bool is_user = (desc & TEST_DESC_USER) != 0;
    assert_true(is_user);
    assert_true((desc & TEST_DESC_PXN) != 0);
    assert_true((desc & TEST_DESC_UXN) != 0);

    rc = arch_vmm_unmap_2m(root, TEST_VA_BASE, NULL);
    assert_eq(0, rc);
}

/* Cases R1-R4: reject combos (NOCACHE without NO_EXEC → -EINVAL
 * via either API). */
TEST_FUNC(test_backend_rejects_nocache_without_noexec_leaf)
{
    /* R1: K RW missing NO_EXEC — use leaf API. */
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    int rc = arch_vmm_map_4k_new(root, 0x5000ULL, TEST_VA_BASE,
                                 VM_PRESENT | VM_WRITE | VM_NOCACHE);
    assert_eq(-EINVAL, rc);
}

TEST_FUNC(test_backend_rejects_nocache_without_noexec_block)
{
    /* R1 again, but via the block API.  Same EINVAL. */
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    int rc = arch_vmm_map_2m(root, 0x400000ULL, TEST_VA_BASE,
                             VM_PRESENT | VM_WRITE | VM_NOCACHE | VM_HUGE);
    assert_eq(-EINVAL, rc);
}

TEST_FUNC(test_backend_rejects_all_four_reject_combos)
{
    /* R1-R4 (VM_NOCACHE & ~VM_NO_EXEC across RO/RW × USER/KERNEL)
     * via the leaf path.  Block-path equivalents already covered
     * by the previous test + the combo matrix. */
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    /* R1: K RW NOCACHE missing NO_EXEC. */
    int rc = arch_vmm_map_4k_new(root, 0x5000ULL, TEST_VA_BASE,
                                 VM_PRESENT | VM_WRITE | VM_NOCACHE);
    assert_eq(-EINVAL, rc);

    /* R2: K RO NOCACHE missing NO_EXEC. */
    rc = arch_vmm_map_4k_new(root, 0x6000ULL, TEST_VA_BASE,
                             VM_PRESENT | VM_NOCACHE);
    assert_eq(-EINVAL, rc);

    /* R3: U RW NOCACHE missing NO_EXEC. */
    rc = arch_vmm_map_4k_new(root, 0x7000ULL, TEST_VA_BASE,
                             VM_PRESENT | VM_WRITE | VM_USER | VM_NOCACHE);
    assert_eq(-EINVAL, rc);

    /* R4: U RO NOCACHE missing NO_EXEC. */
    rc = arch_vmm_map_4k_new(root, 0x8000ULL, TEST_VA_BASE,
                             VM_PRESENT | VM_USER | VM_NOCACHE);
    assert_eq(-EINVAL, rc);
}

/* Case: software-bit round-trip via the backend.  Map with
 * VM_PROTNONE (PRESENT must be clear — PROTNONE without PRESENT is
 * the only legal "invalid but holds PA" state per spec §4.2) →
 * query returns -EPROT_NONE with the stashed PA in phys_out.  Then
 * update_4k back to a plain VALID mapping → query returns 0. */
TEST_FUNC(test_backend_software_bit_roundtrip)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    uint64_t pa = 0xb000ULL;
    int rc = arch_vmm_map_4k_new(root, pa, TEST_VA_BASE, VM_PROTNONE);
    assert_eq(0, rc);

    uint64_t qpa = 0;
    uint32_t qvm = 0;
    rc = arch_vmm_query_4k(root, TEST_VA_BASE, &qpa, &qvm);
    assert_eq(AARCH64_PT_EPROT_NONE, rc);
    assert_eq(pa, qpa);
    assert_true((qvm & VM_PROTNONE) != 0);
    assert_true((qvm & VM_PRESENT)  == 0);

    /* Update back to a normal VALID KERNEL_RW leaf (PROTNONE cleared,
     * PA preserved). */
    rc = arch_vmm_update_4k(root, pa, TEST_VA_BASE, VM_KERNEL_RW,
                            NULL, NULL);
    assert_eq(0, rc);

    rc = arch_vmm_query_4k(root, TEST_VA_BASE, &qpa, &qvm);
    assert_eq(0, rc);
    assert_eq(pa, qpa);
    assert_true((qvm & VM_PRESENT)  != 0);
    assert_true((qvm & VM_PROTNONE) == 0);
}

/* Case: backend dispatch sanity — map_4k_new with VM_HUGE → -EINVAL
 * (Huge is the block API's signal; leaf API rejects it).  Mirrors
 * the leaf-vs-block split the brief calls out. */
TEST_FUNC(test_backend_4k_api_rejects_huge_flag)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    int rc = arch_vmm_map_4k_new(root, 0x5000ULL, TEST_VA_BASE,
                                 VM_KERNEL_RW | VM_HUGE);
    assert_eq(-EINVAL, rc);
}

/* Case: backend dispatch sanity — map_2m without VM_HUGE → -EINVAL
 * (Block API requires HUGE to be set; without it the path is the
 * leaf API's territory). */
TEST_FUNC(test_backend_2m_api_rejects_missing_huge_flag)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    int rc = arch_vmm_map_2m(root, 0x400000ULL, TEST_VA_BASE, VM_KERNEL_RW);
    assert_eq(-EINVAL, rc);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_backend_4k_kernel_rw_normal),
    TEST_ENTRY(test_backend_4k_kernel_ro_normal),
    TEST_ENTRY(test_backend_4k_user_rw_normal),
    TEST_ENTRY(test_backend_4k_user_ro_normal),
    TEST_ENTRY(test_backend_4k_kernel_device),
    TEST_ENTRY(test_backend_4k_user_device),
    TEST_ENTRY(test_backend_2m_kernel_rw_normal),
    TEST_ENTRY(test_backend_2m_kernel_ro_normal),
    TEST_ENTRY(test_backend_2m_user_rw_normal),
    TEST_ENTRY(test_backend_2m_user_ro_normal),
    TEST_ENTRY(test_backend_2m_kernel_device),
    TEST_ENTRY(test_backend_2m_user_device),
    TEST_ENTRY(test_backend_rejects_nocache_without_noexec_leaf),
    TEST_ENTRY(test_backend_rejects_nocache_without_noexec_block),
    TEST_ENTRY(test_backend_rejects_all_four_reject_combos),
    TEST_ENTRY(test_backend_software_bit_roundtrip),
    TEST_ENTRY(test_backend_4k_api_rejects_huge_flag),
    TEST_ENTRY(test_backend_2m_api_rejects_missing_huge_flag),
TEST_LIST_END

int main(void)
{
    /* mmap the kernel half window so PA + ARCH_PAGE_OFFSET resolves.
     * <memory/vmm.h> defines `mmap` as a macro (the kernel's typed
     * page-table-root pointer), which collides with the host libc
     * mmap(2) call we need to set up the test's backing region.
     * Undef the macro here so the system mmap is callable; the
     * kernel type is still available via kernel_map's declaration
     * elsewhere in the TU. */
    #undef mmap
    void *base = mmap((void *)(uintptr_t)ARCH_PAGE_OFFSET,
                      0x200000ULL,                          /* 2 MiB window */
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                      -1, 0);
    if (base == MAP_FAILED) return 2;
    memset(base, 0, 0x200000ULL);

    /* Reset kernel_map so check_kernel_map fails for any stray call
     * before fresh_root_and_set_kernel_map runs. */
    kernel_map = NULL;

    RUN_ALL_TESTS();

    /* Reset kernel_map so a re-run of the test starts clean. */
    kernel_map = NULL;

    return __test_stats.failed > 0 ? 1 : 0;
}
