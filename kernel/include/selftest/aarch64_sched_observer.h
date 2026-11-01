#ifndef KERNEL_SELFTEST_AARCH64_SCHED_OBSERVER_H
#define KERNEL_SELFTEST_AARCH64_SCHED_OBSERVER_H

/* Concurrency-safe resource observer for the aarch64 EL1 scheduler
 * selftests (P1 Task 2, spec §6).
 *
 * The observer tracks kernel-thread resource allocations (raw task
 * stack, thread context, FPU area) across CPUs with:
 *   - one ACTIVE batch at a time (begin/end, generation-tagged);
 *   - a monotonically increasing non-zero instance token per
 *     allocation (tokens are never reused; freed slots are);
 *   - an explicit FREEING state between free_begin() and the real
 *     kfree, so a batch cannot end while a release is still in
 *     flight;
 *   - an IRQ-safe dedicated lock; no allocation, printing or rq-lock
 *     access happens inside it.
 *
 * Same-address records from different generations may coexist (an old
 * FREEING record and a fresh ALLOCATED record for a reused address);
 * a stale token can never complete a newer record.
 */

#ifdef OS01_SELFTEST
#include <stdint.h>
#include <stdbool.h>

/* Resource kinds shared with <selftest/aarch64_scheduler.h>: */
#define AARCH64_SCHED_RESOURCE_STACK  1
#define AARCH64_SCHED_RESOURCE_THREAD 2
#define AARCH64_SCHED_RESOURCE_FPU    3

/* Active record slots (NOT capacity for the whole boot: FREED slots
 * are reused, only simultaneously-live records count). */
#define AARCH64_SCHED_OBSERVER_CAPACITY 512

/* Open a batch.  @cpu is the batch's TARGET label (which CPU's work is
 * being observed), not the hook execution CPU.  Only one batch may be
 * active; a record is tracked only while a batch is open. */
void aarch64_sched_observer_begin(unsigned cpu, unsigned generation);

/* Close the batch opened for @cpu/@generation.  Returns 0 only when
 * every tracked record of this generation is FREED (zero ALLOCATED,
 * zero FREEING) and no overflow / duplicate-free / unknown-token /
 * stale-false-alloc failure was recorded.  A failed end keeps the
 * batch open so diagnostics continue to accumulate. */
int aarch64_sched_observer_end(unsigned cpu, unsigned generation);

/* Allocation-side registration.  ONLY alloc=true may be passed from
 * production code: a false call used to fake a release completion and
 * is now recorded as a batch failure.  Untracked when no batch is
 * open, on a NULL pointer or a kind outside 1..3. */
void aarch64_sched_observe_resource(const void *ptr, unsigned kind,
                                    bool alloc);

/* Release-side, step 1: transition the LATEST matching ALLOCATED
 * record of the active generation to FREEING and return its token.
 * Returns 0 when the pointer is not tracked (callers pass the token
 * through unchanged).  Token capture happens BEFORE the real kfree. */
uint64_t aarch64_sched_observe_free_begin(const void *ptr, unsigned kind);

/* Release-side, step 2: called AFTER the real kfree with the token
 * from free_begin.  Transitions the record to FREED and advances the
 * released count.  Token 0 (untracked) is a no-op; an unknown
 * non-zero token or a second completion of the same token is a
 * recorded failure.  Never dereferences the (already freed) object. */
void aarch64_sched_observe_free_complete(uint64_t token);

/* Cumulative counters since boot (for the evidence lines). */
uint64_t aarch64_sched_observer_created(void);
uint64_t aarch64_sched_observer_released(unsigned kind);

/* Batch failure flags (T4 evidence fields): bit0 overflow, bit1
 * legacy-false-alloc, bit2 unknown token, bit3 duplicate completion,
 * bit4 begin-while-active.  Snapshot of the CURRENT batch. */
uint32_t aarch64_sched_observer_failure_flags(void);
#endif

#endif /* KERNEL_SELFTEST_AARCH64_SCHED_OBSERVER_H */
