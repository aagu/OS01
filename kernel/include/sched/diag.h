#ifndef KERNEL_SCHED_DIAG_H
#define KERNEL_SCHED_DIAG_H

/* Scheduler diagnostics (aarch64 EL1 scheduler plan Phase 2, Task 2).
 *
 * hang_dump_all() used to live in kernel/core/panic.c, which drags the
 * x86-only framebuffer/console headers into every TU that wants the
 * dump.  The dump itself is arch-neutral (it walks the scheduler's own
 * task list and defers the saved-context fields to the
 * arch_dump_task_context() facade hook), so it moved here and the
 * aarch64 build can reach it.  The x86-only fb/console panic bits stay
 * in panic.c. */

/* Dump every task on the global list plus per-CPU scheduler state.
 * Called from schedule()'s hang detector (watchdog_counter >=
 * HANG_THRESHOLD, see <core/panic.h>) and available to any arch
 * watchdog. */
void hang_dump_all(void);

#endif /* KERNEL_SCHED_DIAG_H */
