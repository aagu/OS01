#ifndef KERNEL_SELFTEST_AARCH64_AP_SCHEDULER_H
#define KERNEL_SELFTEST_AARCH64_AP_SCHEDULER_H

/* P1 AP production-path selftest driver (spec §7, plan P1 Task 4).
 * Built ONLY in the AARCH64_AP_SCHED_TEST_AP variant; the normal and
 * old-sched selftest ELFs carry neither entry (nm-audited). */

#ifdef OS01_SELFTEST

/* BSP task stack, after its own scheduler_ok publication and BEFORE
 * the AP release gate opens.  Validates runtime admission (invalid CPU
 * rejected with zero publication; an IPI-ready but not-yet-scheduler
 * real AP rejected) and completes the pre-handoff WORK_BARRIER round
 * against every accepted AP's old work loop. */
void test_aarch64_ap_scheduler_before_release(void);

/* BSP task stack, after every accepted AP published scheduler_ok.
 * Runs the seven production cases (ready/entry/yield/reap/work/
 * admission/default-mask), owns the single selftest_end_run() call and
 * writes the frozen [ap-sched] START/case/END evidence envelope. */
void test_aarch64_ap_scheduler_run(void);

#endif /* OS01_SELFTEST */

#endif /* KERNEL_SELFTEST_AARCH64_AP_SCHEDULER_H */
