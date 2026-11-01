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
int test_aarch64_ap_scheduler_cases(void);

/* ── P2 wake-variant timing hooks (AARCH64_AP_SCHED_WAKE_TEST=1) ──
 * Every hook below is compiled ONLY into the sched-wake variant; the
 * normal / ap / fatal-owner / old-sched ELFs carry neither the calls
 * nor the symbols.  All hooks run without locks and never change the
 * production lock order; waits are bounded (2s cycle deadline) and a
 * timeout only sets BSS failure flags (the runner hard-times-out an
 * unrecoverable target — never a SKIP). */

#ifdef OS01_SELFTEST
struct task_struct;

/* idle.c call sites (spec §5.3 window instrumentation). */
enum aarch64_ap_sched_idle_stage {
    AP_SCHED_IDLE_AFTER_WORK = 1,  /* after consuming ap_work, IRQs on */
    AP_SCHED_IDLE_AFTER_CHECK = 2, /* final no-work check done + rq_lock
                                      released, IRQs masked; may wait
                                      for BSP permission (<=2s) */
    AP_SCHED_IDLE_BEFORE_WAIT = 3, /* immediately before the wait
                                      helper, IRQs masked; publish
                                      WAIT_ARMED, return at once */
    AP_SCHED_IDLE_AFTER_WAIT = 4,  /* wait helper returned + IRQ enable
                                      + ISB done, IRQs on */
};
void aarch64_ap_sched_idle_hook(unsigned cpu, unsigned stage);

/* core.c scanner: called with task_list_lock held, IRQs masked, BEFORE
 * the target's blocker is read; atomically reads only the target
 * identity/generation and returns — true skips this explicit test
 * object in the BSP fallback scan (never pauses the scan itself). */
bool aarch64_ap_sched_skip_scan(struct task_struct *task);

/* core.c schedule(): after current's dequeue/conditional re-enqueue and
 * the rq_lock release, adjacent to the old observe_block_enqueue, before
 * the scan; IRQs masked, no lock held, <=2s test handshake. */
void aarch64_ap_sched_after_dequeue(struct task_struct *task);

/* core.c task_finish_switch(): at the very start, before the rq_lock
 * is taken and on_cpu cleared; runs on the incoming task stack, IRQs
 * masked, no lock held, <=2s handshake; prev is still valid here. */
void aarch64_ap_sched_before_finish(struct task_struct *prev);

/* ap_sched.c: the AP has installed its legal idle stack, IRQs still
 * masked and scheduler_ok not yet published, no lock held, <=2s. */
void test_aarch64_ap_sched_before_ready(unsigned cpu);

/* BSP: called in the ready wait loop (IRQs on, no lock); processes
 * CHECKPOINT and submits work, never sleeps. */
void test_aarch64_ap_sched_poll_ready(void);

/* gic_driver.c: for intid==4 ONLY, once before the handler dispatch
 * and once after the real EOIR write; IRQs masked, no rq/task-list
 * lock, never waits, never EOIs itself.  Records the complete raw IAR
 * and the entry/completion counts into the per-CPU slots. */
void aarch64_ap_sched_sgi_trace(unsigned cpu, uint32_t raw_iar,
                                bool after_eoi);
#endif /* OS01_SELFTEST */

#ifdef AARCH64_AP_SCHED_WAKE_TEST
void aarch64_ap_sched_scan_woke(struct task_struct *task);
void aarch64_ap_sched_finish_enqueued(struct task_struct *task);
extern uint64_t aarch64_ap_sched_sgi_active[8];
#endif

/* Wake-variant driver entry (replaces the old suite's role in the
 * sched-wake build; envelope phase=wake). */
void test_aarch64_ap_scheduler_wake_run(void);

#endif /* OS01_SELFTEST */

#endif /* KERNEL_SELFTEST_AARCH64_AP_SCHEDULER_H */
