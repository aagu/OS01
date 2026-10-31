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
/* Mask-aware picker (thermal plan Task 2): returns a cpu
 * 0 ≤ cpu < NR_CPUS that is online, scheduler_ok, and whose
 * bit is set in @allowed.  Returns NR_CPUS when no such cpu
 * exists.  Internally power-of-cap-aware (prefers fewer bits
 * beyond num_cpus so a mask wider than the live set still
 * finds a target).  Old sched_pick_cpu is the all-cpus wrapper
 * for back-compat. */
unsigned sched_pick_cpu_mask(uint64_t allowed);
void sched_notify_remote(task_t *tsk);

/* ── Internal helper (exec.c / fork.c shared) ── */
void *fpu_area_alloc(void);

/* ── Common switch tail (kthread.c; aarch64 EL1 scheduler spec §8.3) ── */
/* Runs on the *next* task's stack, after the architecture has installed
 * next's context. Captures the self-reap decision and the objects to
 * release BEFORE task_finish_switch() clears on_cpu, because after that
 * a waiter may free prev. Never reads prev after task_finish_switch()
 * returns. The single-argument signature is frozen in the series index:
 * on next's stack `current` identifies next, so the self-switch case
 * (actual_prev == current) returns without finish or free. */
void task_switch_complete(task_t *actual_prev);

/* Notify a destination CPU without retaining a task pointer across
 * publication unlock/reclamation. */
void sched_notify_remote_cpu(unsigned cpu);

/* ── Common kernel-thread constructor (kthread.c) ───────────────────
 * Allocates, initialises and publishes a PF_KTHREAD task.
 *
 * Publication ordering is part of the contract: every fallible step
 * happens BEFORE the task becomes visible to another CPU, and the
 * publication order is task_list_lock then rq_lock.
 *
 * target_cpu: the CPU the task is published on.
 * pinned:     true  -> the task is pinned to target_cpu and its
 *                       allowed_cpus mask is exactly that CPU.
 *             false -> the task may use the default affinity mask.
 *
 * out_task receives the non-owning task pointer, out_pid the PID.
 * Both are captured BEFORE publication; a caller must not dereference
 * out_task after the task has self-reaped. */
int kthread_alloc_and_publish(uint64_t (*fn)(uint64_t), uint64_t arg,
                               const char *name, unsigned target_cpu,
                               bool pinned, task_t **out_task, int *out_pid);

/* ── Kernel-thread exit path (kthread.c) ──────────────────────────
 * Terminates a kernel thread: releases the thread's copied file-table
 * reference, removes it from the global task list, marks it
 * PF_SELF_REAP + TASK_ZOMBIE, and switches away permanently.  The
 * shared init_mm is NOT touched: no VMA is freed and no page table is
 * destroyed (spec §8.3).
 *
 * Only kernel threads may call this; the idle task and user tasks are
 * rejected. */
void kthread_exit(uint64_t code);

#endif /* KERNEL_SCHED_INTERNAL_H */
