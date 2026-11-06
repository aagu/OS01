/* aarch64 per-CPU scheduler tick — public interface of
 * kernel/arch/aarch64/cpu/sched_tick.c.
 *
 * The BSP owns the GLOBAL time base (jiffies, the poll timeout, the
 * timer list / TIMER_SIRQ and the blocker scan).  Every admitted CPU
 * owns a LOCAL 100 Hz CNTP tick that is used ONLY for per-CPU and
 * per-task runtime accounting and for the tick-driven vruntime.
 *
 * This is still EL1 COOPERATIVE scheduling: the tick never switches
 * tasks, never calls schedule() and never leaves the interrupt.  It
 * only publishes the accounting and — on the BSP — requests a
 * cooperative reschedule, which the ordinary cooperative entry point
 * consumes later.
 *
 * 100 Hz is a NOMINAL rate: every really delivered CNTP interrupt is
 * ONE sample and a lost tick is never backfilled from the wall clock.
 */

#ifndef OS01_AARCH64_SCHED_TICK_H
#define OS01_AARCH64_SCHED_TICK_H

#include <stdint.h>
#include <stdbool.h>

/* Nominal local scheduler tick rate [Hz]. */
#define AARCH64_SCHED_TICK_HZ 100U

/* BSP: one-time initialization of the local scheduler tick.  Runs on
 * the boot stack BEFORE the AP handoff and only initializes data: it
 * never reads `current` and never touches an uninitialized lock. */
void aarch64_sched_tick_init(void);

/* Start the local scheduler tick on the CALLING CPU.
 *
 * Preconditions: IRQs masked, a legal idle stack, the CPU's idle
 * metadata already published.  A refusal never reads `current`.
 *
 * Idempotent: a repeated successful start does NOT rearm the
 * counter, does NOT reset the counters and does NOT move the
 * accounting epoch.  Returns false when the CPU is not admitted, when
 * its local counter is not really running, or when the global
 * initialization has not happened yet. */
bool aarch64_sched_tick_start(void);

/* One local scheduler tick on the CALLING CPU.  Called ONLY from the
 * CNTP interrupt handler (IRQs masked).  Never schedules, never
 * switches, never allocates, never formats a log line. */
void aarch64_sched_tick(void);

/* Is the local scheduler tick of CPU @cpu really running?  READ-ONLY:
 * it never arms, rearms or touches a register.  Returns false for an
 * unknown CPU and for a CPU whose start was refused. */
bool aarch64_sched_tick_started(unsigned cpu);

#if defined(OS01_SELFTEST) && defined(AARCH64_SCHED_TICK_TEST)
/* The idle sleep boundary of CPU @cpu (selftest build only): the idle
 * step and the tick variant's sleep probe report their sleep boundary
 * through this wrapper.  The observer hooks themselves stay in the
 * tick selftest TU. */
void aarch64_sched_tick_sleep_boundary(unsigned cpu, bool entering);
#endif

#endif /* OS01_AARCH64_SCHED_TICK_H */
