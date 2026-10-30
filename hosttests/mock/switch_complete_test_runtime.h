/* Host-only runtime surface for compiling the REAL
 * kernel/sched/kthread.c (aarch64 EL1 scheduler plan Phase 1, Task 2).
 *
 * Pattern follows accounting_test_runtime.h: heavyweight kernel headers
 * are skipped via their include guards and a small controlled surface is
 * provided instead, so the fixture drives the production
 * task_switch_complete() tail, not a mock of it.
 *
 * Two compilation modes:
 *   - PRODUCTION OBJECT (kernel/sched/kthread.c): this header is the only
 *     preinclude; it provides the whole platform surface with a
 *     test-controlled current plus spy declarations for
 *     task_finish_switch / kfree (the spies live in the test TU).
 *   - TEST TU (test_switch_complete.c): FRAMEWORK_INC preincludes
 *     test_platform.h FIRST; this header then only adds the task_t
 *     fixture type, the spy surface and the control externs.
 */
#ifndef SWITCH_COMPLETE_TEST_RUNTIME_H
#define SWITCH_COMPLETE_TEST_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Minimal task surface: task_switch_complete() only reads flags, thread,
 * fpu_save and stack_alloc_base, and compares the pointer against
 * current. */
typedef struct thread_struct {
    int unused;
} thread_t;

typedef struct task_struct {
    uint64_t flags;
    thread_t *thread;
    void *fpu_save;
    void *stack_alloc_base;
} task_t;

/* Same value as sched/task.h's PF_SELF_REAP (bit 5). */
#define PF_SELF_REAP (1 << 5)

/* ── Production interface under test (kernel/include/sched/internal.h;
 * declared here for BOTH modes because that header is skipped via its
 * include guard below) ── */
void task_switch_complete(task_t *actual_prev);

#ifdef TEST_PLATFORM_H
/* ── Test TU: platform surface already provided by test_platform.h ──
 * Only the control/spy externs are needed here. */
#else
/* ── Production object: provide the whole surface ── */

/* Skip the real kernel headers via their include guards. */
#define KERNEL_TASK_H
#define KERNEL_SCHED_INTERNAL_H
#define _KERNEL_SLAB_H

#endif /* TEST_PLATFORM_H */

/* ── Shared control surface (both modes) ── */

/* The task we are "running as" when the tail executes: on the next
 * task's stack, current identifies next.  The fixture repoints this
 * between cases (including to prev itself for the self-switch case). */
extern task_t *switch_tail_current;
#define current switch_tail_current

/* Spy declarations — DEFINED in the test TU, called by the production
 * object.  kfree keeps the production <memory/slab.h> signature. */
void task_finish_switch(task_t *prev);
size_t kfree(void *address);

/* ── Spy state (owned by the test TU; asserted by the cases) ── */
extern int spy_finish_calls;        /* task_finish_switch invocations */
extern task_t *spy_finish_last;     /* its last argument */
extern int spy_seq_finish;          /* op counter value at finish */
extern int spy_kfree_calls;         /* kfree invocations (incl. overflow) */
extern void *spy_kfreed[8];         /* the first 8 freed pointers */
extern int spy_seq_kfree[8];        /* op counter value at each kfree */
void switch_tail_reset(void);       /* zero all spy state + prev fixture */

/* Poison sentinels the mock task_finish_switch writes into prev: a tail
 * that reads prev AFTER finish (the read-after-free window) frees one of
 * these instead of the real pointers, which the cases detect. */
#define SWITCH_TAIL_POISON_THREAD ((thread_t *)0xbaad000000000001ULL)
#define SWITCH_TAIL_POISON_FPU    ((void *)0xbaad000000000002ULL)
#define SWITCH_TAIL_POISON_STACK  ((void *)0xbaad000000000003ULL)

#endif /* SWITCH_COMPLETE_TEST_RUNTIME_H */
