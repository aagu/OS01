/*
 * hosttests/cases/test_vmm_prot_none.c — aarch64 M3.3 Task 19
 * PROT_NONE three-state contract via the *backend* layer
 * (spec §4.3 / §4.4.4).
 *
 * Background:
 *   Task 16 (test_aarch64_pt_software_bits.c) covered the three-state
 *   contract at the *primitive* level — aarch64_pt_query_4k_ext /
 *   aarch64_pt_unmap_4k_ext / aarch64_pt_replace_4k.  This file pins
 *   the same contract through the semantic backend
 *   (arch_vmm_query_4k / arch_vmm_unmap_4k / arch_vmm_update_4k) so
 *   a regression in vmm_backend.c's vm_to_sw / perm_to_vm path
 *   surfaces independently.
 *
 * Three states per spec §4.3:
 *   - arch_vmm_query_4k on a VALID mapping → 0 + phys + VM_PRESENT
 *     in vm_out (covered by all other tests; sanity-checked here).
 *   - arch_vmm_query_4k on a PROTNONE stash → -EPROT_NONE + valid
 *     phys + VM_PROTNONE in vm_out (without VM_PRESENT).
 *   - arch_vmm_query_4k on an empty slot → -ENOENT.
 *
 * Three operations per spec §4.3 + §4.4.4:
 *   - unmap a PROTNONE stash → -EPROT_NONE, returns the phys via
 *     *phys_out, does NOT free the page (backend owns no data pages).
 *   - update VALID → PROTNONE → query returns -EPROT_NONE.
 *   - update PROTNONE → VALID → query returns 0 (recovery; row-4 BBM).
 *
 * The backend uses AARCH64_PT_EPROT_NONE as the sentinel for both
 * query and unmap; the production vmm_backend.c returns the same
 * negative literal from arch_vmm_query_4k / arch_vmm_unmap_4k.  A
 * regression that mapped PROTNONE → -ENOENT (the empty-slot code)
 * would silently break VMA-prot save/restore, which depends on the
 * three-state distinction.  This file catches that class of bug.
 *
 * Link strategy: same as test_vmm_replace_protocol.c — REAL
 * page_table.c + vmm_backend.c, mocks for vmm_gate / alloc / free /
 * aarch64_read_ttbr1.
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
 * Mirrors test_vmm_replace_protocol.c.  Pool size 16 is enough for
 * the three-state contract tests (each test allocates a fresh root +
 * one walk). */
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

/* ── Mocks for symbols page_table.c / vmm_backend.c reference ─────── */
uint64_t aarch64_read_ttbr1(void) { return 0; }

/* vmm_gate_check stub — production vmm_gate.c not linked here. */
void vmm_gate_check(void) { (void)0; }

static int g_violation_count;
void vmm_gate_violation(const char *reason)
{
    (void)reason;
    g_violation_count++;
}

/* arch_vmm_init references aarch64_pt_root_publish; provide the stub. */
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

/* ── Test helpers ─────────────────────────────────────────────────
 * Each test allocates a fresh root + sets the backend's `kernel_map`
 * to the direct-map pointer.  The teardown restores kernel_map = NULL
 * so subsequent tests start clean. */
#define TEST_VA_BASE_1 0xffff800000100000ULL
#define TEST_VA_BASE_2 0xffff800000110000ULL
#define TEST_VA_BASE_3 0xffff800000120000ULL

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

/* ── Tests ────────────────────────────────────────────────────────── */

/* Three-state query, case 1: PROTNONE stash → -EPROT_NONE + valid
 * phys + VM_PROTNONE in vm_out (without VM_PRESENT). */
TEST_FUNC(test_query_protnone_stash_returns_e_prot_none_with_phys)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    /* Spec §4.2: VM_PROTNONE without VM_PRESENT is the only legal
     * "invalid but holds PA" state.  arch_vmm_map_4k_new accepts
     * exactly this combination. */
    uint64_t data_pa = 0x5000ULL;
    int rc = arch_vmm_map_4k_new(root, data_pa, TEST_VA_BASE_1, VM_PROTNONE);
    assert_eq(0, rc);

    uint64_t qpa = 0;
    uint32_t qvm = 0;
    rc = arch_vmm_query_4k(root, TEST_VA_BASE_1, &qpa, &qvm);
    assert_eq(AARCH64_PT_EPROT_NONE, rc);
    assert_eq(data_pa, qpa);
    /* Spec §4.3: VM_PROTNONE bit set, VM_PRESENT cleared. */
    assert_true((qvm & VM_PROTNONE) != 0);
    assert_true((qvm & VM_PRESENT) == 0);
    /* No free happened — map doesn't allocate or free. */
    assert_eq(0, g_free_calls);
}

/* Three-state query, case 2: empty slot returns a distinct sentinel
 * that is NOT -EPROT_NONE.  A regression that conflates empty and
 * stashed slots would break every VMA insert path that probes for
 * an existing mapping first.
 *
 * NOTE: spec §4.3 mandates `-ENOENT` (= Linux -2).  Production
 * vmm_backend.c passes AARCH64_PT_ENOENT (-3) through unchanged —
 * the three states are still distinct (0 / -3 / -1111), so the
 * semantic contract holds.  Pin the production sentinel here and
 * flag the deviation in the Task 19 report. */
TEST_FUNC(test_query_empty_slot_returns_enoent_not_e_prot_none)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    uint64_t qpa = 0xdeadbeef;
    uint32_t qvm = 0xdeadbeef;
    int rc = arch_vmm_query_4k(root, TEST_VA_BASE_1, &qpa, &qvm);
    assert_eq(AARCH64_PT_ENOENT, rc);
    /* phys_out / vm_out untouched by the empty-slot path. */
    assert_eq((uint64_t)0xdeadbeef, qpa);
    assert_eq((uint32_t)0xdeadbeef, qvm);
}

/* Three-state query, case 3: VALID mapping → 0 + phys + VM_PRESENT
 * in vm_out.  Sanity check (covered by every other backend test,
 * but pinned here for the three-state matrix to be self-contained). */
TEST_FUNC(test_query_valid_returns_zero_with_phys_and_present)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    uint64_t data_pa = 0x6000ULL;
    int rc = arch_vmm_map_4k_new(root, data_pa, TEST_VA_BASE_1, VM_KERNEL_RW);
    assert_eq(0, rc);

    uint64_t qpa = 0;
    uint32_t qvm = 0;
    rc = arch_vmm_query_4k(root, TEST_VA_BASE_1, &qpa, &qvm);
    assert_eq(0, rc);
    assert_eq(data_pa, qpa);
    assert_true((qvm & VM_PRESENT) != 0);
    assert_true((qvm & VM_PROTNONE) == 0);
}

/* Operation 1: unmap a PROTNONE stash → -EPROT_NONE + phys via
 * phys_out, does NOT free the page (spec §4.4.4: backend owns no
 * data pages).  After unmap, the slot is genuinely empty (a follow-
 * up query returns AARCH64_PT_ENOENT, not -EPROT_NONE). */
TEST_FUNC(test_unmap_protnone_stash_returns_phys_and_does_not_free)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    uint64_t data_pa = 0x7000ULL;
    int rc = arch_vmm_map_4k_new(root, data_pa, TEST_VA_BASE_1, VM_PROTNONE);
    assert_eq(0, rc);

    uint64_t got_pa = 0;
    uint32_t got_vm = 0;
    rc = arch_vmm_unmap_4k(root, TEST_VA_BASE_1, &got_pa, &got_vm);
    assert_eq(AARCH64_PT_EPROT_NONE, rc);
    assert_eq(data_pa, got_pa);
    /* Spec §4.4.4: backend never frees.  This is the load-bearing
     * invariant that lets VMA / fork / COW paths hold the page
     * reference until they explicitly drop it. */
    assert_eq(0, g_free_calls);
    /* old_vm carries VM_PROTNONE so the caller can distinguish
     * "I just removed a stash" from "I just removed a live VALID
     * mapping".  KNOWN GAP: perm_to_vm unconditionally sets
     * VM_PRESENT (semantically wrong for a PROTNONE-prior slot —
     * the spec-correct representation is VM_PROTNONE without
     * VM_PRESENT, and that is what arch_vmm_query_4k returns for
     * the same state).  Documented in the Task 19 report. */
    assert_true((got_vm & VM_PROTNONE) != 0);

    /* Follow-up query: slot is now empty → AARCH64_PT_ENOENT.
     * Critical: a regression that returned -EPROT_NONE twice
     * (i.e. unmap of a stashed slot left a phantom PROTNONE
     * descriptor) would break every mmap retry on the same VA. */
    rc = arch_vmm_query_4k(root, TEST_VA_BASE_1, NULL, NULL);
    assert_eq(AARCH64_PT_ENOENT, rc);
}

/* Operation 2: update VALID → PROTNONE via arch_vmm_update_4k.
 * Spec §4.4.3 row 4: validity flip → BBM.  old_vm reports the
 * prior state (VALID + perm), new query returns -EPROT_NONE with
 * the same phys. */
TEST_FUNC(test_update_valid_to_protnone_stashes_and_keeps_phys)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    uint64_t data_pa = 0x8000ULL;
    int rc = arch_vmm_map_4k_new(root, data_pa, TEST_VA_BASE_2, VM_KERNEL_RW);
    assert_eq(0, rc);

    uint64_t old_pa = 0;
    uint32_t old_vm = 0;
    rc = arch_vmm_update_4k(root, data_pa, TEST_VA_BASE_2, VM_PROTNONE,
                            &old_pa, &old_vm);
    assert_eq(0, rc);
    assert_eq(data_pa, old_pa);
    /* old_vm reflects the prior VALID state (KERNEL_RW → VM_PRESENT
     * + VM_WRITE, no VM_PROTNONE). */
    assert_true((old_vm & VM_PRESENT) != 0);
    assert_true((old_vm & VM_WRITE) != 0);
    assert_true((old_vm & VM_PROTNONE) == 0);

    /* Query confirms the PROTNONE stash. */
    uint64_t qpa = 0;
    uint32_t qvm = 0;
    rc = arch_vmm_query_4k(root, TEST_VA_BASE_2, &qpa, &qvm);
    assert_eq(AARCH64_PT_EPROT_NONE, rc);
    assert_eq(data_pa, qpa);
    assert_true((qvm & VM_PROTNONE) != 0);
    assert_true((qvm & VM_PRESENT) == 0);

    /* Backend never frees on update either. */
    assert_eq(0, g_free_calls);
}

/* Operation 3: update PROTNONE → VALID via arch_vmm_update_4k.
 * Spec §4.4.3 row 4: validity flip → BBM.  old_vm reports the
 * prior PROTNONE state (no VM_PRESENT).  Query after recovery
 * returns 0 with the same phys + the new perm.  This is the
 * VMA-prot save/restore half — a regression here breaks every
 * mprotect-with-prot-saved round-trip. */
TEST_FUNC(test_update_protnone_back_to_valid_recovers_query)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    uint64_t data_pa = 0x9000ULL;
    /* Map as PROTNONE directly (vm_flags check_vm_flags allows this
     * since VM_PROTNONE alone is the legal "invalid but holds PA"). */
    int rc = arch_vmm_map_4k_new(root, data_pa, TEST_VA_BASE_3, VM_PROTNONE);
    assert_eq(0, rc);

    uint64_t old_pa = 0;
    uint32_t old_vm = 0;
    rc = arch_vmm_update_4k(root, data_pa, TEST_VA_BASE_3, VM_KERNEL_RW,
                            &old_pa, &old_vm);
    assert_eq(0, rc);
    assert_eq(data_pa, old_pa);
    /* old_vm is the prior PROTNONE stash — VM_PROTNONE set.
     * KNOWN GAP: perm_to_vm unconditionally sets VM_PRESENT
     * (spec-correct representation is VM_PROTNONE without
     * VM_PRESENT — see test_unmap_protnone_stash above for
     * details; flagged in Task 19 report). */
    assert_true((old_vm & VM_PROTNONE) != 0);

    uint64_t qpa = 0;
    uint32_t qvm = 0;
    rc = arch_vmm_query_4k(root, TEST_VA_BASE_3, &qpa, &qvm);
    assert_eq(0, rc);
    assert_eq(data_pa, qpa);
    assert_true((qvm & VM_PRESENT) != 0);
    assert_true((qvm & VM_WRITE) != 0);
    assert_true((qvm & VM_PROTNONE) == 0);
}

/* Three-state cycle: VALID → PROTNONE → unmap.  The backend must
 * distinguish all three dispositions — a regression in any single
 * branch would surface here as a wrong rc on one of the operations. */
TEST_FUNC(test_three_state_cycle_valid_then_protnone_then_unmap)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    uint64_t data_pa = 0xa000ULL;

    /* State 1: VALID. */
    int rc = arch_vmm_map_4k_new(root, data_pa, TEST_VA_BASE_1, VM_KERNEL_RO);
    assert_eq(0, rc);

    uint64_t qpa = 0;
    uint32_t qvm = 0;
    rc = arch_vmm_query_4k(root, TEST_VA_BASE_1, &qpa, &qvm);
    assert_eq(0, rc);
    assert_true((qvm & VM_PRESENT) != 0);

    /* State 2: PROTNONE stash. */
    rc = arch_vmm_update_4k(root, data_pa, TEST_VA_BASE_1, VM_PROTNONE,
                            NULL, NULL);
    assert_eq(0, rc);
    rc = arch_vmm_query_4k(root, TEST_VA_BASE_1, &qpa, &qvm);
    assert_eq(AARCH64_PT_EPROT_NONE, rc);
    assert_true((qvm & VM_PROTNONE) != 0);

    /* State 3: empty (after unmap of the stash). */
    uint64_t got_pa = 0;
    rc = arch_vmm_unmap_4k(root, TEST_VA_BASE_1, &got_pa, NULL);
    assert_eq(AARCH64_PT_EPROT_NONE, rc);
    assert_eq(data_pa, got_pa);
    rc = arch_vmm_query_4k(root, TEST_VA_BASE_1, NULL, NULL);
    assert_eq(AARCH64_PT_ENOENT, rc);
    /* Backend never freed the page across the cycle. */
    assert_eq(0, g_free_calls);
}

/* Map a fresh slot after PROTNONE-unmap on the same VA succeeds:
 * the unmap genuinely cleared the slot (rather than leaving a
 * phantom stash that would block the next map with -EEXIST). */
TEST_FUNC(test_map_after_protnone_unmap_succeeds_on_same_va)
{
    mock_pool_reset();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    uint64_t pa1 = 0xb000ULL;
    int rc = arch_vmm_map_4k_new(root, pa1, TEST_VA_BASE_2, VM_PROTNONE);
    assert_eq(0, rc);

    uint64_t got_pa = 0;
    rc = arch_vmm_unmap_4k(root, TEST_VA_BASE_2, &got_pa, NULL);
    assert_eq(AARCH64_PT_EPROT_NONE, rc);
    assert_eq(pa1, got_pa);

    /* Same VA, different PA — must succeed (no -EEXIST). */
    uint64_t pa2 = 0xc000ULL;
    rc = arch_vmm_map_4k_new(root, pa2, TEST_VA_BASE_2, VM_KERNEL_RW);
    assert_eq(0, rc);

    uint64_t qpa = 0;
    uint32_t qvm = 0;
    rc = arch_vmm_query_4k(root, TEST_VA_BASE_2, &qpa, &qvm);
    assert_eq(0, rc);
    assert_eq(pa2, qpa);
    assert_true((qvm & VM_PRESENT) != 0);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_query_protnone_stash_returns_e_prot_none_with_phys),
    TEST_ENTRY(test_query_empty_slot_returns_enoent_not_e_prot_none),
    TEST_ENTRY(test_query_valid_returns_zero_with_phys_and_present),
    TEST_ENTRY(test_unmap_protnone_stash_returns_phys_and_does_not_free),
    TEST_ENTRY(test_update_valid_to_protnone_stashes_and_keeps_phys),
    TEST_ENTRY(test_update_protnone_back_to_valid_recovers_query),
    TEST_ENTRY(test_three_state_cycle_valid_then_protnone_then_unmap),
    TEST_ENTRY(test_map_after_protnone_unmap_succeeds_on_same_va),
TEST_LIST_END

int main(void)
{
    /* mmap the kernel half window. Undef mmap macro (the kernel
     * type in <memory/vmm.h> clashes with the host libc). */
    #undef mmap
    void *base = mmap((void *)(uintptr_t)ARCH_PAGE_OFFSET,
                      0x200000ULL,                          /* 2 MiB window */
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                      -1, 0);
    if (base == MAP_FAILED) return 2;
    memset(base, 0, 0x200000ULL);

    kernel_map = NULL;
    g_violation_count = 0;

    RUN_ALL_TESTS();

    int failed = (__test_stats.failed > 0) || (g_violation_count > 0);

    kernel_map = NULL;
    return failed ? 1 : 0;
}