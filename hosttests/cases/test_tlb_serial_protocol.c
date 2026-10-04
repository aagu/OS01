/* hosttests/cases/test_tlb_serial_protocol.c — M2/M3 Task 12 Step 1 (TDD).
 *
 * Compiles the PRODUCTION kernel/memory/tlb.c against the host with the
 * preinclude mock/tlb_test_runtime.h short-circuiting percpu/spinlock/
 * mmu/cpu/ipi headers.  The test TU supplies percpu_data[], num_cpus,
 * the ipi_broadcast / arch_flush_tlb_all mocks and overrides the weak
 * tlb_shootdown_panic() hook with a longjmp capture.
 *
 * Cases:
 *   1. two ready targets: initiator flushes locally, broadcasts the
 *      ready∧¬self mask, and both targets' gens reach their snapshot →
 *      returns with no panic;
 *   2. not-ready / offline / self targets are excluded from the mask;
 *   3. single CPU: no IPI, local flush only;
 *   4. uint32 wraparound: target gen 0xFFFFFFFF → 0 satisfies the
 *      equality wait (the reason the wait is ==, never <=);
 *   5. timeout: no target acks → tlb_shootdown_panic FATAL (longjmp).
 */
#include <test_framework.h>
#include <stdint.h>
#include <setjmp.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* Shared mock surface (NR_CPUS, percpu_t, spinlock/cycle/flush stubs).
 * Included explicitly here because the test TU does not preinclude it;
 * the production TU gets it via -include on the Makefile rule. */
#include "../mock/tlb_test_runtime.h"

/* Production declaration (memory/vmm.h is too heavy for the host mock);
 * keep the signature pinned here exactly as the kernel declares it. */
void tlb_shootdown(void);

/* ── Test-supplied mock state (tlb.c references these symbols) ── */
percpu_t percpu_data[NR_CPUS];
uint32_t num_cpus = 1;
uint64_t tlb_mock_cycles = 0;
uint32_t tlb_mock_local_flushes = 0;

static uint32_t mock_ipi_vector;
static uint64_t mock_ipi_mask;
static int      mock_ipi_calls;
static int      ack_on_broadcast;   /* emulate handler running per target */

void ipi_broadcast(uint32_t vector, uint64_t target_mask)
{
    mock_ipi_vector = vector;
    mock_ipi_mask = target_mask;
    mock_ipi_calls++;
    if (ack_on_broadcast) {
        /* Emulates IPI delivery + per-target handler (unconditional
         * flush + gen bump).  The gen snapshot inside tlb_shootdown is
         * taken BEFORE this call, so host-side this is race-free. */
        for (uint32_t cpu = 0; cpu < NR_CPUS; cpu++)
            if (target_mask & (1UL << cpu))
                __atomic_fetch_add(&percpu_data[cpu].tlb_ack_gen, 1,
                                   __ATOMIC_RELEASE);
    }
}

/* Weak-hook override: longjmp capture (mirrors the EXPECT_VIOLATION
 * pattern in test_foundational_primitives.c). */
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
    percpu_data[0].cpu_id = 0;
    percpu_data[0].online = 1;
    percpu_data[0].ipi_ready = 1;       /* initiator itself is ready */
}

static void case_ok_two_targets(void)
{
    TEST_SUITE("tlb_shootdown: two ready targets");
    reset_mock(3);
    percpu_data[1].online = 1; percpu_data[1].ipi_ready = 1;
    percpu_data[2].online = 1; percpu_data[2].ipi_ready = 1;
    ack_on_broadcast = 1;

    tlb_shootdown();

    assert_eq(tlb_mock_local_flushes, 1);   /* local flush happened */
    assert_eq(mock_ipi_calls, 1);
    assert_eq(mock_ipi_vector, IPI_VECTOR_TLB);
    assert_eq(mock_ipi_mask, (uint64_t)(1UL << 1) | (1UL << 2));
    assert_eq(percpu_data[1].tlb_ack_gen, 1);   /* snapshot+1 reached */
    assert_eq(percpu_data[2].tlb_ack_gen, 1);
    assert_false(panic_armed);
}

static void case_mask_excludes_unready_offline_self(void)
{
    TEST_SUITE("tlb_shootdown: target mask filter");
    reset_mock(4);
    percpu_data[1].online = 1; percpu_data[1].ipi_ready = 0; /* not ready */
    percpu_data[2].online = 0; percpu_data[2].ipi_ready = 1; /* offline  */
    percpu_data[3].online = 1; percpu_data[3].ipi_ready = 1; /* target   */
    ack_on_broadcast = 1;

    tlb_shootdown();

    assert_eq(mock_ipi_mask, 1UL << 3);
    assert_eq(percpu_data[3].tlb_ack_gen, 1);
}

static void case_single_cpu_no_broadcast(void)
{
    TEST_SUITE("tlb_shootdown: single CPU");
    reset_mock(1);
    ack_on_broadcast = 1;

    tlb_shootdown();

    assert_eq(mock_ipi_calls, 0);           /* empty mask → no IPI */
    assert_eq(tlb_mock_local_flushes, 1);   /* local flush still happens */
    assert_eq(percpu_data[0].tlb_ack_gen, 1);
}

static void case_wraparound(void)
{
    TEST_SUITE("tlb_shootdown: uint32 gen wraparound");
    reset_mock(2);
    percpu_data[1].online = 1; percpu_data[1].ipi_ready = 1;
    percpu_data[1].tlb_ack_gen = UINT32_MAX;    /* next ack wraps to 0 */
    ack_on_broadcast = 1;

    tlb_shootdown();

    /* The ==-wait (never <=) must accept gen 0 == snapshot+1 (mod 2^32). */
    assert_eq(percpu_data[1].tlb_ack_gen, 0);
    assert_false(panic_armed);
}

static void case_timeout_fatal(void)
{
    TEST_SUITE("tlb_shootdown: timeout → FATAL");
    reset_mock(2);
    percpu_data[1].online = 1; percpu_data[1].ipi_ready = 1;
    /* ack_on_broadcast == 0: no target acks → the wait must spin to the
     * deadline.  arch_cpu_pause() advances tlb_mock_cycles by 1e8 per
     * iteration (mock header), so TLB_SD_TIMEOUT_CYCLES (1e9) is
     * exhausted in ~10 wait-loop iterations. */
    assert_true(EXPECT_PANIC(tlb_shootdown()));
    assert_false(panic_armed);      /* hook disarmed by the capture */
}

int main(void)
{
    case_ok_two_targets();
    case_mask_excludes_unready_offline_self();
    case_single_cpu_no_broadcast();
    case_wraparound();
    case_timeout_fatal();

    printf("\n  tlbsp: total=%d passed=%d failed=%d\n",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed > 0 ? 1 : 0;
}
