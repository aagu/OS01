#include <sched/resource.h>
#include <sched/task.h>
#include <config/limits.h>
#include <errno.h>
#include <uapi/resource.h>
#include <arch/spinlock.h>

void task_nofile_init(task_t *tsk)
{
    if (!tsk)
        return;
    tsk->nofile_soft = OS01_NOFILE_CAPACITY;
    tsk->nofile_hard = OS01_NOFILE_CAPACITY;
    spin_init(&tsk->nofile_lock);
}

void task_nofile_inherit(task_t *child, task_t *parent)
{
    if (!child)
        return;
    if (!parent) {
        task_nofile_init(child);
        return;
    }

    uint64_t soft, hard;
    uint64_t flags = spin_lock_irqsave(&parent->nofile_lock);
    soft = parent->nofile_soft;
    hard = parent->nofile_hard;
    spin_unlock_irqrestore(&parent->nofile_lock, flags);

    child->nofile_soft = soft;
    child->nofile_hard = hard;
    spin_init(&child->nofile_lock);
}

uint64_t task_nofile_soft(task_t *tsk)
{
    if (!tsk)
        return OS01_NOFILE_CAPACITY;

    uint64_t soft;
    uint64_t flags = spin_lock_irqsave(&tsk->nofile_lock);
    soft = tsk->nofile_soft;
    spin_unlock_irqrestore(&tsk->nofile_lock, flags);
    return soft;
}

int task_nofile_update(task_t *tsk, const struct os01_rlimit64 *new_limit, struct os01_rlimit64 *old_limit)
{
    if (!tsk)
        return -ESRCH;

    uint64_t flags = spin_lock_irqsave(&tsk->nofile_lock);

    if (old_limit) {
        old_limit->cur = tsk->nofile_soft;
        old_limit->max = tsk->nofile_hard;
    }

    if (new_limit) {
        if (new_limit->cur > new_limit->max) {
            spin_unlock_irqrestore(&tsk->nofile_lock, flags);
            return -EINVAL;
        }
        if (new_limit->max > tsk->nofile_hard) {
            spin_unlock_irqrestore(&tsk->nofile_lock, flags);
            return -EPERM;
        }
        tsk->nofile_soft = new_limit->cur;
        tsk->nofile_hard = new_limit->max;
    }

    spin_unlock_irqrestore(&tsk->nofile_lock, flags);
    return 0;
}
