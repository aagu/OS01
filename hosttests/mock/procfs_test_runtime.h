/* Host-only runtime surface for compiling the REAL monitor plan Task 4
 * production sources (kernel/fs/procfs_snap.c, kernel/fs/procfs_fmt.c,
 * kernel/sched/identity.c, kernel/arch/thermal.c) for the
 * test_monitor_procfs harness.
 *
 * Pattern follows gfx_test_runtime.h / accounting_test_runtime.h:
 * heavyweight kernel headers are skipped via their include guards and a
 * small controlled surface is provided instead, so the fixture drives
 * the production snapshot producers (not mocks of them).
 *
 * Headers NOT skipped (declared by production headers, stubbed by the
 * test TU): fs/vfs.h, fs/file.h (with -D__SPINLOCK_H__ on the command
 * line), fs/procfs.h, fs/procfs_fmt.h, sched/accounting.h,
 * arch/thermal.h, arch/clocksource.h, memory/pmm.h, subsys/subsys.h,
 * list.h.  The test TU must define every non-inline symbol those
 * reference: kmalloc/kfree, arch_clocksource_read_ns, pmm_get_stats,
 * vfs_read, task_list_lock, init_task_union, current.
 */
#ifndef OS01_PROCFS_TEST_RUNTIME_H
#define OS01_PROCFS_TEST_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <list.h>

/* Skip heavyweight kernel headers via their include guards. */
#define KERNEL_TASK_H
#define _KERNEL_PERCPU_H
#define _KERNEL_SLAB_H
#define _ARCH_CLOCKSOURCE_H   /* mock declares arch_clocksource_read_ns below */

#ifndef NR_CPUS
#define NR_CPUS 8
#endif

#ifdef TEST_PLATFORM_H
/* ── Test TU mode: test_platform.h (FRAMEWORK_INC preinclude) already
 * provided NR_CPUS / spinlock_T + spin_* stubs with the
 * poll_test_spinlock_acquire observation hook / container_of.  Only the
 * task surface and control externs are needed here. */
#else
/* ── Production-object mode: provide the whole platform surface. ── */

/* Spinlock stub — same shape/guard contract as test_platform.h. */
#ifndef OS01_HOST_SPINLOCK_T_PROVIDED
#define OS01_HOST_SPINLOCK_T_PROVIDED
typedef struct { unsigned long lock; } spinlock_T;
void poll_test_spinlock_acquire(spinlock_T *lock) __attribute__((weak));
static inline void spin_init(spinlock_T *l) { l->lock = 1; }
static inline void spin_lock(spinlock_T *l) { (void)l; }
static inline void spin_unlock(spinlock_T *l) { (void)l; }
static inline uint64_t spin_lock_irqsave(spinlock_T *l)
{
    if (poll_test_spinlock_acquire) poll_test_spinlock_acquire(l);
    return 0;
}
static inline void spin_unlock_irqrestore(spinlock_T *l, uint64_t f)
{ (void)l; (void)f; }
#endif /* OS01_HOST_SPINLOCK_T_PROVIDED */

#ifndef container_of
#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - __builtin_offsetof(type, member)))
#endif

#endif /* TEST_PLATFORM_H */

/* ── Task surface (both modes) ────────────────────────────────
 * sched/task.h is skipped, so this mirrors the production fields the
 * snapshot producers touch.  procfs_snap.c container_of()s the list
 * member, so `list` must stay first, exactly like the production
 * task_struct. */
#define TASK_RUNNING       (1 << 0)
#define TASK_INTERRUPTIBLE (1 << 1)
#define TASK_UNINTERRUPTIBLE (1 << 2)
#define TASK_ZOMBIE        (1 << 3)
#define TASK_STOPPED       (1 << 4)
#define PF_KTHREAD         (1 << 0)

#define TASK_COMM_LEN 64

typedef struct task_struct {
    list_t list;                 /* must be first — container_of(list) */
    volatile int64_t state;
    uint64_t flags;
    int64_t pid;
    uint32_t cpu;
    int64_t priority;
    struct task_struct *parent;
    uint64_t birth_id;
    uint64_t start_ns;
    uint64_t runtime_ticks;
    char comm[TASK_COMM_LEN];
} task_t;

/* Task-identity surface (kernel/sched/identity.c) — mirrors the decls
 * in the skipped sched/task.h. */
uint64_t task_next_birth_id(void);
const char *task_basename(const char *path);
void task_identity_init(task_t *tsk, const char *name);
void task_comm_set(task_t *tsk, const char *name);
void task_comm_inherit(task_t *child, const task_t *parent);
int task_comm_apply_exec(task_t *tsk, const char *path);

typedef union {
    task_t task;
    unsigned long long align_[8192];
} procfs_test_task_union_t;

extern procfs_test_task_union_t procfs_test_init_task_union;
#define init_task_union procfs_test_init_task_union

extern spinlock_T task_list_lock;

/* "current" — only used to resolve the /proc/self sentinel. */
extern task_t *procfs_test_current;
#define current procfs_test_current

/* Controlled monotonic clock (identity.c start_ns, snapshot times).
 * arch/clocksource.h is skipped (guard above); the test TU defines this
 * symbol on top of procfs_test_now_ns_storage. */
extern uint64_t procfs_test_now_ns_storage;
uint64_t arch_clocksource_read_ns(void);

/* Slab stubs (memory/slab.h is skipped; the test TU provides the
 * definitions — counting + fail-injection). */
void *kmalloc(size_t size);
size_t kfree(void *ptr);

#endif /* OS01_PROCFS_TEST_RUNTIME_H */
