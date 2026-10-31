/* kernel/sched/kthread.c — arch-neutral kernel-thread mechanics
 * (aarch64 EL1 kernel-thread scheduler, Phase 2).
 *
 * This TU owns:
 *   - the common kernel-thread constructor kthread_alloc_and_publish()
 *   - the public kernel-thread entry points kernel_thread(),
 *     kernel_thread_on_cpu() and create_kthread()
 *   - the kernel-thread exit path kthread_exit()
 *   - the common switch tail task_switch_complete() (Phase 1)
 *
 * Nothing in here is architecture-specific and nothing in here names
 * a VFS, user-MM or framebuffer symbol: the arch-specific half of a
 * kernel thread's resource set lives behind the arch_kthread_prepare()
 * / arch_kthread_drop_files() hooks (<arch/context.h>).
 */

#include <sched/task.h>
#include <sched/internal.h>
#include <sched/boot.h>
#include <percpu/percpu.h>
#include <arch/spinlock.h>
#include <arch/irq.h>
#include <memory/slab.h>
#include <string.h>
#include <errno.h>
#if defined(OS01_SELFTEST) && defined(__aarch64__)
#include <selftest/aarch64_scheduler.h>
#endif

/* The task struct lives at the base of the task_union stack region,
 * so the stack reserve above it must still fit the task struct plus a
 * diagnostic margin.  Enforced at compile time: a task struct that
 * outgrows its own stack is a layout bug, not a runtime condition. */
_Static_assert(sizeof(task_t) + 256 <= STACK_SIZE,
               "task_t does not fit in the task_union stack reserve");

/* ── Placement ────────────────────────────────────────────────
 * The default affinity mask: every CPU the system could reasonably
 * run a task on right now.  Mirrors the historical do_fork() rule (all
 * CPUs up to num_cpus, not just the eligible ones) so an unpinned
 * kernel thread keeps the same reach it had before. */
static uint64_t kthread_default_mask(void)
{
    return (num_cpus >= NR_CPUS) ? ~0ULL : ((1ULL << num_cpus) - 1);
}

/* Choose the CPU an unpinned kernel thread runs on.
 *
 * Eligible means online && scheduler_ok (sched_pick_cpu_mask()).
 * If no CPU is eligible:
 *   - while the BSP boot creation window is open, fall back to CPU0
 *     (spec §8.1: the x86 boot-phase exception).  This is what lets
 *     net_lwip_init() create the lwIP tcpip thread before the BSP
 *     has published scheduler_ok.
 *   - otherwise there is no valid target: -EINVAL.  A runtime
 *     "no eligible target" is NOT a boot-phase case. */
static int kthread_place_unpinned(unsigned *out_cpu)
{
    unsigned c = sched_pick_cpu_mask(kthread_default_mask());
    if (c == NR_CPUS) {
        if (!sched_boot_creation_open())
            return -EINVAL;
        if (!percpu_data[0].online)
            return -EINVAL;
        c = 0;
    }
    *out_cpu = c;
    return 0;
}

/* ── The common constructor ────────────────────────────────────
 * Allocates, initialises and publishes a PF_KTHREAD task.
 *
 * Order matters and is part of the contract: every fallible step
 * happens BEFORE the task becomes visible to another CPU, and the
 * publication order is task_list_lock then rq_lock.
 */
int kthread_alloc_and_publish(uint64_t (*fn)(uint64_t), uint64_t arg,
                               const char *name, unsigned target_cpu,
                               bool pinned, task_t **out_task, int *out_pid)
{
    if (out_task)
        *out_task = NULL;
    if (out_pid)
        *out_pid = 0;

    /* 1. Validate.  Everything before this point is a no-op. */
    if (!fn)
        return -EINVAL;
    if (target_cpu >= NR_CPUS || target_cpu >= num_cpus)
        return -EINVAL;
    if (!percpu_data[target_cpu].online)
        return -EINVAL;
    if (!percpu_data[target_cpu].scheduler_ok) {
        /* The boot-phase exception: an unpinned task may land on CPU0
         * while the BSP boot window is open (CPU0 is online but has
         * not published scheduler_ok yet).  A pinned task never gets
         * the exception, and once the window is closed no CPU0
         * fallback exists at all. */
        bool boot_fallback = !pinned && target_cpu == 0 &&
                             sched_boot_creation_open();
        if (!boot_fallback)
            return -EINVAL;
    }

    /* 2. Allocate the raw task-union space (with alignment slack so
     *    the masked `current` accessor resolves the task) and the
     *    thread context. */
    void *raw = kmalloc(sizeof(union task_union) + STACK_SIZE);
    if (!raw)
        return -ENOMEM;
#if defined(OS01_SELFTEST) && defined(__aarch64__)
    aarch64_sched_observe_resource(raw, 1, true);
#endif
    task_t *tsk = (task_t *)(((uint64_t)raw + STACK_SIZE - 1) &
                             ~(uint64_t)(STACK_SIZE - 1));
    /* Keep the task metadata and diagnostic reserve below the usable
     * stack.  Retain this runtime guard alongside the static assertion:
     * it checks the geometry used by this allocation path itself. */
    if (sizeof(task_t) > STACK_SIZE ||
        (size_t)256 > STACK_SIZE - sizeof(task_t)) {
        kfree(raw);
#if defined(OS01_SELFTEST) && defined(__aarch64__)
        aarch64_sched_observe_resource(raw, 1, false);
#endif
        return -EINVAL;
    }
    thread_t *thd = kmalloc(sizeof(thread_t));
    if (!thd) {
        kfree(raw);
#if defined(OS01_SELFTEST) && defined(__aarch64__)
        aarch64_sched_observe_resource(raw, 1, false);
#endif
        return -ENOMEM;
    }
#if defined(OS01_SELFTEST) && defined(__aarch64__)
    aarch64_sched_observe_resource(thd, 2, true);
#endif

    /* 3. Zero the new task and its thread context. */
    memset(tsk, 0, sizeof(task_t));
    memset(thd, 0, sizeof(thread_t));
    tsk->thread = thd;
    tsk->stack_alloc_base = raw;

    /* 4. Identity, metadata and affinity. */
    task_identity_init(tsk, name);
    tsk->pid        = alloc_pid();
    tsk->parent     = current;
    tsk->pgrp       = current->pgrp;
    tsk->session    = current->session;
    tsk->ctty_type  = current->ctty_type;
    tsk->ctty       = current->ctty;
    tsk->addr_limit = current->addr_limit;
    tsk->priority   = 3;
    tsk->signal     = 0;
    tsk->blocked    = current->blocked;
    tsk->exit_code  = 0;
    tsk->cpu        = target_cpu;

    /* Signal handlers are value-copied from the parent (the kernel
     * thread inherits the parent's handler table, like every other
     * task creation path). */
    for (int sig = 0; sig < NSIG; sig++)
        tsk->sighand[sig] = current->sighand[sig];

    /* Fresh blocker / wait / io nodes: a kernel thread starts with no
     * blocker and with empty wait and io lists. */
    list_init(&tsk->list);
    list_init(&tsk->wait_list);
    list_init(&tsk->io_wait_node);
    tsk->blocker.type = BLOCKER_NONE;
    tsk->blocker.check = NULL;
    tsk->blocker.signal_can_wake = false;

    /* EEVDF fair start: the new task joins the target CPU's timeline
     * no later than its current task, so a fresh kernel thread cannot
     * run ahead of the task that created it. */
    uint64_t fair_start = percpu_data[target_cpu].min_vruntime;
    tsk->vruntime = current->vruntime < fair_start ? current->vruntime
                                                   : fair_start;
    tsk->deadline = tsk->vruntime + EEVDF_MIN_SLICE;

    /* Affinity: a pinned kernel thread owns exactly one CPU; an
     * unpinned one keeps the default reach. */
    tsk->allowed_cpus = pinned ? (1ULL << target_cpu) : kthread_default_mask();

    /* 5. Flags: a kernel thread, and ONLY a kernel thread.  PF_SELF_REAP
     *    and the on-CPU / on-runqueue / in-schedule flags are never
     *    inherited. */
    tsk->flags = PF_KTHREAD;

    /* 6. Architecture resources: the inherited mm/CR3, the copied file
     *    table, the FPU save area and the first-entry frame.  The hook
     *    cleans up and NULLs whatever it acquired when it fails, so
     *    the common rollback only has to free the thread struct and the
     *    raw stack. */
    int rc = arch_kthread_prepare(tsk, fn, arg);
    if (rc) {
        kfree(thd);
        kfree(raw);
#if defined(OS01_SELFTEST) && defined(__aarch64__)
        aarch64_sched_observe_resource(thd, 2, false);
        aarch64_sched_observe_resource(raw, 1, false);
#endif
        return rc;
    }

    /* 7. Publish atomically.  task_list_lock is taken first and the
     *    destination runqueue lock second; both are released in reverse
     *    order.  No fallible operation follows insertion. */
    percpu_t *rq = &percpu_data[target_cpu];
    uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
    uint64_t rq_flags = spin_lock_irqsave(&rq->rq_lock);

    /* Re-validate the placement under the same protected snapshot that
     * publishes the task: the target CPU must still be usable. */
    if (!rq->online ||
        (!rq->scheduler_ok &&
         !(target_cpu == 0 && !pinned && sched_boot_creation_open()))) {
        spin_unlock_irqrestore(&rq->rq_lock, rq_flags);
        spin_unlock_irqrestore(&task_list_lock, tl_flags);
        arch_kthread_drop_files(tsk);
        kfree(thd);
        kfree(raw);
#if defined(OS01_SELFTEST) && defined(__aarch64__)
        aarch64_sched_observe_resource(thd, 2, false);
        aarch64_sched_observe_resource(raw, 1, false);
#endif
        return -EINVAL;
    }

    /* The comm protocol (spec §8.1 / monitor spec §5.1): a NAMED
     * create_kthread published its name before publication (written by
     * task_identity_init above); a plain kernel_thread inherits its
     * creator's comm under task_list_lock — the same lock the procfs
     * snapshot copy uses, so a reader never sees a half-written comm. */
    if (!name)
        task_comm_inherit(tsk, current);

    /* Capture the outputs BEFORE the task becomes visible: a caller
     * never has to look the task up again, and no failure path after
     * this point exists (nothing fallible follows). */
    if (out_task)
        *out_task = tsk;
    if (out_pid)
        *out_pid = (int)tsk->pid;

    list_add_to_before(&init_task_union.task.list, &tsk->list);
    tsk->state = TASK_RUNNING;
    enqueue_task(tsk, rq);

    spin_unlock_irqrestore(&rq->rq_lock, rq_flags);
    spin_unlock_irqrestore(&task_list_lock, tl_flags);

    sched_notify_remote(tsk);
    return 0;
}

/* ── Public kernel-thread API ─────────────────────────────────
 * The signatures and return conventions are unchanged:
 * kernel_thread() and kernel_thread_on_cpu() return the new PID or a
 * negative errno; create_kthread() returns a non-owning task pointer or
 * NULL. */

/* Create a PF_KTHREAD task that runs fn(arg).
 *
 * flags: only 0 and PF_KTHREAD are accepted call shapes; anything
 * else is -EINVAL.  Both accepted values produce a PF_KTHREAD task. */
int kernel_thread(uint64_t (*fn)(uint64_t), uint64_t arg, uint64_t flags)
{
    if (flags & ~(uint64_t)PF_KTHREAD)
        return -EINVAL;

    unsigned cpu;
    int rc = kthread_place_unpinned(&cpu);
    if (rc)
        return rc;

    task_t *tsk = NULL;
    int pid = 0;
    rc = kthread_alloc_and_publish(fn, arg, NULL, cpu, false, &tsk, &pid);
    if (rc)
        return rc;
    return pid;
}

/* Create a PF_KTHREAD task pinned to one CPU.
 *
 * The CPU must be online and have published scheduler_ok: a pinned
 * kernel service may not start on a half-initialised CPU.  Unlike an
 * unpinned creation, this never gets the boot-phase exception. */
int kernel_thread_on_cpu(uint64_t (*fn)(uint64_t), uint64_t arg, unsigned cpu)
{
    if (cpu >= NR_CPUS || cpu >= num_cpus)
        return -EINVAL;
    if (!percpu_data[cpu].online || !percpu_data[cpu].scheduler_ok)
        return -EINVAL;

    task_t *tsk = NULL;
    int pid = 0;
    int rc = kthread_alloc_and_publish(fn, arg, NULL, cpu, true, &tsk, &pid);
    if (rc)
        return rc;
    return pid;
}

/* Create a named PF_KTHREAD task.
 *
 * The returned pointer is a non-owning reference: it must not be
 * dereferenced after the task has self-reaped. */
struct task_struct *create_kthread(uint64_t (*fn)(uint64_t), uint64_t arg,
                                   const char *name)
{
    unsigned cpu;
    if (kthread_place_unpinned(&cpu) != 0)
        return NULL;

    task_t *tsk = NULL;
    int pid = 0;
    if (kthread_alloc_and_publish(fn, arg, name, cpu, false, &tsk, &pid) != 0)
        return NULL;
    return tsk;
}

/* Cooperative yield API. schedule() intentionally returns with IRQs
 * masked after a real switch, so preserve and restore the caller's
 * state here.  Distinct from libc sched_yield(), which is not linked
 * as this implementation. */
void kthread_yield(void)
{
    arch_irq_state_t flags = arch_local_irq_save();
    schedule();
    arch_local_irq_restore(flags);
}

/* ── Kernel-thread exit ───────────────────────────────────────
 * Terminates a kernel thread.  Only kernel threads may call this;
 * the idle task and user tasks are rejected.
 *
 * The shared init_mm is NOT touched: no VMA is freed and no page table
 * is destroyed here (spec §8.3).  The thread's copied file-table
 * reference is released through the arch hook, using the existing pin
 * protocol (detach under task_list_lock, unpin outside it). */
void kthread_exit(uint64_t code)
{
    task_t *self = current;

    if (!self || !(self->flags & PF_KTHREAD) ||
        self == percpu_data[self->cpu].idle) {
        /* Not a kernel thread: refuse.  The idle task must never exit
         * and a user task must go through the user exit path. */
        for (;;)
            arch_cpu_halt();
    }

    /* Release the copied file table (x86) before switching out.  The
     * hook detaches under task_list_lock and unpins outside it. */
    arch_kthread_drop_files(self);

    self->exit_code = code;

    /* On-CPU: our kernel stack is in use until the switch tail clears
     * on_cpu.  A waiter must not free us after we set ZOMBIE below
     * (the final schedule() still runs on this stack). */
    __atomic_store_n(&self->on_cpu, 1, __ATOMIC_RELEASE);

    /* Remove from the global list and mark for self-reclaim.  The
     * transition is atomic under task_list_lock (IRQs off): a tick
     * firing between any two steps could otherwise re-enqueue a
     * still-RUNNING task whose stack the switch tail is about to
     * free. */
    uint64_t fl = spin_lock_irqsave(&task_list_lock);
    list_del(&self->list);
    self->list.next = NULL;
    self->list.prev = NULL;
    self->flags |= PF_SELF_REAP;
    self->state = TASK_ZOMBIE;
    spin_unlock_irqrestore(&task_list_lock, fl);

    /* Switch out permanently: the switch tail frees the thread
     * context, the FPU area and the raw stack. */
    schedule();

    /* A ZOMBIE task is never re-enqueued, so schedule() always
     * switches away.  If a future early-return path (scheduler_ok ==
     * 0, or in_schedule) ever let us fall through, we are off-list +
     * ZOMBIE + PF_SELF_REAP — halt rather than return into the
     * first-entry trampoline's epilogue, which would exit again and
     * double-list_del our already-NULL'd list node. */
    for (;;)
        arch_cpu_halt();
}

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

#if defined(OS01_SELFTEST) && defined(__aarch64__)
    uintptr_t live_sp;
    __asm__ __volatile__("mov %0, sp" : "=r"(live_sp));
    bool next_stack = live_sp < (uintptr_t)actual_prev ||
                      live_sp >= (uintptr_t)actual_prev + STACK_SIZE;
    aarch64_sched_observe_reap(actual_prev, next_stack);
#endif

    task_finish_switch(actual_prev);

    if (self_reap) {
        if (thd) {
#if defined(OS01_SELFTEST) && defined(__aarch64__)
            aarch64_sched_observe_resource(thd, 2, false);
#endif
            kfree(thd);
        }
        if (fpu) {
#if defined(OS01_SELFTEST) && defined(__aarch64__)
            aarch64_sched_observe_resource(fpu, 3, false);
#endif
            kfree(fpu);
        }
        if (stk) {
#if defined(OS01_SELFTEST) && defined(__aarch64__)
            aarch64_sched_observe_resource(stk, 1, false);
#endif
            kfree(stk);
        }
    }
}
