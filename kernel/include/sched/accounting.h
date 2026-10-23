#ifndef _KERNEL_SCHED_ACCOUNTING_H
#define _KERNEL_SCHED_ACCOUNTING_H

/*
 * sched/accounting.h — per-CPU tick accounting (monitor plan Task 2)
 *
 * Design: docs/superpowers/specs/2026-10-09-monitor-design.md §2.
 *
 * Every valid local-timer interrupt records one sample against the
 * per-CPU usage counters: the tick is "idle" when the interrupted task
 * is the CPU's idle task, "busy" otherwise.  Consumers (proc/stat in
 * Task 4, the monitor's delta math in Task 7) read consistent
 * snapshots through sched_accounting_read().
 *
 * Concurrency (spec §2.4): writers and readers take the per-CPU
 * statistics lock with IRQs saved, so busy/idle are always copied as a
 * pair; the read path copies and unlocks immediately, never nesting
 * another lock.  The hot path does no formatting and no allocation.
 */

#include <stdint.h>
#include <stdbool.h>

/* One sample period, as published to user space.  valid requires the
 * global accounting to be initialized (sched_accounting_init), the
 * target CPU's scheduler to be up, and a valid tick source to have
 * been started on that CPU (LAPIC success or enabled-PIT fallback on
 * x86_64).  Cores whose timer never started report valid=0 and are
 * displayed as unavailable — never as 0%. */
struct cpu_usage_snapshot {
    uint64_t busy_ticks;
    uint64_t idle_ticks;
    uint64_t epoch;          /* changes on reset or re-onlining */
    uint32_t tick_hz;
    uint32_t online;         /* from percpu actual online state */
    uint32_t valid;
};

/* One-time initialization: spin_init every per-CPU usage lock (the
 * .bss image is all zeros, which is the LOCKED value for this
 * spinlock — "不能仅依赖清零"), then release-publish accounting_ready
 * so early tick interrupts stay inert until the locks exist. */
void sched_accounting_init(void);

/* Publish a tick-source lifecycle for one CPU.  started=true means a
 * periodic tick is really running (the caller must pass the real
 * lapic_timer_start() return value, not an assumption); started=false
 * marks the CPU unavailable.  Only an explicitly NEW lifecycle (first
 * start, or a stop/start transition) changes the epoch; repeated
 * publication of the same parameters is idempotent. */
void sched_accounting_cpu_start(unsigned cpu, unsigned hz, bool started);

/* Hot path: called once per valid local-timer interrupt.  BSP ticks
 * account inside tick_handler(); AP ticks account in lapic_timer_handler()'s
 * AP branch (the BSP LAPIC path must NOT call this a second time).
 * Inert until accounting_ready && scheduler_ok && an idle task is
 * published && the CPU's tick source is valid. Unclassifiable ticks
 * leave CPU counters and task runtime unchanged. */
void sched_account_tick(void);

/* Copy one CPU's counters into *out.  Returns 0 on success, -EINVAL
 * for an invalid CPU id or NULL out.  Never modifies the counters. */
int sched_accounting_read(unsigned cpu, struct cpu_usage_snapshot *out);

#endif /* _KERNEL_SCHED_ACCOUNTING_H */
