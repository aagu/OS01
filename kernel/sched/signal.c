#include <sched/task.h>
#include <sched/internal.h>
#include <arch/spinlock.h>
#include <errno.h>
#include <kernel.h>

// ── task_send_signal ────────────────────────────────────────
// SMP-safe signal delivery: find task by pid under
// task_list_lock, check PF_KTHREAD / init protection,
// set signal bit, wake if interruptible.
int task_send_signal(int pid, int sig)
{
    int ret = 0;
    uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
    list_t *pos = init_task_union.task.list.next;
    task_t *target = NULL;
    while (pos != &init_task_union.task.list) {
        task_t *t = container_of(pos, task_t, list);
        pos = task_list_next(pos);
        if (t->pid == pid) {
            target = t;
            break;
        }
    }
    if (!target) {
        ret = -ESRCH;
    } else if (target->flags & PF_KTHREAD) {
        ret = -EPERM;
    } else {
        target->signal |= (1ULL << sig);
        if (target->state == TASK_INTERRUPTIBLE)
            task_wake(target);
    }
    spin_unlock_irqrestore(&task_list_lock, tl_flags);
    return ret;
}

// ── signal_pgrp ───────────────────────────────────────────
// Sends sig to all tasks with pgrp==target, skipping PF_KTHREAD.
// Holds task_list_lock.  Returns 0 on match, -ESRCH if no match,
// 0 if target==0 (silent no-op).
int signal_pgrp(pid_t target, int sig)
{
    if (target == 0) return 0;  // silent no-op
    if (sig < 1 || sig >= NSIG) return -EINVAL;
    int matched = 0;
    uint64_t flags = spin_lock_irqsave(&task_list_lock);
    list_t *pos = init_task_union.task.list.next;
    while (pos != &init_task_union.task.list) {
        task_t *t = container_of(pos, task_t, list);
        pos = task_list_next(pos);
        if (t->pgrp == target && !(t->flags & PF_KTHREAD)) {
            t->signal |= (1ULL << sig);
            if (t->state == TASK_INTERRUPTIBLE)
                task_wake(t);
            matched++;
        }
    }
    spin_unlock_irqrestore(&task_list_lock, flags);
    return matched > 0 ? 0 : -ESRCH;
}
