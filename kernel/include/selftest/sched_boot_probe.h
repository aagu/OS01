#ifndef KERNEL_SELFTEST_SCHED_BOOT_PROBE_H
#define KERNEL_SELFTEST_SCHED_BOOT_PROBE_H

/* x86 UP boot-window probe (aarch64 EL1 scheduler plan, Phase 5 Task 4
 * / spec §10.1 item 4).
 *
 * Two halves that only mean something together:
 *
 *   sched_boot_probe_create()  — called from task_init() INSIDE the legal
 *       pre-publication boot window (after sched_boot_creation_begin()
 *       and spawn_user_task("/bin/init"), before sched_boot_start()), while
 *       CPU0 is online but scheduler_ok == 0.  It calls the REAL
 *       kernel_thread() with a marker function and records the PID — proving
 *       a boot-phase creation succeeds with no scheduler_ok target.
 *
 *   test_sched_boot_probe()    — the late selftest case, executed after
 *       sched_boot_start() published scheduler_ok == 1.  It waits
 *       (bounded, via the production schedule()) for the marker function
 *       to have ACTUALLY EXECUTED under the scheduler, and verifies the
 *       runtime negative calls are rejected now that the window closed.
 *
 * There is no test scheduler: the marker is a plain PF_KTHREAD function
 * that sets one flag, and the wait deadline is jiffies-bounded.
 */

/* Set by the marker thread itself, exactly once, on first entry. */
extern volatile unsigned sched_boot_probe_ran;

/* The PID returned by the boot-phase kernel_thread() call (negative
 * errno would mean the boot-phase creation failed). */
extern volatile int sched_boot_probe_pid;

/* Boot-phase half: create the marker thread while scheduler_ok == 0. */
void sched_boot_probe_create(void);

/* Late selftest case body (returns 0 = PASS). */
int test_sched_boot_probe(void);

/* Late selftest case: user init PID 1 invariant (returns 0 = PASS). */
int test_init_pid(void);

#endif /* KERNEL_SELFTEST_SCHED_BOOT_PROBE_H */
