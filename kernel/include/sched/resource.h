#ifndef _KERNEL_SCHED_RESOURCE_H
#define _KERNEL_SCHED_RESOURCE_H

#include <stdint.h>
#include <sched/task.h>
#include <uapi/resource.h>

void task_nofile_init(task_t *tsk);
void task_nofile_inherit(task_t *child, task_t *parent);
uint64_t task_nofile_soft(task_t *tsk);
int task_nofile_update(task_t *tsk, const struct os01_rlimit64 *new_limit, struct os01_rlimit64 *old_limit);

// Snapshot of the CURRENT task's RLIMIT_NOFILE soft limit.
//
// Consumers that size a per-task resource (descriptor installation,
// poll buffer allocation) take the snapshot ONCE, at entry, and use
// that value for the whole operation.  A later lowering of the limit
// never invalidates work already admitted under an earlier snapshot.
//
// Lock order: the snapshot is taken under the task's resource lock
// and the resource lock is RELEASED before the consumer takes any
// other lock (e.g. files_t.lock).  The two locks are never held at
// the same time.
//
// Inline on purpose: a consumer that never asks for the current
// task's limit must not pull the current-task lookup into its link
// unit.
static inline uint64_t task_nofile_soft_current(void)
{
    return task_nofile_soft(current);
}

#endif /* _KERNEL_SCHED_RESOURCE_H */
