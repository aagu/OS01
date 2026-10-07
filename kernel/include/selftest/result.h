#ifndef _KERNEL_SELFTEST_RESULT_H
#define _KERNEL_SELFTEST_RESULT_H

#include <stdint.h>
#include <core/selftest.h>   /* selftest_fn */

/*
 * Protocol-v1 publisher for the in-kernel selftest suite (spec §6.1).
 *
 * The kernel selftest runs in two phases:
 *
 *   * the "early" phase — boot-time cases registered with
 *     selftest_register() and executed, in registration order, by
 *     selftest_run_all();
 *   * the "late" phase — scheduled (post-scheduler) cases executed from
 *     task_init() on x86_64, each reported with selftest_record_late().
 *
 * selftest_begin_run() declares the *complete* configuration-specific
 * selection (early + late) before it emits START; selftest_end_run()
 * emits the single END.  Registration overflow, a duplicate id, an
 * expected/registered late-count mismatch, a late case that is recorded
 * twice or never runs, and a second END are all surfaced as protocol
 * failures — the emitted trace is rejected by the host's parse_v1 (a
 * phantom SELECT that never begins, or an END that disagrees with the
 * declared totals).
 *
 * The historical `[selftest] ...` diagnostic lines (including the
 * `[selftest] N total: N passed, N failed` summary) are still printed,
 * but they are diagnostics only: the v1 records are the success gate.
 * Spec §5.2 — "内核 selftest ... 不能在第一份汇总时提前判通过".
 */

#define SELFTEST_SUITE_NAME "kernel-selftest"

/* Register an early (boot-time) case.  Declared here because the
 * registry lives in result.c while the registration list lives in
 * selftest.c. */
void selftest_register(const char *id, selftest_fn fn);

/* Declare the full selection and emit START + every SELECT record. */
void selftest_begin_run(unsigned int expected_late);

/* Execute the registered early subset, emitting BEGIN/terminal records. */
int  selftest_run_all(void);

/* Record one scheduled (late) case's outcome, emitting its records. */
void selftest_record_late(const char *id, int rc);

/* Emit the single END record; nonzero if the run observed a contract
 * violation (including a declared late case that never ran). */
int  selftest_end_run(void);

/* Register a late (scheduled) case id.  The id must later be reported
 * exactly once via selftest_record_late(). */
void selftest_register_late(const char *id);

/* Populate the full per-configuration registration (early + late).
 * Defined in selftest.c; called by selftest_begin_run(). */
void selftest_register_builtin_tests(void);

#endif /* _KERNEL_SELFTEST_RESULT_H */
