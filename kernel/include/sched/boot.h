#ifndef KERNEL_SCHED_BOOT_H
#define KERNEL_SCHED_BOOT_H

#include <stdbool.h>

/* Public scheduler-boot interface (aarch64 EL1 kernel-thread scheduler,
 * spec §6 / §8.1).
 *
 * This header is the ONLY public surface of the boot phase.  The
 * arch-neutral implementation lives in kernel/sched/boot.c; the x86
 * system startup (fd table, /bin/init spawn, thermal worker, late
 * selftests, idle loop) lives in kernel/arch/x86_64/cpu/sched_boot.c.
 *
 * Locking: none of these functions take a lock.  They are called once
 * per boot from the BSP, before (and around) the moment the scheduler
 * becomes reachable. */

/* Publish idle metadata without current; never reset an existing
 * task list/runqueue (x86 boot services may already be queued).
 *
 * Sets percpu_data[0].idle, the idle task's identity, state =
 * TASK_RUNNING, on_cpu = 1, on_rq = 0, and the CPU mask (CPU0 only),
 * per spec §6.  It must not create a TTY or file table and must not
 * load a user init — that is x86 system startup, a different concern. */
void sched_boot_publish_idle(void);

/* Close the boot creation window, publish BSP scheduler_ok = 1, then
 * RETURN.  Architecture startup owns post-publication workers/tests/idle.
 *
 * The window is cleared BEFORE scheduler_ok is published: there must be
 * no instant in which the window is open while scheduler_ok is set. */
void sched_boot_start(void);

/* The BSP boot-phase creation window (spec §8.1).
 *
 * True only during the BSP boot phase: per-CPU state is up, the current
 * task stack is up, the CPU0 runqueue is up, and the BSP has not yet
 * published scheduler_ok.  Closes permanently at the first BSP
 * scheduler publication, even if scheduler_ok is later cleared.
 *
 * While open, an *unpinned* kernel_thread/create_kthread whose
 * placement finds no eligible CPU falls back to CPU0 and queues there.
 * When closed, the same situation returns -EINVAL. */
bool sched_boot_creation_open(void);

/* Open the boot creation window.  Called once only on the x86 BSP
 * after x86_64_boot_percpu() has established percpu, the current task
 * stack and the CPU0 runqueue, and before any boot-service kthread
 * creation.  aarch64 never calls it. */
void sched_boot_creation_begin(void);

#endif /* KERNEL_SCHED_BOOT_H */
