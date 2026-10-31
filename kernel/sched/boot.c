/* kernel/sched/boot.c — arch-neutral scheduler-boot sequence
 * (aarch64 EL1 kernel-thread scheduler, spec §6 / §8.1).
 *
 * This TU owns the four public boot-phase functions declared in
 * <sched/boot.h>:
 *
 *   sched_boot_creation_begin()  open the BSP boot creation window
 *   sched_boot_creation_open()   query it
 *   sched_boot_publish_idle()    publish the idle task metadata
 *   sched_boot_start()           close the window + publish scheduler_ok
 *
 * The x86 SYSTEM startup (fd table, /bin/init spawn, thermal worker,
 * late selftests, idle loop) is NOT here: it lives in
 * kernel/arch/x86_64/cpu/sched_boot.c and calls into this module.
 *
 * Locking: none of these take a lock.  They run once per boot on the
 * BSP, before (and around) the moment the scheduler becomes reachable.
 */

#include <sched/task.h>
#include <sched/internal.h>
#include <sched/boot.h>
#include <percpu/percpu.h>
#include <arch/spinlock.h>

/* ── The BSP boot creation window ─────────────────────────────
 * State machine:
 *   0  never opened (aarch64 stays here forever)
 *   1  open       (x86 BSP, between percpu-up and the first
 *                  BSP scheduler_ok publication)
 *   2  closed     (permanent: sched_boot_start() latches it)
 *
 * The window is latched closed so that "scheduler_ok was cleared
 * later" can never reopen it (spec §8.1: the exception closes
 * permanently at the first BSP scheduler publication). */
static int boot_creation_state;

void sched_boot_creation_begin(void)
{
    /* Once closed, never reopened. */
    if (boot_creation_state != 0)
        return;
    boot_creation_state = 1;
}

bool sched_boot_creation_open(void)
{
    return boot_creation_state == 1;
}

/* ── Idle publication (spec §6) ────────────────────────────────
 * Publish the idle task metadata WITHOUT reading `current`: the BSP
 * may be on the boot stack, where `current` is not a task.
 *
 * Never resets an existing task list or runqueue: x86 boot services
 * (the lwIP tcpip thread) may already be queued by the time this
 * runs. */
void sched_boot_publish_idle(void)
{
    task_t *idle = &init_task_union.task;

    /* The idle task's identity is the statically initialised one
     * (INIT_TASK: comm "idle", pid 0, pgrp/session 1).  Only the
     * scheduling-visible fields are (re)published here. */
    idle->cpu          = 0;
    idle->allowed_cpus = 1ULL;      /* CPU0 only */
    idle->on_cpu       = 1;
    idle->on_rq        = 0;
    idle->state        = TASK_RUNNING;

    percpu_data[0].idle = idle;
}

/* ── Scheduler start (spec §6 step 5) ──────────────────────────
 * Close the boot creation window BEFORE publishing scheduler_ok:
 * there must be no instant in which the window is open while
 * scheduler_ok is published.  Then publish CPU0's scheduler_ok and
 * RETURN — architecture startup owns everything after this point. */
void sched_boot_start(void)
{
    boot_creation_state = 2;        /* closed, permanently */
    percpu_data[0].scheduler_ok = 1;
}
