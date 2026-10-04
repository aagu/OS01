/*
 * hosttests/cases/test_vmm_ack_wraparound.c — aarch64 M3.3 Task 19
 * Ack-generation algebraic wraparound contract (spec §6.4 step 4).
 *
 * Background:
 *   Spec §6.4 step 4 mandates the wait condition be `!=` rather than
 *   `<` to handle the 32-bit generation wraparound.  When gen = UINT32_MAX
 *   and the per-target inc is `gen + 1`, the resulting target wraps
 *   to 0; a `<` wait would treat 0 as "less than" any prior gen and
 *   exit immediately — a false-positive that masks lost acks.
 *
 *   Task 12's test_tlb_serial_protocol.c (case_wraparound, 5-case
 *   suite) already pins the happy-path wraparound (gen=UINT32_MAX
 *   → handler fires → gen=0 → BSP sees target=0 → returns OK).
 *   What is NOT pinned there is the *negative* counterpart: with
 *   the same wraparound snapshot but the handler STUCK, the `!=`
 *   wait must time out (FATAL) rather than exit early on `<`.
 *   That second half is the load-bearing half of the wraparound
 *   guarantee — a `<` regression would let the suite pass for the
 *   happy path AND the generic timeout case (case_timeout_fatal,
 *   which starts with gen=0 so wraparound doesn't apply), but
 *   silently mis-handle a real SMP shootdown against a CPU whose
 *   gen is at UINT32_MAX.
 *
 * This file covers BOTH halves on a fresh test root, linking the
 * REAL kernel/memory/tlb.c so the actual wait-loop (`!=`) and
 * timeout path are exercised — no source-scan fallback.
 *
 * Link strategy: same as test_tlb_serial_protocol.c — preinclude
 * mock/tlb_test_runtime.h short-circuits percpu/spinlock/mmu/cpu/
 * ipi headers; the test TU supplies percpu_data[], num_cpus, the
 * ipi_broadcast mock, the local-flush counter, and overrides the
 * weak tlb_shootdown_panic() hook with a longjmp capture.
 */

#include <test_framework.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Shared mock surface (NR_CPUS, percpu_t, spinlock/cycle/flush stubs). */
#include "../mock/tlb_test_runtime.h"

/* Production declaration (memory/vmm.h is too heavy for the host
 * mock); pin the signature here exactly as the kernel declares it. */
void tlb_shootdown(void);

/* ── Test-supplied mock state ─────────────────────────────────── */
percpu_t percpu_data[NR_CPUS];
uint32_t num_cpus = 1;
uint64_t tlb_mock_cycles = 0;
uint32_t tlb_mock_local_flushes = 0;

static uint32_t mock_ipi_vector;
static uint64_t mock_ipi_mask;
static int      mock_ipi_calls;
static int      ack_on_broadcast;       /* emulate per-target handler */

/* Mock IPI delivery.  When ack_on_broadcast == 1 we emulate the
 * handler running per target — atomic gen bump, same as the real
 * aarch64 / x86 TLB IPI handlers.  When ack_on_broadcast == 0, no
 * gen bump happens — the wait must spin to the deadline. */
void ipi_broadcast(uint32_t vector, uint64_t target_mask)
{
    mock_ipi_vector = vector;
    mock_ipi_mask = target_mask;
    mock_ipi_calls++;
    if (ack_on_broadcast) {
        for (uint32_t cpu = 0; cpu < NR_CPUS; cpu++)
            if (target_mask & (1UL << cpu))
                __atomic_fetch_add(&percpu_data[cpu].tlb_ack_gen, 1,
                                   __ATOMIC_RELEASE);
    }
}

/* Override the weak tlb_shootdown_panic hook (default spins
 * forever) with a longjmp capture — the test framework treats
 * the FATAL path as a normal case outcome. */
static jmp_buf panic_jb;
static int panic_armed;

void tlb_shootdown_panic(const char *reason)
{
    (void)reason;
    if (panic_armed) {
        panic_armed = 0;
        longjmp(panic_jb, 1);
    }
    fprintf(stderr, "tlb_shootdown_panic outside armed test\n");
    exit(2);
}

#define EXPECT_PANIC(stmt) __extension__ ({ \
    int _hit; \
    if (setjmp(panic_jb) == 0) { \
        panic_armed = 1; \
        stmt; \
        panic_armed = 0; \
        _hit = 0;                       /* no FATAL captured */ \
    } else { \
        _hit = 1;                       /* FATAL captured */ \
    } \
    _hit; })

/* Reset every piece of test-controllable state.  ack_on_broadcast=0
 * by default — caller opts into "handler fires" per phase. */
static void reset_mock(int ncpus)
{
    memset(percpu_data, 0, sizeof(percpu_data));
    num_cpus = ncpus;
    tlb_mock_cycles = 0;
    tlb_mock_local_flushes = 0;
    mock_ipi_vector = 0;
    mock_ipi_mask = 0;
    mock_ipi_calls = 0;
    ack_on_broadcast = 0;
    for (uint32_t i = 0; i < NR_CPUS; i++) {
        percpu_data[i].cpu_id = i;
        percpu_data[i].online = 1;
        percpu_data[i].ipi_ready = 1;
    }
}

/* ── Tests ────────────────────────────────────────────────────────── */

/* Phase A (happy path): gen starts at UINT32_MAX; handler fires
 * (ack_on_broadcast=1); the atomic_fetch_add(gen, 1) wraps
 * UINT32_MAX+1 → 0.  BSP takes target_i = snapshot(gen_i) + 1,
 * which also wraps to 0 (because the snapshot is taken under
 * tlb_sd_lock BEFORE the IPI).  BSP waits `!=`: gen is already
 * 0, target is 0 → wait succeeds immediately.  Returns OK with
 * no panic.
 *
 * This is the same shape as test_tlb_serial_protocol.c case 4
 * (case_wraparound); reasserted here so the wraparound scenario is
 * self-contained in this file's two-phase flow. */
static void case_wraparound_happy_path(void)
{
    TEST_SUITE("ack wraparound phase A: gen=UINT32_MAX → 0, handler fires → OK");
    reset_mock(2);
    /* BSP = 0 (percpu_data[0].cpu_id set by reset_mock).  Target =
     * 1, ready + online.  Pre-set the target's gen to UINT32_MAX
     * so the next handler bump wraps to 0. */
    percpu_data[1].tlb_ack_gen = UINT32_MAX;
    ack_on_broadcast = 1;

    tlb_shootdown();

    assert_eq(percpu_data[1].tlb_ack_gen, 0u);    /* wrapped */
    assert_eq(mock_ipi_calls, 1);
    assert_eq(mock_ipi_vector, IPI_VECTOR_TLB);
    assert_eq(mock_ipi_mask, (uint64_t)(1UL << 1));
    assert_false(panic_armed);                    /* BSP did NOT panic */
}

/* Phase B (negative): the same initial state (gen = UINT32_MAX on
 * the target) but the handler is STUCK (ack_on_broadcast = 0 — no
 * atomic_fetch_add).  BSP takes target_i = UINT32_MAX + 1 → 0.
 * BSP waits `!=`: gen stays at UINT32_MAX, target is 0; UINT32_MAX
 * != 0 forever → wait spins to the deadline → tlb_shootdown_panic
 * FATAL.
 *
 * This is the load-bearing half: a `<` regression in the wait
 * condition would interpret UINT32_MAX < 0 as false (UINT32_MAX is
 * the LARGEST uint32) and exit immediately — falsely declaring
 * the shootdown complete.  The test catches the regression as a
 * non-FATAL return. */
static void case_wraparound_stuck_handler_times_out(void)
{
    TEST_SUITE("ack wraparound phase B: gen=UINT32_MAX, handler stuck → FATAL");
    reset_mock(2);
    percpu_data[1].tlb_ack_gen = UINT32_MAX;
    ack_on_broadcast = 0;        /* handler does NOT fire */

    /* The timeout-fatigue counter (tlb_mock_cycles) advances by
     * 1e8 per arch_cpu_pause() (mock header); TLB_SD_TIMEOUT_CYCLES
     * is 1e9, so the deadline is hit in ~10 wait-loop passes.  The
     * production tlb_shootdown_panic() is overridden with the
     * longjmp capture above. */
    int hit = EXPECT_PANIC(tlb_shootdown());

    assert_true(hit);                                 /* FATAL captured */
    assert_false(panic_armed);                        /* hook disarmed */
    /* The handler never bumped gen — gen is still at UINT32_MAX. */
    assert_eq(percpu_data[1].tlb_ack_gen, UINT32_MAX);
    /* Exactly one IPI broadcast (the BSP made one attempt). */
    assert_eq(mock_ipi_calls, 1);
}

/* Combined scenario from the brief (spec §8.2 ack 代数回绕): one
 * shootdown sequence that performs Phase A then transitions into
 * Phase B's preconditions by hand-setting gen back to UINT32_MAX
 * and disabling the handler.  A second shootdown must now FATAL.
 * This is the literal brief wording:
 *   "构造 gen=UINT32_MAX → 发起 shootdown → target=0 →
 *    mock handler gen = 0（回绕）；BSP 等待 != → 等到 0 → 返回 OK；
 *    mock 立即 gen = UINT32_MAX（不变） → BSP 等待超时 FATAL" */
static void case_brief_wraparound_sequence(void)
{
    TEST_SUITE("ack wraparound (brief verbatim): Phase A → Phase B on same root");
    reset_mock(2);

    /* Phase A setup. */
    percpu_data[1].tlb_ack_gen = UINT32_MAX;
    ack_on_broadcast = 1;
    tlb_shootdown();
    /* Phase A completed cleanly.  After this call, the target's
     * gen is 0. */
    assert_eq(percpu_data[1].tlb_ack_gen, 0u);
    assert_false(panic_armed);

    /* Transition to Phase B: the mock handler is "stuck" (won't
     * bump gen anymore); the brief's wording is "mock 立即 gen =
     * UINT32_MAX（不变）".  The handler is disabled (ack_on_broadcast
     * = 0) AND gen is rolled back to UINT32_MAX so the next
     * shootdown's snapshot+1 wraps to 0 again, but no bump will
     * happen. */
    percpu_data[1].tlb_ack_gen = UINT32_MAX;
    ack_on_broadcast = 0;

    /* Reset mock_ipi_calls so we can assert the second shootdown
     * still issues exactly one IPI (it shouldn't exit early — a
     * `<` regression would exit at the first wait-loop iteration). */
    mock_ipi_calls = 0;

    int rc_hit = EXPECT_PANIC(tlb_shootdown());
    assert_true(rc_hit);
    assert_false(panic_armed);
    assert_eq(mock_ipi_calls, 1);              /* one IPI sent */
    assert_eq(percpu_data[1].tlb_ack_gen, UINT32_MAX);  /* still UINT32_MAX */
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void)
{
    case_wraparound_happy_path();
    case_wraparound_stuck_handler_times_out();
    case_brief_wraparound_sequence();

    printf("\n  ackwrap: total=%d passed=%d failed=%d\n",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed > 0 ? 1 : 0;
}