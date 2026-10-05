/*
 * hosttests/cases/test_aarch64_arch_vmm_init.c — aarch64 M3.5 Task 24
 * RED test: arch_vmm_init() production entry contract
 * (kernel/arch/aarch64/memory/vmm_backend.c).
 *
 * arch_vmm_init() locates the installed M1 root via aarch64_read_ttbr1,
 * extracts the base PA with AARCH64_TTBR_BASE_MASK, validates it
 * (nonzero, 4 KiB aligned, < 1 TiB), calls aarch64_pt_init_locks()
 * (idempotent static-locks double insurance, Task 17 §5.4), publishes
 * the PA via aarch64_pt_root_publish (idempotent — boot_direct_map.c
 * already published), and pins kernel_map = (mmap)(pa + ARCH_PAGE_OFFSET).
 *
 * Cases per the brief:
 *   1. valid ttbr1 (nonzero, 4 KiB aligned, < 1 TiB) → rc = 0,
 *      kernel_map == (mmap)(pa + ARCH_PAGE_OFFSET).
 *   2. pa == 0  (raw == 0) → -EINVAL, kernel_map untouched.
 *   3. pa == 0  via permitted non-base bits only (masked base = 0)
 *      → -EINVAL, kernel_map untouched.
 *   4. PA at 1 TiB boundary (raw value's bit 40 set) — the
 *      AARCH64_TTBR_BASE_MASK strips bit 40, so the masked base is 0
 *      and the existing pa == 0 check rejects it. The contract
 *      observable here is -EINVAL + kernel_map untouched.
 *   5. PA above 1 TiB with the same bit 12 set so the mask still
 *      leaves a non-zero base — exposes whether the PA < 1 TiB
 *      check actually triggers. (See note below on the mask width.)
 *
 * NOTE on the unaligned-PA case: AARCH64_TTBR_BASE_MASK clears bits
 * [11:0] of the raw value, so any masked base is automatically 4 KiB
 * aligned. The `(pa & (PAGE_4K_SIZE - 1)) != 0` validation in
 * arch_vmm_init is therefore unreachable from the production call
 * path; we drop the "unaligned PA" case the brief lists because it
 * cannot be triggered through the documented entry contract.
 *
 * NOTE on case 5 and the AARCH64_TTBR_BASE_MASK width: the literal
 * in kernel/include/arch/aarch64/page_table.h is
 * `UINT64_C(0x000000fffffff000)` — only 7 f's (bits 12-39, 28 bits
 * wide), NOT the [47:12] window the header comment claims. The
 * production mask therefore drops bit 40 (the top of the 40-bit PA
 * space), so a raw value with bits 40 and 12 set becomes PA = 0x1000
 * after masking — nonzero, 4 KiB aligned, < 1 TiB — and arch_vmm_init
 * accepts it. Case 5's assertion `rc == -EINVAL` therefore RED's on
 * the current code: the kernel mask needs widening to bits [47:12]
 * (literal `UINT64_C(0x0000fffffffff000)`). This test pins the bug
 * so the next increment can land the fix; without case 5 the PA <
 * 1 TiB invariant has no host-side coverage.
 *
 * Link strategy: real page_table.c + real vmm_backend.c, mocks for
 * aarch64_read_ttbr1 (configurable via g_mock_ttbr1), vmm_gate_check
 * (no-op), aarch64_pt_root_publish (no-op returning true). The real
 * aarch64_pt_init_locks is called from arch_vmm_init; it spin_init's
 * the static pt_locks / pt_upper_lock arrays (both already initialised
 * to .lock = 1UL at file scope — idempotent re-write).
 *
 * arch_vmm_init does NOT allocate any pages — no PMM pool mock needed.
 *
 * Pre-SMP gate is implicitly satisfied on host (single-threaded
 * runner); the production vmm_gate.c is not linked here.
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

/* ── Configurable mock aarch64_read_ttbr1 ──────────────────────────
 * arch_vmm_init reads the live TTBR1 once at entry; we expose a
 * per-test settable global so each test case can inject a different
 * raw value (including edge cases at the validation boundary).
 *
 * The raw value is the entire 64-bit TTBR1_EL1 read — including the
 * permitted non-base bits (ASID [63:48], CnP [0]) which
 * AARCH64_TTBR_BASE_MASK strips before validation. */
static uint64_t g_mock_ttbr1 = 0;
uint64_t aarch64_read_ttbr1(void) { return g_mock_ttbr1; }

/* vmm_gate_check stub — production vmm_gate.c is too heavy (pulls in
 * percpu_data[] + dtb_cpu_count()) and is not linked here. Pre-SMP
 * gate is implicitly satisfied on the host's single thread. */
void vmm_gate_check(void) { (void)0; }

/* aarch64_pt_root_publish stub — the real vmm_gate.c isn't linked.
 * arch_vmm_init expects a successful idempotent re-publish. */
bool aarch64_pt_root_publish(uint64_t root_pa)
{
    (void)root_pa;
    return true;
}

/* aarch64_pt_root_is_published stub — not exercised by arch_vmm_init
 * (page_table.c's split_block_2m references it; kept for symbol
 * completeness in case future tests join this link line). */
bool aarch64_pt_root_is_published(const uint64_t *root)
{
    (void)root;
    return false;
}

/* Override the weak vmm_gate_violation (default: spin forever) with
 * a counter so any stray violation surfaces as a test failure rather
 * than a hang. */
static int g_violation_count;
void vmm_gate_violation(const char *reason)
{
    (void)reason;
    g_violation_count++;
}

/* alloc_4k_page / free_4k_page stubs — referenced by page_table.c
 * (ensure_child_table / split_block_2m) which this binary links.
 * arch_vmm_init never allocates, but the linker must resolve these
 * symbols. Return 0 on alloc (no allocations) so any stray call
 * surfaces as NULL rather than silently succeeding. */
uint64_t alloc_4k_page(void) { return 0; }
void free_4k_page(uint64_t phys) { (void)phys; }

/* ── Test failure counter (independent of __test_stats) ───────────
 * The framework's TEST_RESULTS macro zeroes __test_stats.* at the
 * end of RUN_ALL_TESTS, so a post-RUN check on __test_stats.failed
 * always sees 0. Track failures ourselves so the test binary exits
 * non-zero when any assertion fails — the hosttests Makefile run
 * recipe counts suite-level non-zero exits as test failures. */
static int g_test_failed_count = 0;

/* ── Test constants ──────────────────────────────────────────────── */
#define VALID_PA      UINT64_C(0x40200000)   /* low M1 arena window */
#define VALID_TTBR1   VALID_PA               /* no ASID, no CnP */
#define AT_1TIB_TTBR1 UINT64_C(0x10000000000) /* base bit 40 set */
#define ABOVE_1TIB_TTBR1 UINT64_C(0x10000001000) /* bit 40 + bit 12 */

/* ── Tests ────────────────────────────────────────────────────────── */

/* Custom assertion macro that increments g_test_failed_count on
 * failure. Mirrors the framework's assert_eq / assert_true shape but
 * survives TEST_RESULTS's __test_stats zeroing (see counter comment
 * above). The framework's macros are still useful for printing
 * failure context; this wrapper layers the persistent counter on
 * top.
 *
 * The framework macros use a do-while wrapper that increments
 * __test_stats.total unconditionally. Our wrapper replicates that
 * shape and re-checks the comparison so g_test_failed_count sticks
 * on failure (unlike a naive `++around_assert` pattern that always
 * decrements). */
#define ASSERT_EQ_TRACKED(expected, actual) do { \
    __test_stats.total++; \
    if ((long)(expected) != (long)(actual)) { \
        __test_stats.failed++; \
        g_test_failed_count++; \
        printf("  [FAIL] %s:%d: assert_eq(" #expected "=%ld, " #actual "=%ld)\n", \
               __FILE__, __LINE__, (long)(expected), (long)(actual)); \
    } else { \
        __test_stats.passed++; \
    } \
} while(0)
#define ASSERT_TRUE_TRACKED(cond) do { \
    __test_stats.total++; \
    if (!(cond)) { \
        __test_stats.failed++; \
        g_test_failed_count++; \
        printf("  [FAIL] %s:%d: assert_true(%s)\n", __FILE__, __LINE__, #cond); \
    } else { \
        __test_stats.passed++; \
    } \
} while(0)

/* Case 1: well-formed TTBR1 → rc = 0, kernel_map == (mmap)(pa + OFFSET). */
TEST_FUNC(test_valid_ttbr1_pins_kernel_map)
{
    g_violation_count = 0;
    kernel_map = NULL;
    g_mock_ttbr1 = VALID_TTBR1;
    int rc = arch_vmm_init();
    ASSERT_EQ_TRACKED(0, rc);
    /* Spec §4.5 / vmm_backend.c::arch_vmm_init: kernel_map is set to
     * the direct-map pointer of the PA extracted from TTBR1. */
    ASSERT_TRUE_TRACKED(kernel_map ==
                (uint64_t *)(uintptr_t)(VALID_PA + ARCH_PAGE_OFFSET));
    /* Happy path must not trip the gate violation hook. */
    ASSERT_EQ_TRACKED(0, g_violation_count);
}

/* Case 2a: raw TTBR1 == 0 → masked PA == 0 → -EINVAL. */
TEST_FUNC(test_pa_zero_raw_zero_returns_einval)
{
    g_violation_count = 0;
    kernel_map = NULL;
    g_mock_ttbr1 = 0;
    int rc = arch_vmm_init();
    ASSERT_EQ_TRACKED(-EINVAL, rc);
    /* Spec §4.5: kernel_map untouched on the failure path. */
    ASSERT_EQ_TRACKED(NULL, kernel_map);
}

/* Case 2b: raw TTBR1 has non-base bits but masked PA == 0.
 * AARCH64_TTBR_BASE_MASK = 0x000000fffffff000; only bits [47:12]
 * survive. A raw value with bits outside the mask but no base bits
 * yields PA = 0 → -EINVAL. We use 0x0000_0000_0000_0FFF (only
 * low non-base bits set, masked PA = 0). */
TEST_FUNC(test_pa_zero_mask_only_nonbase_returns_einval)
{
    g_violation_count = 0;
    kernel_map = NULL;
    g_mock_ttbr1 = UINT64_C(0x0000000000000FFF); /* base mask → 0 */
    int rc = arch_vmm_init();
    ASSERT_EQ_TRACKED(-EINVAL, rc);
    ASSERT_EQ_TRACKED(NULL, kernel_map);
}

/* Case 4: PA exactly at the 1 TiB limit (>= 1 TiB) → -EINVAL.
 * The spec's bound is "PA < 1 TiB" (PA_LIMIT = 0x10000000000); PA
 * == 1 TiB is rejected. The current AARCH64_TTBR_BASE_MASK drops
 * bit 40 (see file header note), so the masked base is 0 and the
 * pa == 0 check rejects it via a different code path; the
 * observable contract is still -EINVAL + kernel_map untouched. */
TEST_FUNC(test_pa_at_1tib_boundary_returns_einval)
{
    g_violation_count = 0;
    kernel_map = NULL;
    g_mock_ttbr1 = AT_1TIB_TTBR1;
    int rc = arch_vmm_init();
    ASSERT_EQ_TRACKED(-EINVAL, rc);
    ASSERT_EQ_TRACKED(NULL, kernel_map);
}

/* Case 5: PA above 1 TiB with the low 4 KiB bit set, so the
 * current (buggy) AARCH64_TTBR_BASE_MASK leaves a non-zero base
 * after masking. This pins the PA < 1 TiB invariant end-to-end.
 * On the current (buggy) mask this case RED's — the kernel mask
 * needs widening to bits [47:12] (9 f's) for this assertion to
 * pass. */
TEST_FUNC(test_pa_above_1tib_returns_einval)
{
    g_violation_count = 0;
    kernel_map = NULL;
    g_mock_ttbr1 = ABOVE_1TIB_TTBR1;
    int rc = arch_vmm_init();
    ASSERT_EQ_TRACKED(-EINVAL, rc);
    ASSERT_EQ_TRACKED(NULL, kernel_map);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_valid_ttbr1_pins_kernel_map),
    TEST_ENTRY(test_pa_zero_raw_zero_returns_einval),
    TEST_ENTRY(test_pa_zero_mask_only_nonbase_returns_einval),
    TEST_ENTRY(test_pa_at_1tib_boundary_returns_einval),
    TEST_ENTRY(test_pa_above_1tib_returns_einval),
TEST_LIST_END

int main(void)
{
    /* mmap the kernel half window so PA + ARCH_PAGE_OFFSET resolves
     * through the host's direct-map. The aarch64 test harness's
     * <arch/mmu.h> defines ARCH_PAGE_OFFSET = 0x100000000; the
     * kernel-half mmap below satisfies that. <memory/vmm.h>
     * `#define`s `mmap` as a macro (the kernel's typed page-table
     * root pointer), which collides with the host libc mmap(2) call.
     * Undef the macro here so the system mmap is callable; the
     * kernel type is still available via kernel_map's declaration
     * in vmm_backend.c. */
    #undef mmap
    void *base = mmap((void *)(uintptr_t)ARCH_PAGE_OFFSET,
                      0x200000ULL,                          /* 2 MiB window */
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                      -1, 0);
    if (base == MAP_FAILED) return 2;
    memset(base, 0, 0x200000ULL);

    /* Reset kernel_map + counters so any stray call before a test
     * starts fails cleanly. */
    kernel_map = NULL;
    g_mock_ttbr1 = 0;
    g_violation_count = 0;
    g_test_failed_count = 0;

    RUN_ALL_TESTS();

    /* No vmm_gate_violation should have fired on any of the above
     * paths — arch_vmm_init never reaches the violation hook on
     * its documented paths. A stray would surface as the test
     * exit code rather than a hang. g_test_failed_count survives
     * the framework's TEST_RESULTS zeroing (see comment above);
     * __test_stats.failed does not. */
    int failed = (g_test_failed_count > 0) || (g_violation_count > 0);

    kernel_map = NULL;
    return failed ? 1 : 0;
}
