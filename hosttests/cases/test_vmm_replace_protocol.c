/*
 * hosttests/cases/test_vmm_replace_protocol.c — aarch64 M3.3 Task 19
 * 4-class replace protocol matrix (spec §4.4.3) with descriptor-level
 * state assertions.
 *
 * Background:
 *   Task 16 (test_aarch64_pt_software_bits.c) already pins the
 *   *branch counter* for each of the 4 classes via weak hooks
 *   (`aarch64_pt_test_note_atomic_replace` / `_bbm_replace`) and
 *   verifies the final descriptor state for perm-only, PA-change,
 *   and PROTNONE-flip (both directions). What was NOT covered:
 *
 *     - memtype-flip (AttrIndx Normal↔Device, spec §4.4.3 row 2)
 *       — deferred by Task 16 because the test mock's PA pool
 *         sits at 0x10000 and the test never exercised a Device
 *         PA. Spec F11 requires Device-window PA per §4.4.3 alias
 *         constraint, so this test uses PA = 0x10000000 (outside
 *         the mock's normal pool).
 *     - the 4-class matrix via the *backend* layer
 *       (arch_vmm_update_4k routing) — Task 18 already covered
 *       the 12-legal combo matrix via arch_vmm_map_4k_new, but
 *       did not pin the update-classification branch counters
 *       through the backend. A regression in vmm_backend.c's
 *       vm_to_perm path that silently dropped VM_NOCACHE would
 *       mis-classify a memtype change as perm-only and route to
 *       the atomic-store branch — a 4 KiB page would be silently
 *       aliased at two memory types without BBM.
 *
 *   Test 16's cases 7 and 8 cover rows 1, 3, and 4 of the spec's
 *   classification table. This file adds:
 *
 *     - row 2 (memtype-flip, AttrIndx change) via primitive
 *     - row 1 (perm-only) routed through arch_vmm_update_4k
 *     - row 3 (PA-change) routed through arch_vmm_update_4k
 *     - row 4 (PROTNONE stash + recovery) routed through
 *       arch_vmm_update_4k
 *
 *   "intermediate-state assertion" caveat:
 *     The spec's PTE-level BBM sequence (clear → dsb → tlbi → set
 *     → dsb → tlbi) opens a window where *pte == 0. Single-threaded
 *     host tests cannot observe that window — the producer thread
 *     never re-enters while the atomic-clear / atomic-set is in
 *     flight. The "mid-state" is therefore only observable in a
 *     multi-CPU test (covered by §8.2 QEMU multi-core selftest,
 *     not here). What THIS test CAN assert is:
 *
 *       - the leaf before the call holds the prior descriptor
 *       - the leaf after the call holds the new descriptor
 *       - the branch counter pin (atomic vs bbm) matches the
 *         classification per spec §4.4.3
 *       - the query returns the FINAL state per spec §4.3
 *
 *     The branch counter pin is the strongest single-threaded
 *     guarantee that the BBM sequence was actually executed.
 *
 * Link strategy: compile the REAL page_table.c AND vmm_backend.c
 * (brief: 链接生产 page_table.c + vmm_backend.c). Mock vmm_gate,
 * alloc/free, aarch64_read_ttbr1 — same harness as Task 18. The
 * backend layer additionally needs kernel_map pinned to the test
 * root (mirrors test_aarch64_backend_4k.c).
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
 * Same harness as test_aarch64_pt_software_bits.c.  Pool stride 0x1000
 * mirrors real 4 KiB pages so pa + ARCH_PAGE_OFFSET resolves through
 * the mmap'd kernel-half window.  Pool size 32 leaves room for the
 * walk_to_l3 + intermediate-table pattern the backend exercises. */
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

/* ── Mocks for symbols page_table.c / vmm_backend.c reference ─────── */
uint64_t aarch64_read_ttbr1(void) { return 0; }    /* not active */

/* page_table.c + vmm_backend.c call vmm_gate_check() on every public
 * entry. The production vmm_gate.c is not linked in this test (would
 * pull in percpu_data[] / dtb_cpu_count() mocks), so we stub it
 * directly. */
void vmm_gate_check(void) { (void)0; }
/* page_table.c calls aarch64_pt_root_is_published() inside the new
 * split_block_2m path (Task 21).  These tests do NOT link the REAL
 * vmm_gate.c (it would pull in percpu_data[] / dtb_cpu_count() mocks
 * they do not need), so we stub it directly.  Returning false means
 * split always proceeds to the unpublished-root branch — same
 * behaviour the production code has on a freshly-allocated scratch
 * root. */
bool aarch64_pt_root_is_published(const uint64_t *root) { (void)root; return false; }

/* Override the weak vmm_gate_violation (default spins forever) with
 * a counter so any stray violation surfaces as a test failure rather
 * than a hang. */
static int g_violation_count;
void vmm_gate_violation(const char *reason)
{
    (void)reason;
    g_violation_count++;
}

/* arch_vmm_init() routes through aarch64_pt_root_publish (vmm_gate.c);
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

/* ── Branch-instrumentation overrides ─────────────────────────────
 * Counters let each test start fresh and assert exactly which branch
 * fired for which classification (spec §4.4.3).  Production builds
 * link the weak no-op defaults — same pattern test_aarch64_pt_software
 * bits.c uses. */
static int g_atomic_count;
static int g_bbm_count;
void aarch64_pt_test_note_atomic_replace(void) { g_atomic_count++; }
void aarch64_pt_test_note_bbm_replace(void)   { g_bbm_count++; }

static void reset_branch_counters(void)
{
    g_atomic_count = 0;
    g_bbm_count = 0;
}

/* ── Test helpers ─────────────────────────────────────────────────
 * Each test allocates a fresh root from the mock pool, mmaps the
 * kernel-half window for direct-mapped access, and assigns the
 * backend's `kernel_map` to the new root.  Two VA bases (one for
 * primitive-only tests, one for backend tests) avoid the rare case
 * where the same VA is still occupied across tests. */
#define TEST_VA_PRIM   0xffff800000200000ULL
#define TEST_VA_BACK   0xffff800000300000ULL
#define TEST_VA_MEMT   0xffff800000400000ULL   /* memtype-flip */

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

/* Descriptor bit constants (mirror page_table.c's locals). */
#define TEST_DESC_VALID UINT64_C(0x001)
#define TEST_DESC_TABLE UINT64_C(0x002)
#define TEST_DESC_AF    UINT64_C(0x400)
#define TEST_DESC_USER  UINT64_C(0x040)
#define TEST_DESC_RO    UINT64_C(0x080)
#define TEST_DESC_PXN   UINT64_C(0x20000000000000)
#define TEST_DESC_UXN   UINT64_C(0x40000000000000)
#define TEST_PA_MASK    UINT64_C(0xffffffffff000)

/* Memtype-flip PA: spec F11 requires the new PA to be outside any
 * Normal direct-map alias.  The mock's Normal window is 0..0x20000
 * (pool + a few pages of slack).  0x10000000 = 256 MiB, well outside
 * the Normal alias range; the descriptor-level test never actually
 * accesses the PA — it only inspects the AttrIndx bit. */
#define TEST_DEVICE_PA 0x10000000ULL
#define TEST_DEVICE_PA2 0x11000000ULL

/* ── Tests ────────────────────────────────────────────────────────── */

/* Row 1 (perm-only) via the backend: arch_vmm_update_4k from
 * VM_KERNEL_RW to VM_KERNEL_RO.  Same PA, same memory type, same
 * validity → atomic-store fast path.  Test 16 covered this at the
 * primitive level; this case reasserts the same classification at
 * the backend layer so a vmm_backend.c regression that swaps
 * vm_to_perm results would surface. */
TEST_FUNC(test_backend_class1_perm_only_routes_to_atomic)
{
    mock_pool_reset();
    reset_branch_counters();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    uint64_t data_pa = 0x5000ULL;
    int rc = arch_vmm_map_4k_new(root, data_pa, TEST_VA_BACK, VM_KERNEL_RW);
    assert_eq(0, rc);

    /* Before-update query to confirm initial state. */
    uint64_t qpa_before = 0;
    uint32_t qvm_before = 0;
    rc = arch_vmm_query_4k(root, TEST_VA_BACK, &qpa_before, &qvm_before);
    assert_eq(0, rc);
    assert_eq(data_pa, qpa_before);
    assert_true((qvm_before & VM_PRESENT) != 0);
    assert_true((qvm_before & VM_WRITE) != 0);

    reset_branch_counters();
    uint64_t old_pa = 0;
    uint32_t old_vm = 0;
    rc = arch_vmm_update_4k(root, data_pa, TEST_VA_BACK, VM_KERNEL_RO,
                            &old_pa, &old_vm);
    assert_eq(0, rc);
    assert_eq(data_pa, old_pa);

    /* Spec §4.4.3 row 1: perm-only → atomic store fast path. */
    assert_eq(1, g_atomic_count);
    assert_eq(0, g_bbm_count);

    /* After-update query confirms the new perm. */
    uint64_t qpa_after = 0;
    uint32_t qvm_after = 0;
    rc = arch_vmm_query_4k(root, TEST_VA_BACK, &qpa_after, &qvm_after);
    assert_eq(0, rc);
    assert_eq(data_pa, qpa_after);
    assert_true((qvm_after & VM_PRESENT) != 0);
    assert_true((qvm_after & VM_WRITE) == 0);   /* now RO */
    assert_eq(0, g_free_calls);
}

/* Row 3 (PA-change) via the backend: arch_vmm_update_4k from PA1
 * to PA2.  Different PA → same_pa=false → BBM. */
TEST_FUNC(test_backend_class3_pa_change_routes_to_bbm)
{
    mock_pool_reset();
    reset_branch_counters();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    uint64_t data_pa1 = 0x6000ULL;
    uint64_t data_pa2 = 0x7000ULL;

    int rc = arch_vmm_map_4k_new(root, data_pa1, TEST_VA_BACK, VM_KERNEL_RW);
    assert_eq(0, rc);

    reset_branch_counters();
    uint64_t old_pa = 0;
    uint32_t old_vm = 0;
    rc = arch_vmm_update_4k(root, data_pa2, TEST_VA_BACK, VM_KERNEL_RW,
                            &old_pa, &old_vm);
    assert_eq(0, rc);
    assert_eq(data_pa1, old_pa);

    /* Spec §4.4.3 row 3: PA change → BBM. */
    assert_eq(0, g_atomic_count);
    assert_eq(1, g_bbm_count);

    uint64_t qpa = 0;
    uint32_t qvm = 0;
    rc = arch_vmm_query_4k(root, TEST_VA_BACK, &qpa, &qvm);
    assert_eq(0, rc);
    assert_eq(data_pa2, qpa);
}

/* Row 4 (PROTNONE stash) via the backend: arch_vmm_update_4k from
 * VM_KERNEL_RW to VM_PROTNONE.  Same PA, same memtype, but validity
 * flips (VALID → no-VALID + PROTNONE software bit) → same_valid=false
 * → BBM.  After update, query returns -EPROT_NONE with the same PA
 * and VM_PROTNONE in vm_out. */
TEST_FUNC(test_backend_class4a_protnone_stash_routes_to_bbm)
{
    mock_pool_reset();
    reset_branch_counters();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    uint64_t data_pa = 0x8000ULL;
    int rc = arch_vmm_map_4k_new(root, data_pa, TEST_VA_BACK, VM_KERNEL_RW);
    assert_eq(0, rc);

    reset_branch_counters();
    uint64_t old_pa = 0;
    uint32_t old_vm = 0;
    /* Backend's vm_to_perm refuses VM_PROTNONE without VM_PRESENT
     * (the legal "invalid but holds PA" combo); vm_to_sw reports
     * the PROTNONE bit.  The primitive then writes the descriptor
     * with VALID cleared + PROTNONE software bit set. */
    rc = arch_vmm_update_4k(root, data_pa, TEST_VA_BACK, VM_PROTNONE,
                            &old_pa, &old_vm);
    assert_eq(0, rc);
    assert_eq(data_pa, old_pa);
    /* old_vm should carry the prior perm — VM_KERNEL_RW minus the
     * PROTNONE bit (since the prior slot was VALID).  Backend's
     * perm_to_vm re-derives this. */
    assert_true((old_vm & VM_PRESENT) != 0);

    /* Spec §4.4.3 row 4: validity flip → BBM. */
    assert_eq(0, g_atomic_count);
    assert_eq(1, g_bbm_count);

    /* Spec §4.3 query: PROTNONE stash → -EPROT_NONE + valid phys. */
    uint64_t qpa = 0;
    uint32_t qvm = 0;
    rc = arch_vmm_query_4k(root, TEST_VA_BACK, &qpa, &qvm);
    assert_eq(AARCH64_PT_EPROT_NONE, rc);
    assert_eq(data_pa, qpa);
    assert_true((qvm & VM_PROTNONE) != 0);
    assert_true((qvm & VM_PRESENT) == 0);
    assert_eq(0, g_free_calls);
}

/* Row 4 (PROTNONE recovery) via the backend: arch_vmm_update_4k from
 * VM_PROTNONE back to VM_KERNEL_RW.  Validity flips back → BBM. */
TEST_FUNC(test_backend_class4b_protnone_recovery_routes_to_bbm)
{
    mock_pool_reset();
    reset_branch_counters();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    uint64_t data_pa = 0x9000ULL;
    int rc = arch_vmm_map_4k_new(root, data_pa, TEST_VA_BACK, VM_PROTNONE);
    assert_eq(0, rc);

    reset_branch_counters();
    uint64_t old_pa = 0;
    uint32_t old_vm = 0;
    rc = arch_vmm_update_4k(root, data_pa, TEST_VA_BACK, VM_KERNEL_RW,
                            &old_pa, &old_vm);
    assert_eq(0, rc);
    assert_eq(data_pa, old_pa);
    /* old_vm is the prior decoded perm: PROTNONE stash.  After
     * Task 19 Fix round 1, perm_to_vm clears VM_PRESENT when the
     * PROTNONE software bit is set, so old_vm here is
     * VM_PROTNONE (no VM_PRESENT) — matching the spec-correct
     * representation that arch_vmm_query_4k returns for the same
     * slot.  This is the load-bearing half of the
     * VMA-prot save/restore contract. */
    assert_true((old_vm & VM_PROTNONE) != 0);
    assert_true((old_vm & VM_PRESENT) == 0);

    assert_eq(0, g_atomic_count);
    assert_eq(1, g_bbm_count);

    uint64_t qpa = 0;
    uint32_t qvm = 0;
    rc = arch_vmm_query_4k(root, TEST_VA_BACK, &qpa, &qvm);
    assert_eq(0, rc);
    assert_eq(data_pa, qpa);
    assert_true((qvm & VM_PRESENT) != 0);
    assert_true((qvm & VM_WRITE) != 0);
    assert_true((qvm & VM_PROTNONE) == 0);
}

/* Row 2 (memtype-flip NORMAL → DEVICE) via the primitive: this is the
 * gap Task 16 deferred.  Spec F11 requires Device-window PA per §4.4.3
 * alias constraint; we use PA = 0x10000000 (outside the mock's normal
 * pool).  The descriptor never accesses the PA in the host test — only
 * the AttrIndx bit is checked. */
TEST_FUNC(test_primitive_class2_memtype_flip_routes_to_bbm)
{
    mock_pool_reset();
    reset_branch_counters();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    /* Map a Normal KERNEL_RW leaf at TEST_VA_MEMT with a normal-window
     * PA.  Then call aarch64_pt_replace_4k to flip memtype (Normal →
     * Device) AND perm (RW → RO, allowed within row 2 because the
     * memtype change dominates the classification).
     *
     * In the brief's wording, memtype-flip "must route to BBM" —
     * that's the contract.  We also vary perm to verify the
     * classification is on memtype, not on perm alone. */
    uint64_t pa_normal = 0xa000ULL;
    int rc = aarch64_pt_map_4k_ext(root, TEST_VA_MEMT, pa_normal,
                                   AARCH64_PT_KERNEL_RW, 0);
    assert_eq(0, rc);

    /* Pre-replace: descriptor carries the Normal AttrIndx (0x004 =
     * AttrIdx 1 = WBWA). */
    uint64_t pa_before = 0;
    uint32_t perm_before = 0;
    uint64_t sw_before = 0;
    rc = aarch64_pt_query_4k_ext(root, TEST_VA_MEMT,
                                 &pa_before, &perm_before, &sw_before);
    assert_eq(0, rc);
    assert_eq(pa_normal, pa_before);
    assert_true((perm_before & AARCH64_PT_DEVICE) == 0);   /* not device */

    /* Replace to Device KERNEL_RO at the Device-window PA. */
    reset_branch_counters();
    uint64_t old_pa = 0;
    uint32_t old_perm = 0;
    uint64_t old_sw = 0;
    rc = aarch64_pt_replace_4k(root, TEST_VA_MEMT, TEST_DEVICE_PA,
                               AARCH64_PT_DEVICE | AARCH64_PT_KERNEL_RO,
                               0,
                               &old_pa, &old_perm, &old_sw);
    assert_eq(0, rc);
    assert_eq(pa_normal, old_pa);

    /* Spec §4.4.3 row 2: memtype change → BBM (perm change alone would
     * have taken the atomic path — the spec's classifier picks the
     * strongest constraint). */
    assert_eq(0, g_atomic_count);
    assert_eq(1, g_bbm_count);

    /* Post-replace: descriptor carries Device (AttrIndx 0), and PA
     * is the Device-window value. */
    uint64_t pa_after = 0;
    uint32_t perm_after = 0;
    uint64_t sw_after = 0;
    rc = aarch64_pt_query_4k_ext(root, TEST_VA_MEMT,
                                 &pa_after, &perm_after, &sw_after);
    assert_eq(0, rc);
    assert_eq(TEST_DEVICE_PA, pa_after);
    assert_true((perm_after & AARCH64_PT_DEVICE) != 0);     /* device */
    assert_true((perm_after & AARCH64_PT_KERNEL_RO) != 0);   /* RO */
}

/* Reverse memtype-flip (DEVICE → NORMAL) at the primitive level:
 * mirror of the Normal→Device case above.  Exercises the row-2 path
 * in the other direction. */
TEST_FUNC(test_primitive_class2_memtype_flip_back_to_normal)
{
    mock_pool_reset();
    reset_branch_counters();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    /* Start from a Device mapping at a Device-window PA. */
    int rc = aarch64_pt_map_4k_ext(root, TEST_VA_MEMT, TEST_DEVICE_PA2,
                                   AARCH64_PT_DEVICE | AARCH64_PT_KERNEL_RW,
                                   0);
    assert_eq(0, rc);

    reset_branch_counters();
    uint64_t old_pa = 0;
    uint32_t old_perm = 0;
    uint64_t old_sw = 0;
    uint64_t pa_normal2 = 0xb000ULL;
    rc = aarch64_pt_replace_4k(root, TEST_VA_MEMT, pa_normal2,
                               AARCH64_PT_KERNEL_RW, 0,
                               &old_pa, &old_perm, &old_sw);
    assert_eq(0, rc);
    assert_eq(TEST_DEVICE_PA2, old_pa);

    /* Spec §4.4.3 row 2 again: memtype flip → BBM. */
    assert_eq(0, g_atomic_count);
    assert_eq(1, g_bbm_count);

    uint64_t pa_after = 0;
    uint32_t perm_after = 0;
    rc = aarch64_pt_query_4k_ext(root, TEST_VA_MEMT,
                                 &pa_after, &perm_after, NULL);
    assert_eq(0, rc);
    assert_eq(pa_normal2, pa_after);
    assert_true((perm_after & AARCH64_PT_DEVICE) == 0);     /* back to Normal */
}

/* Row 2 also via the backend: arch_vmm_update_4k from VM_KERNEL_RW
 * to VM_DEVICE.  This is the realistic caller path (mprotect-style
 * cache-control flip).  Pin the backend's classification routes to
 * the primitive's BBM branch. */
TEST_FUNC(test_backend_class2_memtype_flip_routes_to_bbm)
{
    mock_pool_reset();
    reset_branch_counters();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    uint64_t pa_normal = 0xc000ULL;
    int rc = arch_vmm_map_4k_new(root, pa_normal, TEST_VA_BACK, VM_KERNEL_RW);
    assert_eq(0, rc);

    reset_branch_counters();
    uint64_t old_pa = 0;
    uint32_t old_vm = 0;
    rc = arch_vmm_update_4k(root, TEST_DEVICE_PA, TEST_VA_BACK, VM_DEVICE,
                            &old_pa, &old_vm);
    assert_eq(0, rc);
    assert_eq(pa_normal, old_pa);

    /* Spec §4.4.3 row 2: memtype flip → BBM. */
    assert_eq(0, g_atomic_count);
    assert_eq(1, g_bbm_count);

    uint64_t qpa = 0;
    uint32_t qvm = 0;
    rc = arch_vmm_query_4k(root, TEST_VA_BACK, &qpa, &qvm);
    assert_eq(0, rc);
    assert_eq(TEST_DEVICE_PA, qpa);
    assert_true((qvm & VM_NOCACHE) != 0);
    assert_true((qvm & VM_NO_EXEC) != 0);    /* DEVICE forces NX */
}

/* Row 1 (perm-only) via the primitive: maps then replaces from
 * KERNEL_RW to KERNEL_RO.  This is the same contract as Test 16's
 * case 7 — reasserted here so the 4-class matrix is self-contained
 * in this file (a regression in case-7's helper code would also
 * surface here).  Documented as "duplicate by design" — the test
 * is the canonical assertion of the atomic-store fast path. */
TEST_FUNC(test_primitive_class1_perm_only_routes_to_atomic)
{
    mock_pool_reset();
    reset_branch_counters();
    uint64_t root_pa;
    uint64_t *root = fresh_root_and_set_kernel_map(&root_pa);
    assert_not_null(root);

    uint64_t data_pa = 0xd000ULL;
    int rc = aarch64_pt_map_4k_ext(root, TEST_VA_PRIM, data_pa,
                                   AARCH64_PT_KERNEL_RW, 0);
    assert_eq(0, rc);

    /* Before-update query to confirm the descriptor holds
     * KERNEL_RW (AP[2:1] = 00, no RO bit). */
    uint64_t pa_before = 0;
    uint32_t perm_before = 0;
    rc = aarch64_pt_query_4k_ext(root, TEST_VA_PRIM,
                                 &pa_before, &perm_before, NULL);
    assert_eq(0, rc);
    assert_eq(data_pa, pa_before);
    assert_true((perm_before & AARCH64_PT_KERNEL_RW) != 0);
    assert_true((perm_before & AARCH64_PT_KERNEL_RO) == 0);

    reset_branch_counters();
    uint64_t old_pa = 0;
    uint32_t old_perm = 0;
    uint64_t old_sw = 0;
    rc = aarch64_pt_replace_4k(root, TEST_VA_PRIM, data_pa,
                               AARCH64_PT_KERNEL_RO, 0,
                               &old_pa, &old_perm, &old_sw);
    assert_eq(0, rc);
    assert_eq(data_pa, old_pa);
    assert_eq((uint32_t)AARCH64_PT_KERNEL_RW, old_perm);

    /* Spec §4.4.3 row 1: perm-only → atomic-store fast path. */
    assert_eq(1, g_atomic_count);
    assert_eq(0, g_bbm_count);

    /* After-update query confirms KERNEL_RO (AP[2] = 1, the RO bit). */
    uint64_t pa_after = 0;
    uint32_t perm_after = 0;
    rc = aarch64_pt_query_4k_ext(root, TEST_VA_PRIM,
                                 &pa_after, &perm_after, NULL);
    assert_eq(0, rc);
    assert_eq(data_pa, pa_after);
    assert_true((perm_after & AARCH64_PT_KERNEL_RO) != 0);
    assert_true((perm_after & AARCH64_PT_KERNEL_RW) == 0);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_backend_class1_perm_only_routes_to_atomic),
    TEST_ENTRY(test_backend_class3_pa_change_routes_to_bbm),
    TEST_ENTRY(test_backend_class4a_protnone_stash_routes_to_bbm),
    TEST_ENTRY(test_backend_class4b_protnone_recovery_routes_to_bbm),
    TEST_ENTRY(test_primitive_class2_memtype_flip_routes_to_bbm),
    TEST_ENTRY(test_primitive_class2_memtype_flip_back_to_normal),
    TEST_ENTRY(test_backend_class2_memtype_flip_routes_to_bbm),
    TEST_ENTRY(test_primitive_class1_perm_only_routes_to_atomic),
TEST_LIST_END

int main(void)
{
    /* mmap the kernel half window so PA + ARCH_PAGE_OFFSET resolves.
     * <memory/vmm.h> defines `mmap` as a macro (the kernel's typed
     * page-table-root pointer), which collides with the host libc
     * mmap(2) call. Undef the macro here so the system mmap is
     * callable; the kernel type is still available via kernel_map's
     * declaration in vmm_backend.c. */
    #undef mmap
    void *base = mmap((void *)(uintptr_t)ARCH_PAGE_OFFSET,
                      0x200000ULL,                          /* 2 MiB window */
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                      -1, 0);
    if (base == MAP_FAILED) return 2;
    memset(base, 0, 0x200000ULL);

    /* Reset kernel_map + counters so any stray call before
     * fresh_root_and_set_kernel_map fails cleanly. */
    kernel_map = NULL;
    g_atomic_count = 0;
    g_bbm_count = 0;
    g_violation_count = 0;

    RUN_ALL_TESTS();

    /* No vmm_gate_violation should have fired on the happy paths. */
    int failed = (__test_stats.failed > 0) || (g_violation_count > 0);

    kernel_map = NULL;
    return failed ? 1 : 0;
}