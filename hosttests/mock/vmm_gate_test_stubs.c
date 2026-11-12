/* hosttests/mock/vmm_gate_test_stubs.c
 *
 * Link-time stubs for hosttests that compile production files calling
 * into kernel/arch/aarch64/memory/vmm_gate.c (e.g. test_m1_install
 * compiles boot_direct_map.c, whose M3 change calls
 * aarch64_pt_root_publish before the TTBR1 install).
 *
 * The vmm_gate production object is compiled against
 * mock/vmm_gate_test_runtime.h (16-byte percpu_t tail stub), so
 * percpu_data[] here uses that same type. The stub dtb_cpu_count
 * returns 1: vmm_gate_check() is a no-op pre-SMP (smp_starting == 0),
 * so the value is never consulted by these tests.
 */
#include "vmm_gate_test_runtime.h"

percpu_t percpu_data[NR_CPUS];

uint32_t dtb_cpu_count(void) { return 1; }

/* Default BSP. Tests that need a non-BSP caller set this directly. */
uint32_t g_test_cpu_id = 0;

/* ── M4 user-mm ownership stubs ──────────────────────────────────
 * vmm_backend.c's user-root branch calls into aarch64_user_mm_*;
 * production user_mm.c is kernel-only (aarch64 build). These stubs
 * default to "no user roots" so the existing kernel_map tests keep
 * their contracts. A test designates a managed root via
 * vmm_test_user_mm_set_owned_root() and controls allocated-page
 * bookkeeping via vmm_test_pmm_{reset,mark,unmark}. */

static uint64_t *g_test_owned_root;
static int g_test_root_busy;

void vmm_test_user_mm_set_owned_root(uint64_t *root)
{
    g_test_owned_root = root;
}

void vmm_test_user_mm_set_busy(int busy)
{
    g_test_root_busy = busy;
}

bool aarch64_user_mm_owns_root(const uint64_t *root)
{
    return root != NULL && (uint64_t *)root == g_test_owned_root;
}

bool aarch64_user_mm_root_busy(const uint64_t *root)
{
    (void)root;
    return g_test_root_busy != 0;
}

/* vmm_gate.c now defines this gate too, and some host tests link the
 * production object alongside this stub.  Keep the stub weak so a test
 * that links both resolves to the production definition, while
 * stub-only links (vmm_backend.c calls it from its user-root branch)
 * still get the "gate always open" default these kernel_map tests rely
 * on. */
__attribute__((weak)) int aarch64_user_mm_runtime_check(void)
{
    return 0;
}

#define VMM_TEST_PMM_MAX 64
static uint64_t g_test_pmm[VMM_TEST_PMM_MAX];
static unsigned g_test_pmm_n;

void vmm_test_pmm_reset(void) { g_test_pmm_n = 0; }

void vmm_test_pmm_mark(uint64_t pa)
{
    if (g_test_pmm_n < VMM_TEST_PMM_MAX)
        g_test_pmm[g_test_pmm_n++] = pa;
}

void vmm_test_pmm_unmark(uint64_t pa)
{
    for (unsigned i = 0; i < g_test_pmm_n; i++) {
        if (g_test_pmm[i] == pa) {
            g_test_pmm[i] = g_test_pmm[--g_test_pmm_n];
            return;
        }
    }
}

bool pmm_4k_page_allocated(uint64_t pa)
{
    for (unsigned i = 0; i < g_test_pmm_n; i++)
        if (g_test_pmm[i] == pa)
            return true;
    return false;
}
