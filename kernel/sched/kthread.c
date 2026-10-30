/* kernel/sched/kthread.c — arch-neutral kernel-thread mechanics
 * (aarch64 EL1 scheduler plan Phase 1, Task 2).
 *
 * Phase 1 houses only the common switch tail; the kthread
 * create/exit lifecycle lands in Phase 2.
 */

#include <sched/task.h>
#include <sched/internal.h>
#include <memory/slab.h>

/* Runs on the *next* task's stack. Captures the self-reap decision and
 * the objects to release BEFORE task_finish_switch() clears on_cpu:
 * after that a waiter may free prev, and reading prev afterwards is a
 * use-after-free. */
void task_switch_complete(task_t *actual_prev)
{
    if (actual_prev == current) return; /* never finish/free our own stack */
    bool self_reap = (actual_prev->flags & PF_SELF_REAP) != 0;

    thread_t *thd  = actual_prev->thread;
    void      *fpu  = actual_prev->fpu_save;
    void      *stk  = actual_prev->stack_alloc_base;

    task_finish_switch(actual_prev);

    if (self_reap) {
        if (thd) kfree(thd);
        if (fpu) kfree(fpu);
        if (stk) kfree(stk);
    }
}
