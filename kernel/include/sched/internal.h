#ifndef KERNEL_SCHED_INTERNAL_H
#define KERNEL_SCHED_INTERNAL_H

#include <sched/task.h>
#include <percpu/percpu.h>

/* ── Global/core state access ── */
extern task_t *user_init_task;
extern int64_t  user_init_pid;
pid_t alloc_pid(void);

/* ── EEVDF algorithm constants & interface (fair.c) ── */
#define EEVDF_MIN_SLICE  10   // time slice = 10 ticks = 100ms
#define EEVDF_LATENCY    40   // eligibility window = 40 ticks = 400ms

void update_curr(task_t *task);
int cmp_deadline(rbtree_node_t *a, rbtree_node_t *b);
void enqueue_task(task_t *task, percpu_t *rq);
void dequeue_task(task_t *task, percpu_t *rq);
task_t *pick_eevdf(percpu_t *rq);

/* ── Load balance & CPU placement (balance.c) ── */
void sched_balance(percpu_t *rq);
uint32_t sched_pick_cpu(void);
void sched_notify_remote(task_t *tsk);

/* ── Internal helper (exec.c / fork.c shared) ── */
void *fpu_area_alloc(void);

#endif /* KERNEL_SCHED_INTERNAL_H */
