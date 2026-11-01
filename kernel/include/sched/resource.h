#ifndef _KERNEL_SCHED_RESOURCE_H
#define _KERNEL_SCHED_RESOURCE_H

#include <stdint.h>
#include <sched/task.h>
#include <uapi/resource.h>

void task_nofile_init(task_t *tsk);
void task_nofile_inherit(task_t *child, task_t *parent);
uint64_t task_nofile_soft(task_t *tsk);
int task_nofile_update(task_t *tsk, const struct os01_rlimit64 *new_limit, struct os01_rlimit64 *old_limit);

#endif /* _KERNEL_SCHED_RESOURCE_H */
