#include <kernel/task.h>
#include <kernel/percpu.h>
#include <kernel/ipi.h>
#include <kernel.h>
#include <kernel/arch/gate.h>
#include <kernel/arch/spinlock.h>
#include <kernel/arch/irq.h>
#include <kernel/hang.h>
#include <kernel/debug.h>
#include <kernel/log.h>
#include <kernel/memory.h>
#include <kernel/pmm.h>
#include <kernel/vma.h>
#include <kernel/vmm.h>
#include <kernel/slab.h>

#include <fs/vfs.h>
#include <fs/elf.h>

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <uapi/time.h>
#include <kernel/deferred_free.h>    // deferred_free() for zombie reaping
#include <kernel/assert.h>

// ── Preemption flag ──────────────────────────────────────
// Now per-CPU (percpu_t.need_resched, offset 8 from GS base).
// Set by timer IRQ on every tick, cleared by schedule() after
// a context switch.  entry.S reads it via %gs:8.

// ── Global task list lock (SMP) ──────────────────────────
// Protects all traversals and modifications to
// init_task_union.task.list.  schedule() paths use
// spin_trylock_irqsave — if the lock is contended they
// skip one cycle (no deadlock possible).
static spinlock_T task_list_lock = { .lock = 1L };

// ── Safe task-list iteration ─────────────────────────────
// The global task list is written (list_add_to_before) without
// synchronisation against readers (sched_unblock_blocked etc.).
// On SMP a reader may see a partially-updated ->next, including
// NULL.  Advance with the guard below to survive the window.
static inline list_t *task_list_next(list_t *pos)
{
    list_t *next = pos->next;
    if ((uintptr_t)next < 0x1000) {
        log_err("[sched] list corruption at %p (next=%p) — breaking\n",
                (void *)pos, (void *)next);
        return NULL;
    }
    return next;
}

/* ── EEVDF scheduler constants ─────────────────────── */
#define EEVDF_MIN_SLICE  10   // time slice = 10 ticks = 100ms
#define EEVDF_LATENCY    40   // eligibility window = 40 ticks = 400ms

/* ── Forward declarations for load balancing ─────────── */
static void sched_balance(percpu_t *rq);

/* ── sched_pick_cpu: choose CPU with fewest nr_running ────
 * Called from do_fork() and spawn_user_task() to place
 * new tasks on the least-loaded CPU.
 * Complexity: O(num_cpus).  Acceptable for NR_CPUS ≤ 8.
 */
static uint32_t sched_pick_cpu(void)
{
    uint32_t me = cpu_id();
    uint32_t best = me;
    uint32_t min_nr = *(volatile uint32_t *)&percpu_data[me].nr_running;

    for (uint32_t i = 0; i < num_cpus; i++) {
        if (!percpu_data[i].online) continue;
        uint32_t nr = *(volatile uint32_t *)&percpu_data[i].nr_running;
        if (nr < min_nr) {
            min_nr = nr;
            best = i;
        }
    }
    return best;
}

/* ── sched_notify_remote: wake remote CPU after enqueue ──
 * Sets need_resched and sends reschedule IPI so the remote
 * CPU discovers the task immediately, not up to 10 ms later.
 *
 * If ipi_send() times out (10K ICR poll), the IPI is silently
 * dropped.  need_resched=1 is the fallback: the remote CPU
 * picks it up on the next LAPIC timer tick (≤10 ms).
 *
 * Called from do_fork() and spawn_user_task() after enqueue.
 */
static void sched_notify_remote(task_t *tsk)
{
    if ((int)tsk->cpu == (int)cpu_id())
        return;
    percpu_t *dst = &percpu_data[tsk->cpu];
    dst->need_resched = 1;
    __sync_synchronize();
    ipi_send(dst->arch_processor_id, IPI_VECTOR_RESCHED);
}

/* ── update_curr: advance vruntime by 1 tick ──────── */
static void update_curr(task_t *task)
{
    if (!task || task == this_cpu()->idle)
        return;
    task->vruntime += 1;
    if (task->vruntime >= task->deadline)
        this_cpu()->need_resched = 1;
}

/* ── rbtree comparator: order by deadline ─────────── */
static int cmp_deadline(rbtree_node_t *a, rbtree_node_t *b)
{
    task_t *ta = container_of(a, task_t, rb_node);
    task_t *tb = container_of(b, task_t, rb_node);
    if (ta->deadline < tb->deadline) return -1;
    if (ta->deadline > tb->deadline) return 1;
    if (ta->pid < tb->pid) return -1;
    if (ta->pid > tb->pid) return 1;
    return (uintptr_t)a < (uintptr_t)b ? -1 : 1;
}

/* ── enqueue / dequeue ─────────────────────────────── */
static void enqueue_task(task_t *task, percpu_t *rq)
{
    /*
     * Idle tasks must never appear on a runqueue.  They are
     * always RUNNING and selected only as a last resort when
     * pick_eevdf() finds the rbtree empty.
     */
    ASSERT(task != rq->idle);

    task->deadline = task->vruntime + EEVDF_MIN_SLICE;
    task->on_rq = true;
    rbtree_node_t *conflict = rbtree_insert(&rq->run_queue, &task->rb_node, cmp_deadline);
    ASSERT(conflict == NULL);
    rq->nr_running++;
}

static void dequeue_task(task_t *task, percpu_t *rq)
{
    rbtree_erase(&rq->run_queue, &task->rb_node);
    task->on_rq = false;
    rq->nr_running--;
}

/* ── pick_eevdf: select next task O(log n) ─────────── */
static task_t *pick_eevdf(percpu_t *rq)
{
    if (rbtree_empty(&rq->run_queue))
        return rq->idle;
    rbtree_node_t *node = rbtree_first(&rq->run_queue);
    task_t *t = container_of(node, task_t, rb_node);
    if (t->vruntime > rq->min_vruntime + EEVDF_LATENCY)
        rq->min_vruntime = t->vruntime;
    return t;
}

/* ── task_wake: mark RUNNING + enqueue (exported) ─── */
void task_wake(task_t *t)
{
    /*
     * Never enqueue the idle task — it is always RUNNING on its
     * CPU and must never appear on a runqueue.
     */
    if (t == percpu_data[t->cpu].idle)
        return;

    t->state = TASK_RUNNING;

retry:
    /*
     * Read t->cpu locklessly — sched_balance may change it concurrently.
     * We re-check both t->cpu and t->on_rq under the acquired rq_lock
     * to close the race window.
     */
    percpu_t *rq = &percpu_data[*(volatile uint32_t *)&t->cpu];
    uint64_t flags = spin_lock_irqsave(&rq->rq_lock);

    /* Re-check on_rq under lock — sched_balance may have enqueued it */
    if (t->on_rq) {
        spin_unlock_irqrestore(&rq->rq_lock, flags);
        return;
    }

    /* Re-check t->cpu under lock — sched_balance may have migrated it */
    if ((uintptr_t)rq != (uintptr_t)&percpu_data[*(volatile uint32_t *)&t->cpu]) {
        spin_unlock_irqrestore(&rq->rq_lock, flags);
        goto retry;
    }

    /* Wakeup boost: prevent starvation by raising vruntime floor */
    uint64_t wake_vruntime = rq->min_vruntime > EEVDF_LATENCY
        ? rq->min_vruntime - EEVDF_LATENCY : 0;
    if (t->vruntime < wake_vruntime)
        t->vruntime = wake_vruntime;

    enqueue_task(t, rq);
    t->cpu = rq->cpu_id;  // keep t->cpu in sync with actual rq

    spin_unlock_irqrestore(&rq->rq_lock, flags);

    if ((int)t->cpu != (int)cpu_id())
        rq->need_resched = 1;
}

// Global PID counter — atomic because spawn/fork/exec may
// race on different CPUs.
static volatile uint64_t pid_counter = 1;

// ── User-space init task pointer ─────────────────────────
// Set by spawn_user_task() the first time it creates a user task.
// do_exit() uses this to reparent orphans and protect the init process.
static task_t *user_init_task = NULL;
static int64_t  user_init_pid = 0;

// Per-CPU scheduler guard — set to 1 by task_init() on each CPU.
// schedule() returns immediately before this point (ticks before
// the scheduler is set up are harmless no-ops).

/*
 * Wake a blocked task if its condition is met.
 *
 * Called by do_exit() (explicit wakeup for fast path) and
 * by sched_unblock_blocked() (scan-based fallback).
 * Only wakes if the condition callback returns true.
 */
void blocker_wake(task_t *task)
{
    // Only wake blocked tasks whose condition is actually met
    if (task->state != TASK_INTERRUPTIBLE && task->state != TASK_UNINTERRUPTIBLE)
        return;
    if (task->blocker.type == BLOCKER_NONE)
        return;
    if (task->blocker.check && !task->blocker.check(task))
        return;

    // Condition met — wake up
    task_wake(task);
    task->blocker.type = BLOCKER_NONE;
    task->blocker.check = NULL;
}

/*
 * Scan all tasks for blocked ones whose conditions are now met.
 *
 * Called from schedule() inside the reap_lock critical section,
 * after zombie reaping and before the priority scan.
 *
 * Also handles signal-based wakeup: if a blocker has signal_can_wake=true
 * and the blocked task has pending signals, wake it with -EINTR return.
 */
void sched_unblock_blocked(void)
{  // Caller MUST hold task_list_lock (only called from schedule()).
    list_t *pos = init_task_union.task.list.next;
    while (pos != &init_task_union.task.list) {
        if ((uintptr_t)pos < 0x1000) {
            log_err("[sched] unblock scan: corrupted list pointer %p, breaking\n",
                    (void *)pos);
            break;
        }
        task_t *t = container_of(pos, task_t, list);
        pos = task_list_next(pos);

        if (t->state != TASK_INTERRUPTIBLE)
            continue;
        if (t->blocker.type == BLOCKER_NONE)
            continue;

        // Check condition callback — use blocker_wake which
        // verifies the condition before setting RUNNING.
        if (t->blocker.check && t->blocker.check(t)) {
            blocker_wake(t);
            continue;
        }

        // Check signal wakeup (bypass condition check — the
        // callback returned false, so we're waking for a signal).
        if (t->blocker.signal_can_wake && t->signal) {
            task_wake(t);
            t->blocker.type = BLOCKER_NONE;
            t->blocker.check = NULL;
        }
    }
}

/*
 * Block the current task until condition is met or signal arrives.
 *
 * 1. Checks condition first via callback — if already true, returns 0 immediately.
 *    This closes the SMP race window that the old do_waitpid pattern had.
 * 2. If condition not met: installs blocker, marks as TASK_INTERRUPTIBLE.
 * 3. Double-checks condition (one more time — catches edge case between step 1 and 2).
 * 4. Calls schedule().
 * 5. On return: clears blocker, returns 0 (condition met) or -EINTR (signal woke us).
 */
int blocker_wait(blocker_check_t check, int type, bool signal_can_wake)
{
    task_t *self = current;
    // It doesn't make sense to call blocker_wait in an interrupt handler
    // or with a NULL check callback.
    if (!check)
        return -EINVAL;

    // Step 1: Check condition first — if already met, don't block at all.
    // This is the critical part that prevents lost-wakeup: even if the
    // caller already checked, we re-check here atomically before sleeping.
    if (check(self))
        return 0;

    // Step 2: Install blocker
    self->blocker.type = type;
    self->blocker.check = check;
    self->blocker.signal_can_wake = signal_can_wake;

    // Step 3: Set interruptible state
    self->state = TASK_INTERRUPTIBLE;

    // Step 4: Double-check condition (our state change is visible now;
    // a concurrent do_exit() might have already checked our state and
    // set us back to RUNNING — if so, don't call schedule())
    if (check(self)) {
        self->blocker.type = BLOCKER_NONE;
        self->blocker.check = NULL;
        self->state = TASK_RUNNING;
        return 0;
    }

    // Step 5: Give up CPU. schedule() will run sched_unblock_blocked()
    // which finds us and wakes us when the condition is met.
    // We may also be woken explicitly by blocker_wake() from do_exit().
    schedule();
    // Woke up — clear blocker and check why
    self->blocker.type = BLOCKER_NONE;
    self->blocker.check = NULL;
    self->state = TASK_RUNNING;

    // Step 6: Check if woken by signal.
    // Re-check the condition first: if it's now met (e.g. child
    // exited AND SIGCHLD was delivered), return 0 — the signal
    // will be handled on the way back to userspace.
    if (signal_can_wake && self->signal && !check(self))
        return -EINTR;

    return 0;
}

/* ── sched_balance: pull or steal tasks from busiest CPU ──
 *
 * Called from schedule() after zombie reaping, before pick_eevdf().
 *
 * Algorithm:
 *   1. Find busiest CPU (max nr_running, tiebreak max min_vruntime)
 *   2. Gate: proceed if local is idle OR gap >= 2 tasks
 *   3. Steal count = max(1, (src - local) / 2) from rbtree tail
 *   4. Double-lock rq_locks (address-ordered), single IRQ save
 *   5. For each task: dequeue from src, normalize vruntime, enqueue to local
 *   6. If src now empty: src.min_vruntime = 0
 *
 * Takes from the tail (largest deadline) — tasks that just used
 * their slice and won't be scheduled again soon.  Preserves source
 * CPU's hot-cache "about to run" tasks.
 */
static void sched_balance(percpu_t *rq)
{
    /* 1. Find busiest online CPU */
    int src_idx = -1;
    uint32_t max_nr = 0;
    uint64_t max_vr = 0;

    for (uint32_t i = 0; i < num_cpus; i++) {
        if (i == rq->cpu_id || !percpu_data[i].online)
            continue;
        uint32_t nr = *(volatile uint32_t *)&percpu_data[i].nr_running;
        if (nr == 0)
            continue;
        uint64_t vr = *(volatile uint64_t *)&percpu_data[i].min_vruntime;
        if (nr > max_nr || (nr == max_nr && vr > max_vr)) {
            max_nr = nr;
            max_vr = vr;
            src_idx = (int)i;
        }
    }
    if (src_idx < 0)
        return;

    /* 2. Gate */
    if (rq->nr_running > 0) {
        /* Non-idle: require >= 2 task gap to prevent oscillation */
        if (max_nr <= rq->nr_running + 1)
            return;
    }
    /* rq->nr_running == 0: idle steal — unconditional */

    /* 3. Determine steal count */
    int count = (int)(max_nr - rq->nr_running) / 2;
    if (count < 1) count = 1;

    percpu_t *src_rq = &percpu_data[src_idx];

    /* 4. Double-lock, address-ordered, single IRQ save */
    spinlock_T *lo, *hi;
    if ((uintptr_t)&src_rq->rq_lock < (uintptr_t)&rq->rq_lock) {
        lo = &src_rq->rq_lock; hi = &rq->rq_lock;
    } else {
        lo = &rq->rq_lock; hi = &src_rq->rq_lock;
    }

    uint64_t flags = arch_local_irq_save();
    spin_lock(lo);
    if (lo != hi) spin_lock(hi);

    /* 5. Steal from tail */
    rbtree_node_t *node = rbtree_last(&src_rq->run_queue);
    int taken = 0;

    while (node && taken < count) {
        task_t *t = container_of(node, task_t, rb_node);

        /* Advance BEFORE erase (rbtree_erase invalidates node's
         * parent/left/right pointers used by rbtree_prev) */
        rbtree_node_t *prev = rbtree_prev(node);

        if (t != src_rq->idle) {
            dequeue_task(t, src_rq);   // rbtree_erase + on_rq=false + nr_running--
            t->cpu = rq->cpu_id;

            /* Normalize vruntime to target CPU's timeline */
            if (t->vruntime < rq->min_vruntime)
                t->vruntime = rq->min_vruntime;

            enqueue_task(t, rq);       // deadline set + on_rq=true + rbtree_insert + nr_running++
            taken++;
        }
        node = prev;
    }

    /* 6. Reset min_vruntime if source is now empty */
    if (src_rq->nr_running == 0)
        src_rq->min_vruntime = 0;

    spin_unlock(hi);
    if (lo != hi) spin_unlock(lo);
    arch_local_irq_restore(flags);

    if (taken > 0) {
        debug_sched("balance: CPU%u <- %d tasks from CPU%d (src_nr=%u local_nr=%u)\n",
                    rq->cpu_id, taken, src_idx,
                    (unsigned)src_rq->nr_running, (unsigned)rq->nr_running);
    }
}

void schedule(void)
{
    percpu_t *rq = this_cpu();
    if (!rq->scheduler_ok)
        return;

    rq->schedule_count++;

    // ── Hang detector ──────────────────────────────────
    if (rq->watchdog_counter >= HANG_THRESHOLD) {
        log_info("[hang] CPU %u recovered (watchdog=%lu ticks)\n",
                 (unsigned)cpu_id(), (unsigned long)rq->watchdog_counter);
        hang_dump_all();
    }
    rq->watchdog_counter = 0;

    // ── 1. Update current task's vruntime ──────────────────
    update_curr(current);

    // ── 2. Dequeue + conditional re-enqueue current ─────────
    {
        uint64_t rq_flags = spin_lock_irqsave(&rq->rq_lock);
        if (current->on_rq)
            dequeue_task(current, rq);
        if (current->state == TASK_RUNNING && current != rq->idle)
            enqueue_task(current, rq);
        spin_unlock_irqrestore(&rq->rq_lock, rq_flags);
    }

    // ── 3. Zombie reaper (global list, with on_rq guard) ──
    {
        uint64_t reap_flags = spin_lock_irqsave(&task_list_lock);

        task_t *reap_list[64];
        int reap_count = 0;

        {
            list_t *pos = init_task_union.task.list.next;
            while (pos != &init_task_union.task.list && reap_count < 64) {
                if ((uintptr_t)pos < 0x1000) {
                    log_err("[sched] zombie scan: corrupted list pointer %p\n", (void *)pos);
                    break;
                }
                task_t *t = container_of(pos, task_t, list);
                pos = task_list_next(pos);
                if (t->state != TASK_ZOMBIE || t == current) continue;
                if (t->on_rq) continue;  // not yet dequeued — skip

                int reap = 0;
                if (t->flags & PF_REAPED) reap = 1;
                else if (t->flags & PF_KTHREAD) reap = 1;
                else if (t->parent == NULL) reap = 1;
                else if (t->parent->state == TASK_ZOMBIE) reap = 1;

                if (reap) reap_list[reap_count++] = t;
            }
        }

        if (reap_count > 0) {
            list_t *pos = init_task_union.task.list.next;
            while (pos != &init_task_union.task.list) {
                if ((uintptr_t)pos < 0x1000) break;
                task_t *child = container_of(pos, task_t, list);
                pos = task_list_next(pos);
                if (!child->parent) continue;
                for (int i = 0; i < reap_count; i++) {
                    if (child->parent == reap_list[i]) {
                        child->parent = NULL;
                        break;
                    }
                }
            }
        }

        for (int i = 0; i < reap_count; i++) {
            task_t *t = reap_list[i];
            list_del(&t->list);
            t->list.next = NULL;
            t->list.prev = NULL;
            if (t->thread) kfree(t->thread);
            if (t->files) deferred_files_free(t->files);
            if (t->fpu_save) kfree(t->fpu_save);
            if (t->stack_alloc_base) deferred_kfree(t->stack_alloc_base);
        }

        sched_unblock_blocked();
        spin_unlock_irqrestore(&task_list_lock, reap_flags);
    }

    // ── 3.5 Load balancing ───────────────────────────────
    sched_balance(rq);

    // ── 4. Pick next task (rbtree O(log n)) ─────────────────
    task_t *next;
    {
        uint64_t rq_flags = spin_lock_irqsave(&rq->rq_lock);
        next = pick_eevdf(rq);
        if (next && next != rq->idle)
            dequeue_task(next, rq);
        spin_unlock_irqrestore(&rq->rq_lock, rq_flags);
    }

    // ── 5. Fallback to idle ─────────────────────────────────
    if (!next || next->state != TASK_RUNNING) {
        if (next)
            log_err("sched: orphan task %d (state=%ld), falling back to idle\n",
                    (int)next->pid, (long)next->state);
        next = rq->idle;
        if (!next) return;
    }

    // ── 6. Preemption guard: if the best candidate is still
    //        current and it hasn't exhausted its time slice,
    //        skip the context switch.
    if (next == current && current->vruntime < current->deadline) {
        rq->need_resched = 0;
        return;
    }

    // ── 6. Update min_vruntime ──────────────────────────────
    if (next != rq->idle && next->vruntime > rq->min_vruntime)
        rq->min_vruntime = next->vruntime;

    rq->need_resched = 0;
    switch_to(current, next);
}

// ── Task exit ──────────────────────────────────────────────
// do_exit() frees user page tables and physical pages, then
// marks the task ZOMBIE. thread_t and task_union are freed
// later by the zombie reaper in schedule() (deferred because
// __switch_to dereferences current->thread, and we're running
// on the kernel stack inside task_union).
//
uint64_t do_exit(uint64_t exit_code)
{
    debug_task("task %d exiting with code %#018lx\n", current->pid, exit_code);

    // ── Init process protection ──────────────────────────
    // The user-space init process (PID 1) must never exit.
    // Check by pid rather than pointer — after fork, child
    // task structs are copies and pointer comparison fails.
    if (current->pid == user_init_pid) {
        debug_task("PANIC: init (pid=%d) attempted to exit with code %#018lx\n",
                      (int)current->pid, exit_code);
        while (1) { __asm__ __volatile__("hlt"); }
    }

    // ── Reparent children to init ────────────────────────
    if (user_init_task && current->pid != user_init_pid) {
        uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
        list_t *cpos = init_task_union.task.list.next;
        while (cpos != &init_task_union.task.list) {
            task_t *child = container_of(cpos, task_t, list);
            cpos = cpos->next;
            if (child->parent == current) {
                child->parent = user_init_task;
                debug_task("reparent: child %d → init (pid=%d)\n",
                              (int)child->pid, (int)user_init_task->pid);
            }
        }
        spin_unlock_irqrestore(&task_list_lock, tl_flags);
    }

    // ── Send SIGCHLD to parent ───────────────────────────
    // NOTE: we write-protect parent->state here because we are
    // about to become ZOMBIE.  After this point, the parent may
    // run and reap us via do_waitpid → PF_REAPED.  The scheduler's
    // zombie reaper will later list_del + kfree.
    if (current->parent && !(current->parent->flags & PF_KTHREAD)) {
        __sync_fetch_and_or(&current->parent->signal, (1ULL << SIGCHLD));
        // Use blocker_wake which checks condition callback before waking.
        // This is the explicit fast path; sched_unblock_blocked() in
        // schedule() is the reliable fallback.
        if (current->parent->blocker.type != BLOCKER_NONE)
            blocker_wake(current->parent);
    }

    // Free VMA-managed pages (anon + file-backed unmaps, not 2MB ELF pages)
    vma_free_all(current->mm);

    if (!(current->flags & PF_KTHREAD) && current->mm) {
        uint64_t *pml4_virt = (uint64_t *)Phy_To_Virt((uint64_t)current->mm->pml4);
        bool mm_is_shared = (current->parent != NULL &&
                             current->parent->mm == current->mm);
        if (!mm_is_shared && current->mm->pml4)
            vmm_free_user_map(pml4_virt);
        kfree(current->mm);
        current->mm = NULL;
    }

    // Close all file descriptors
    if (current->files) {
        files_free(current->files);
        current->files = NULL;
    }

    current->exit_code = exit_code;
    current->state = TASK_ZOMBIE;

    // ── Direct switch to parent ─────────────────────────────
    // By the time we reach ZOMBIE the parent is either already
    // RUNNING (SIGCHLD woke it) or still INTERRUPTIBLE in
    // do_waitpid.  In either case we want to switch directly to
    // avoid schedule()'s round-robin scan which may pick the
    // idle task instead (task list order changes after zombie
    // reaping inside schedule() can cause this).
    task_t *parent = current->parent;
    int parent_woken = 0;

    if (parent) {
        uint64_t ps = parent->state;
        if (ps == TASK_INTERRUPTIBLE) {
            // EEVDF: don't set RUNNING here — task_wake handles it at end of do_exit
            parent_woken = 1;
        } else if (ps == TASK_RUNNING) {
            // SIGCHLD already woke the parent.
            parent_woken = 1;
        } else if (ps == TASK_UNINTERRUPTIBLE) {
            // Parent is in an unkillable sleep — unlikely for
            // waitpid but handle gracefully: leave it, the
            // scheduler's zombie reaper will eventually clean
            // us up if parent never comes back.
            debug_task("exit: p%d parent p%d UNINTERRUPTIBLE (%ld), "
                          "skipping direct switch\n",
                          current->pid, parent->pid, ps);
        }
    }

    debug_task("task %d now ZOMBIE (parent=%d w=%d ps=%ld)\n",
                  current->pid,
                  parent ? (int)parent->pid : -1,
                  parent_woken,
                  parent ? (long)parent->state : -1);

    // Transfer directly to the woken parent to avoid scheduler
    // scan races.  parent_woken is always 1 for normal exit
    // (parent in waitpid = INTERRUPTIBLE or RUNNING).
    if (parent_woken) {
        task_wake(parent);
    }
    current->state = TASK_ZOMBIE;
    schedule();
    return 0;  // unreachable
}

/*
 * Blocker condition callback for do_waitpid.
 * Returns true when a waited-for child has become TASK_ZOMBIE.
 * Sets waiter->blocker_data.waited_child so the caller can reap it immediately.
 */
static bool waitpid_should_unblock(task_t *waiter)
{
    int64_t target_pid = waiter->blocker_data.waited_pid;
    list_t *pos = init_task_union.task.list.next;

    while (pos != &init_task_union.task.list) {
        task_t *t = container_of(pos, task_t, list);
        pos = task_list_next(pos);

        if (t->parent != waiter)
            continue;
        if (t->flags & PF_REAPED)
            continue;
        if (target_pid != -1 && t->pid != target_pid)
            continue;
        if (t->state == TASK_ZOMBIE) {
            waiter->blocker_data.waited_child = t;
            return true;
        }
    }
    return false;
}

// ── do_waitpid ────────────────────────────────────────────
// Block until a child with <pid> exits, or return immediately
// if WNOHANG is set and no child is ready.
//
// pid > 0  → wait for specific child
// pid == -1 → wait for any child
// Returns child PID on success, -ECHILD if no such child, -EINTR if interrupted.
int64_t do_waitpid(int64_t pid, int *user_status, int options)
{
    for (;;) {
        task_t *child = NULL;
        list_t *pos;

        // Pass 1: scan for a matching, unreaped ZOMBIE child
        {
            uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
            pos = init_task_union.task.list.next;
            while (pos != &init_task_union.task.list) {
                task_t *t = container_of(pos, task_t, list);
                if (!(t->flags & PF_REAPED) &&
                    t->parent == current &&
                    t->state == TASK_ZOMBIE &&
                    (pid == -1 || t->pid == pid)) {
                    child = t;
                    break;
                }
                pos = task_list_next(pos);
            }
            spin_unlock_irqrestore(&task_list_lock, tl_flags);
        }

        if (child) {
            int64_t child_pid = child->pid;
            int64_t exit_code = child->exit_code;

            if (user_status) {
                int status = (int)((exit_code & 0xFF) << 8);
                if ((uint64_t)user_status < current->addr_limit)
                    *user_status = status;
            }
            // Mark reaped — schedule()'s zombie reaper will list_del+kfree.
            // We MUST NOT touch child->list here: on SMP the child may still
            // be running schedule() → container_of(current->list.next, …),
            // and list_del/list_add_to_before would corrupt that.
            __sync_fetch_and_or(&child->flags, PF_REAPED);

            debug_task("waitpid: pid=%d reaped child %d (exit=%d)\n",
                          (int)current->pid, (int)child_pid, (int)exit_code);
            return child_pid;
        }

        debug_task("waitpid: pid=%d search for child pid=%d\n",
                      current ? (int)current->pid : -1, (int)pid);
        // Check if the child even exists (unreaped, still running)
        int child_exists = 0;
        int child_count = 0;
        debug_task("waitpid: child-list cur=%d pid=%lld:", current->pid, (long long)pid);
        {
            uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
            pos = init_task_union.task.list.next;
            while (pos != &init_task_union.task.list) {
                task_t *t = container_of(pos, task_t, list);
                if (t->parent == current && !(t->flags & PF_REAPED)) {
                    child_count++;
                    debug_task(" [%d s=%d f=%lx p=%lx]",
                        t->pid, t->state, t->flags, (unsigned long)t->parent);
                    if (pid == -1 || t->pid == pid)
                        child_exists = 1;
                }
                pos = task_list_next(pos);
            }
            spin_unlock_irqrestore(&task_list_lock, tl_flags);
        }
        debug_task(" count=%d exist=%d\n", child_count, child_exists);

        if (!child_exists) {
            debug_task("waitpid: pid=%d returning ECHILD (no child pid=%lld)\n",
                          current ? (int)current->pid : -1, (long long)pid);
            return -ECHILD;
        }

        if (options & WNOHANG)
            return 0;

        // Block until a child exits or signal arrives.
        // blocker_wait() handles the condition check atomically
        // (checks before sleeping, double-checks after state change,
        // and is rechecked by sched_unblock_blocked in schedule()).
        current->blocker_data.waited_pid = pid;
        current->blocker_data.waited_child = NULL;

        int ret = blocker_wait(waitpid_should_unblock, BLOCKER_WAITPID, true);
        if (ret == -EINTR) {
            debug_task("waitpid: pid=%d woken by signal, retrying\n",
                          current->pid);
            continue;  // re-check for children before sleeping again
        }
        // blocker_wait returned 0 → condition was met
        // (waited_child was set by the callback)
        child = current->blocker_data.waited_child;
        continue;
    }
}

// kernel_thread_func is defined in arch/x86_64/thread_entry.S.
extern void kernel_thread_func(void);

#define USER_CODE_ADDR   0x400000UL

// ── FPU helper ──────────────────────────────────────────────
// Alloc a 512+15 byte buffer for FXSAVE/FXRSTOR.  The returned
// pointer is the raw malloc block (for kfree).  Users must align
// it to 16 bytes before passing to fxsave64/fxrstor64.
// Sets FCW=0x037F (default x87 control word) and MXCSR=0x1F80
// (default SSE control/status).
static void *fpu_area_alloc(void)
{
    char *raw = (char *)malloc(512 + 16);
    if (!raw) return NULL;
    char *aligned = (char *)(((uint64_t)raw + 15) & ~15ULL);
    memset(aligned, 0, 512);
    *(uint16_t *)(aligned + 0)  = 0x037F;
    *(uint16_t *)(aligned + 24) = 0x1F80;
    return raw;  // raw ptr — caller aligns before FXSAVE/RSTOR
}

// ── spawn_user_task(path) ──────────────────────────────────
// Loads an ELF from the filesystem, creates a new user task,
// and adds it to the scheduler. Returns the new task's PID or -1 on error.
int64_t spawn_user_task(const char *path, const char *const *argv)
{
    // 1. Open the ELF file via VFS
    vfs_node_t *node = vfs_lookup(path);
    if (!node) {
        debug_task("spawn: cannot open '%s'\n", path);
        return -1;
    }
    if (node->type != VFS_FILE) {
        debug_task("spawn: '%s' is not a file\n", path);
        vfs_node_put(node);
        return -1;
    }

    // 2. Quick ELF validation
    if (elf_validate(node) != 0) {
        debug_task("spawn: '%s' is not a valid ELF\n", path);
        vfs_node_put(node);
        return -1;
    }

    // 3. Allocate task structures (pattern mirrors old user_task_create)
    void *raw_alloc = malloc(sizeof(union task_union) + STACK_SIZE);
    task_t *tsk = (task_t *)(((uint64_t)raw_alloc + STACK_SIZE - 1) & ~(STACK_SIZE - 1));
    thread_t *thd = (thread_t *)calloc(1, sizeof(thread_t));
    mm_t *mm = (mm_t *)calloc(1, sizeof(mm_t));
    if (mm) {
        list_init(&mm->vma_list);
        mm->mmap_base = 0x40000000;
    }
    if (!raw_alloc || !thd || !mm) {
        if (raw_alloc) kfree(raw_alloc);
        if (thd) kfree(thd);
        if (mm) kfree(mm);
        vfs_node_put(node);
        return -1;
    }

    memset(tsk, 0, sizeof(task_t));
    tsk->stack_alloc_base = raw_alloc;

    tsk->state = TASK_UNINTERRUPTIBLE;
    tsk->flags = 0;                        // NOT PF_KTHREAD → user task
    tsk->addr_limit = 0x00007FFFFFFFFFFF;
    tsk->pid = atomic_fetch_add((volatile uint64_t *)&pid_counter, 1);
    tsk->blocked = 0;                        // child starts with empty blocked mask
    tsk->counter = 1;
    tsk->signal = 0;
    tsk->blocked = 0;                        // user tasks start with empty blocked mask
    tsk->priority = 5;                     // 50 ms quantum at 100 Hz
    tsk->cpu = sched_pick_cpu();          // place on least-loaded CPU

    // Inherit fd table from parent (the init task)
    tsk->parent = current;
    list_init(&tsk->wait_list);
    list_init(&tsk->io_wait_node);
    tsk->exit_code = 0;
    if (current->files)
        tsk->files = files_dup(current->files);

    list_init(&tsk->list);
    {
        uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
        list_add_to_before(&init_task_union.task.list, &tsk->list);
        spin_unlock_irqrestore(&task_list_lock, tl_flags);
    }
    tsk->thread = thd;

    // FPU save area — user tasks may use float/SSE
    tsk->fpu_save = fpu_area_alloc();

    // 4. Create per-process page table
    uint64_t *user_pml4 = (uint64_t *)vmm_alloc_map();  // 4KB zeroed PML4
    if (!user_pml4) {
        kfree(raw_alloc); kfree(thd); kfree(mm);
        vfs_node_put(node);
        return -1;
    }
    uint64_t *kernel_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)init_mm.pml4);
    memcpy(&user_pml4[256], &kernel_pml4[256], 256 * sizeof(uint64_t));

    mm->pml4 = (uint64_t *)Virt_To_Phy((uint64_t)user_pml4);
    tsk->mm = mm;
    thd->cr3 = (uint64_t)mm->pml4;

    // 5. Load ELF segments into the new address space
    uint64_t entry_point;
    if (elf_load(node, mm, &entry_point) != 0) {
        debug_task("spawn: ELF load failed for '%s'\n", path);
        vmm_free_user_map(user_pml4);
        kfree(mm);
        kfree(thd);
        kfree(raw_alloc);
        vfs_node_put(node);
        return -1;
    }
    vfs_node_put(node);

    // Set heap just after the loaded ELF segments
    mm->start_brk = PAGE_4K_ALIGN(mm->end_code);
    mm->end_brk   = mm->start_brk;

    // 6. Map the user stack page (separate 2MB page at 0x600000)
    struct Page *stack_page = alloc_pages(ZONE_NORMAL, 1, 0);
    if (!stack_page) {
        vmm_free_user_map(user_pml4);
        kfree(mm); kfree(thd); kfree(raw_alloc);
        return -1;
    }
    vmm_map_page(user_pml4, stack_page->phy_address,
                 USER_STACK_BASE, PAGE_USER_Page | PAGE_XD);
    mm->start_stack = USER_STACK_BASE;

    // ââ 6.5 Set up argv on user stack âââââââââââââââ
    int s_argc = 0;
    uint64_t user_rsp = USER_STACK_TOP;
    uint64_t user_arg_ptr = 0;

    if (argv != NULL) {
        while (argv[s_argc] != NULL) s_argc++;
        char *kstack = (char *)Phy_To_Virt(stack_page->phy_address);
#define KSTACK(va) (kstack + ((va) - USER_STACK_BASE))
        uint64_t str_offset[128];
        for (int i = 0; i < s_argc; i++) {
            size_t len = strlen(argv[i]) + 1;
            user_rsp -= len;
            memcpy(KSTACK(user_rsp), argv[i], len);
            str_offset[i] = user_rsp;
        }
        user_rsp &= ~15ULL;

        // ── Calculate aligned metadata size ───────────────────
        // Layout from bottom (RSP) up: argc | argv[]+NULL |
        // envp_NULL | auxv AT_NULL.  Total must be 16-byte
        // aligned so RSP (pointing to argc) & 0xF == 0.
        // Without padding, total = 24 + (s_argc+1)*8 bytes.
        // When s_argc is even, we need 8 extra bytes.
        int meta_pad = (s_argc & 1) ? 0 : 8;

        // ── auxv: AT_NULL terminator ──────────────────────────
        user_rsp -= 16;
        *(uint64_t *)KSTACK(user_rsp) = 0;      // AT_NULL type
        *(uint64_t *)KSTACK(user_rsp + 8) = 0;  // value

        // ── envp end NULL + optional alignment padding ────────
        user_rsp -= 8 + meta_pad;
        *(uint64_t *)KSTACK(user_rsp) = 0;      // NULL (end of envp)

        // ── argv[] array (NULL-terminated) ────────────────────
        user_rsp -= (s_argc + 1) * 8;
        user_arg_ptr = user_rsp;
        for (int i = 0; i < s_argc; i++)
            *(uint64_t *)KSTACK(user_rsp + i * 8) = str_offset[i];
        *(uint64_t *)KSTACK(user_rsp + s_argc * 8) = 0;  // NULL terminator

        // ── argc ──────────────────────────────────────────────
        user_rsp -= 8;
        *(uint64_t *)KSTACK(user_rsp) = (uint64_t)s_argc;
#undef KSTACK
    }

    // 7. Set up pt_regs for iretq to ring 3
    pt_regs_t *regs = (pt_regs_t *)((uint64_t)tsk + STACK_SIZE - sizeof(pt_regs_t));
    memset(regs, 0, sizeof(pt_regs_t));
    regs->cs      = USER_CS;
    regs->ss      = USER_DS;
    regs->ds      = USER_DS;
    regs->es      = USER_DS;
    regs->rsp     = (argv != NULL) ? user_rsp : USER_STACK_TOP;
    regs->rip     = entry_point;
    regs->rflags  = (1 << 9);              // IF=1
    regs->rdi     = (uint64_t)s_argc;
    regs->rsi     = user_arg_ptr;
    regs->rdx     = 0;                     // envp = NULL

    // 8. Thread context for switch_to / __switch_to
    thd->rsp0 = (uint64_t)tsk + STACK_SIZE;
    thd->rsp  = (uint64_t)tsk + STACK_SIZE - sizeof(pt_regs_t);
    thd->fs   = KERNEL_DS;
    thd->gs   = KERNEL_DS;
    thd->rip  = (uint64_t)ret_from_intr;   // first entry via RESTORE_ALL → iretq

    tsk->state = TASK_RUNNING;
    enqueue_task(tsk, &percpu_data[tsk->cpu]);

    // The first user task we create is "init" — track it globally.
    if (!user_init_task) {
        user_init_task = tsk;
        user_init_pid  = tsk->pid;
    }

    debug_task("spawn: pid=%d '%s' entry=%p rsp=%p cr3=%p\n",
                  tsk->pid, path, entry_point, regs->rsp, thd->cr3);

    return tsk->pid;
}

// ── sys_exec(path, regs, argv, envp) ────────────────────────
// Replaces the current process image with a new ELF loaded from
// the filesystem. Called from do_system_call (SYS_exec).
// regs is the pt_regs frame on the kernel stack that will be
// restored by RESTORE_ALL → iretq.
//
// If argv == NULL: old behavior (no args, rsp=USER_STACK_TOP,
// argc=0, argv=NULL, envp=NULL).
// If argv != NULL: copies argv/envp strings onto the user stack
// and sets up the standard ABI stack layout so the child's
// _start receives argc in %rdi, argv in %rsi, envp in %rdx.
int64_t sys_exec(const char *path, pt_regs_t *regs,
                 const char *const *argv, const char *const *envp)
{
    debug_task("sys_exec: pid=%d path=%s argv=%p\n", current->pid, path ? path : "(null)", (void*)argv);
    // 1. Look up the ELF file (support relative paths)
    const char *cwd = current->files ? current->files->cwd : "/";
    vfs_node_t *node = vfs_lookup_from(path, cwd);
    if (!node)
        return -ENOENT;
    if (node->type != VFS_FILE) {
        vfs_node_put(node);
        return -EACCES;
    }

    // 2. Quick ELF validation (elf_load does full validation)
    if (elf_validate(node) != 0) {
        vfs_node_put(node);
        return -ENOEXEC;
    }

    // 3. Create a fresh page table for the new process image
    uint64_t *new_pml4 = (uint64_t *)vmm_alloc_map();
    if (!new_pml4) {
        vfs_node_put(node);
        return -ENOMEM;
    }
    uint64_t *kernel_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)init_mm.pml4);
    memcpy(&new_pml4[256], &kernel_pml4[256], 256 * sizeof(uint64_t));

    // 4. Create new mm_struct
    mm_t *new_mm = (mm_t *)calloc(1, sizeof(mm_t));
    if (new_mm) {
        list_init(&new_mm->vma_list);
        new_mm->mmap_base = 0x40000000;
    }
    if (!new_mm) {
        kfree(new_pml4);
        vfs_node_put(node);
        return -ENOMEM;
    }
    new_mm->pml4 = (uint64_t *)Virt_To_Phy((uint64_t)new_pml4);
    // 5. Load ELF segments into the new address space
    uint64_t entry_point;
    if (elf_load(node, new_mm, &entry_point) != 0) {
        vmm_free_user_map(new_pml4);
        kfree(new_mm);
        vfs_node_put(node);
        return -ENOEXEC;
    }
    vfs_node_put(node);

    // Set heap just after the loaded ELF segments
    new_mm->start_brk = PAGE_4K_ALIGN(new_mm->end_code);
    new_mm->end_brk   = new_mm->start_brk;

    // 6. Map the user stack page
    struct Page *stack_page = alloc_pages(ZONE_NORMAL, 1, 0);
    if (!stack_page) {
        vmm_free_user_map(new_pml4);
        kfree(new_mm);
        return -ENOMEM;
    }
    vmm_map_page(new_pml4, stack_page->phy_address,
                 USER_STACK_BASE, PAGE_USER_Page | PAGE_XD);
    new_mm->start_stack = USER_STACK_BASE;

    // ── 6.5 Set up argv/envp on user stack ──────────────────
    int s_argc = 0;
    int s_envc = 0;
    uint64_t user_rsp = USER_STACK_TOP;
    uint64_t user_arg_ptr = 0;   // rsi value
    uint64_t user_env_ptr = 0;   // rdx value

    if (argv != NULL) {
        // Count argv
        while (argv[s_argc] != NULL) s_argc++;

        // Count envp
        if (envp != NULL) {
            while (envp[s_envc] != NULL) s_envc++;
        }

        // Access the stack page through kernel mapping
        char *kstack = (char *)Phy_To_Virt(stack_page->phy_address);
        // kstack[0..0x1FFFFF] maps to USER_STACK_BASE..USER_STACK_TOP
        #define KSTACK(va) (kstack + ((va) - USER_STACK_BASE))

        // ── Copy string data to top of stack ──────────────────
        // str_offsets[i] records the virtual address of each string on stack
        uint64_t str_offset[128];  // enough for ~32 argv + envp each
        int si = 0;

        // Copy argv strings
        for (int i = 0; i < s_argc; i++) {
            size_t len = strlen(argv[i]) + 1;
            user_rsp -= len;
            memcpy(KSTACK(user_rsp), argv[i], len);
            str_offset[si++] = user_rsp;
        }

        // Copy envp strings
        for (int i = 0; i < s_envc; i++) {
            size_t len = strlen(envp[i]) + 1;
            user_rsp -= len;
            memcpy(KSTACK(user_rsp), envp[i], len);
            str_offset[si++] = user_rsp;
        }

        // ── 16-byte align ─────────────────────────────────────
        user_rsp &= ~15ULL;

        // ── Calculate aligned metadata size ───────────────────
        // Layout from bottom (RSP) up: argc | argv[]+NULL |
        // envp[]+NULL | auxv AT_NULL.  Total must be 16-byte
        // aligned so RSP (pointing to argc) & 0xF == 0.
        // Without padding, total = 24 + (argc+1)*8 + (envc+1)*8.
        // When (s_argc + s_envc) is even, we need 8 extra bytes.
        int meta_pad = ((s_argc + s_envc) & 1) ? 0 : 8;

        // ── auxv: just AT_NULL terminator ─────────────────────
        user_rsp -= 16;  // {AT_NULL=0, 0}
        *(uint64_t *)KSTACK(user_rsp) = 0;      // AT_NULL
        *(uint64_t *)KSTACK(user_rsp + 8) = 0;

        // ── envp[] array (NULL-terminated) + alignment pad ────
        user_rsp -= (s_envc + 1) * 8 + meta_pad;
        user_env_ptr = user_rsp;
        for (int i = 0; i < s_envc; i++) {
            *(uint64_t *)KSTACK(user_rsp + i * 8) = str_offset[s_argc + i];
        }
        *(uint64_t *)KSTACK(user_rsp + s_envc * 8) = 0;  // NULL terminator

        // ── argv[] array (NULL-terminated) ────────────────────
        user_rsp -= (s_argc + 1) * 8;
        user_arg_ptr = user_rsp;
        for (int i = 0; i < s_argc; i++) {
            *(uint64_t *)KSTACK(user_rsp + i * 8) = str_offset[i];
        }
        *(uint64_t *)KSTACK(user_rsp + s_argc * 8) = 0;  // NULL terminator

        // ── argc ──────────────────────────────────────────────
        user_rsp -= 8;
        *(uint64_t *)KSTACK(user_rsp) = (uint64_t)s_argc;

        #undef KSTACK
    }

    // 7. Free the OLD user address space (both VMA pages and page tables).
    // fork_mm_copy creates fully independent page table hierarchies
    // (vmm_alloc_map + calloc per level), so vmm_free_user_map on the
    // child's PML4 is safe — it won't corrupt the parent's address space.
    if (current->mm) {
        mm_t *old_mm = current->mm;
        uint64_t *old_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)old_mm->pml4);

        vma_free_all(old_mm);            // free VMA-tracked 4KB pages + VMA nodes
        vmm_free_user_map(old_pml4);     // free page tables + remaining 2MB pages

        kfree(old_mm);
        current->mm = NULL;
    }

    // 7.5 POSIX: exec() resets caught signal handlers to SIG_DFL.
    // SIG_IGN is supposed to survive exec, but shells (ash) set
    // SIGINT to SIG_IGN to protect themselves from Ctrl-C.  A
    // forked child inherits SIG_IGN, and with POSIX-correct exec
    // SIG_IGN survives — leaving the new process immune to Ctrl-C.
    //
    // Reset ALL handlers to SIG_DFL unconditionally.  This is what
    // Linux does for several signals (SIGCHLD is always reset on
    // exec, even if SIG_IGN), and it's the pragmatic fix for the
    // shell-child-signal-inheritance problem.
    for (int sig = 1; sig < NSIG; sig++)
        current->sighand[sig].sa_handler = SIG_DFL;

    // 8. Install new mm and page table
    current->mm = new_mm;
    current->thread->cr3 = (uint64_t)new_mm->pml4;

    // 9. Switch CR3 to the new page table
    __asm__ __volatile__("movq %0, %%cr3" :: "r"(current->thread->cr3) : "memory");

    // 10. Overwrite pt_regs for RESTORE_ALL → iretq to the new process
    regs->cs      = USER_CS;
    regs->ss      = USER_DS;
    regs->ds      = USER_DS;
    regs->es      = USER_DS;
    regs->rsp     = (argv != NULL) ? user_rsp : USER_STACK_TOP;
    regs->rip     = entry_point;
    regs->rflags  = (1 << 9);              // IF=1
    regs->rdi     = (uint64_t)s_argc;      // argc
    regs->rsi     = user_arg_ptr;          // argv
    regs->rdx     = user_env_ptr;          // envp

    debug_task("exec: pid=%d entry=%p rsp=%p argc=%d cr3=%p\n",
                  current->pid, entry_point, regs->rsp, s_argc, current->thread->cr3);

    return 0;
}

// ── fork_mm_copy — create private address space for fork child ─
// Builds a new PML4 with private copies of all user 2MB pages.
// Uses inline rep movsb instead of memcpy because libk's memcpy
// has a bug with 2MB copies (CR2=0x8).
static mm_t *fork_mm_copy(mm_t *parent_mm, uint64_t *cr3_out)
{
    mm_t *child_mm = (mm_t *)calloc(1, sizeof(mm_t));
    uint64_t *child_pml4 = (uint64_t *)vmm_alloc_map();
    if (!child_mm || !child_pml4)
        goto fail;

    uint64_t *parent_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)parent_mm->pml4);
    uint64_t *kernel_pml4 = (uint64_t *)Phy_To_Virt((uint64_t)init_mm.pml4);

    memcpy(&child_pml4[256], &kernel_pml4[256], 256 * sizeof(uint64_t));

    for (int l4 = 0; l4 < 256; l4++) {
        uint64_t pml4e = parent_pml4[l4];
        if (!(pml4e & PAGE_Present)) continue;

        uint64_t *parent_pml3 = (uint64_t *)Phy_To_Virt(pml4e & PAGE_4K_MASK);
        uint64_t *child_pml3  = (uint64_t *)calloc(1, PAGE_4K_SIZE);
        if (!child_pml3) continue;
        child_pml4[l4] = Virt_To_Phy((uint64_t)child_pml3) | PAGE_USER_GDT;

        for (int l3 = 0; l3 < 512; l3++) {
            uint64_t pml3e = parent_pml3[l3];
            if (!(pml3e & PAGE_Present)) continue;

            uint64_t *parent_pml2 = (uint64_t *)Phy_To_Virt(pml3e & PAGE_4K_MASK);
            uint64_t *child_pml2  = (uint64_t *)calloc(1, PAGE_4K_SIZE);
            if (!child_pml2) continue;
            child_pml3[l3] = Virt_To_Phy((uint64_t)child_pml2) | PAGE_USER_Dir;

            for (int l2 = 0; l2 < 512; l2++) {
                uint64_t pml2e = parent_pml2[l2];
                if (!(pml2e & PAGE_Present)) continue;

                // Eager copy: allocate a private 2MB page and copy
                // using rep movsb.  Inline asm is used instead of
                // memcpy because the kernel's libk memcpy has a bug
                // with 2MB copies (CR2=0x8).
                // Only 2MB huge pages (PAGE_PS) are eagerly copied.
                // Non-2MB entries (4KB page table pointers, etc.) are
                // shared -- the child inherits the parent's mapping.
                // 4KB PTE table: share pages via COW.
                // Check PAGE_COW before PAGE_R_W — a COW page has R/W=0
                // and must not be misclassified as plain read-only.
                if (!(pml2e & PAGE_PS)) {
                    if (!(pml2e & PAGE_Present)) {
                        child_pml2[l2] = 0;
                        continue;
                    }
                    uint64_t *parent_pte =
                        (uint64_t *)Phy_To_Virt(pml2e & PAGE_4K_MASK);
                    uint64_t *child_pte =
                        (uint64_t *)calloc(1, PAGE_4K_SIZE);
                    if (!child_pte) {
                        child_pml2[l2] = pml2e;  // OOM: share PDE
                        continue;
                    }
                    child_pml2[l2] = Virt_To_Phy((uint64_t)child_pte)
                                   | (pml2e & 0xfff);
                    for (int l1 = 0; l1 < 512; l1++) {
                        uint64_t pte = parent_pte[l1];
                        if (!(pte & (PAGE_Present | PAGE_PROTNONE)))
                            continue;

                        // Compute VA from page table indices
                        uint64_t vaddr = ((uint64_t)l4 << 39)
                                       | ((uint64_t)l3 << 30)
                                       | ((uint64_t)l2 << 21)
                                       | ((uint64_t)l1 << 12);
                        vma_t *vma = vma_find(parent_mm, vaddr);
                        if (vma && (vma->vm_flags & VM_IO)) {
                            child_pte[l1] = pte;  // share MMIO PTE, no COW
                            continue;
                        }

                        if (pte & PAGE_COW) {
                            // Already COW-shared (fork-of-fork)
                            page_cow_get(pte & PAGE_4K_MASK);
                            child_pte[l1] = pte;
                        } else if (pte & PAGE_R_W) {
                            // Path A: writable -> COW on BOTH parent and child.
                            // page_cow_get TWICE: parent PTE (R/W->R/O+COW) +1,
                            // child PTE (new COW) +1 -> cow_count grows by 2.
                            parent_pte[l1] &= ~PAGE_R_W;
                            parent_pte[l1] |= PAGE_COW;
                            page_cow_get(pte & PAGE_4K_MASK);
                            page_cow_get(pte & PAGE_4K_MASK);
                            child_pte[l1] = parent_pte[l1];
                        } else {
                            // Path B: plain read-only -> share directly
                            child_pte[l1] = pte;
                        }
                    }
                    continue;
                }
                uint64_t phys = pml2e & PAGE_2M_MASK;
                // VM_IO guard: skip MMIO huge pages, share directly
                {
                    uint64_t vaddr_2m = ((uint64_t)l4 << 39)
                                       | ((uint64_t)l3 << 30)
                                       | ((uint64_t)l2 << 21);
                    vma_t *vm = vma_find(parent_mm, vaddr_2m);
                    if (vm && (vm->vm_flags & VM_IO)) {
                        child_pml2[l2] = pml2e;
                        continue;
                    }
                }
                struct Page *s = alloc_pages(ZONE_NORMAL, 1, 0);
                if (s) {
                    uint64_t dst = (uint64_t)Phy_To_Virt(s->phy_address);
                    uint64_t src = (uint64_t)Phy_To_Virt(phys & ~PAGE_XD);
                    uint64_t sz  = PAGE_2M_SIZE;
                    __asm__ __volatile__(
                        "cld\n\t"
                        "rep movsb\n\t"
                        : "+S"(src), "+D"(dst), "+c"(sz)
                        :
                        : "memory"
                    );
                    child_pml2[l2] = s->phy_address
                                   | (pml2e & ~PAGE_2M_MASK);
                } else {
                    child_pml2[l2] = pml2e; // OOM fallback: share
                }
            }
        }
    }

    memcpy(child_mm, parent_mm, sizeof(mm_t));
    // vma_list must NOT be shared — fork_vma_copy will fill child's own
    list_init(&child_mm->vma_list);
    child_mm->pml4 = (uint64_t *)Virt_To_Phy((uint64_t)child_pml4);
    *cr3_out = (uint64_t)child_mm->pml4;

    // TLB shootdown: parent's in-memory PTEs were modified (R/W → R/O+COW).
    // With SMP load balancing the parent may run on any CPU — must
    // invalidate ALL cores' TLBs, not just the local one.
    tlb_shootdown();

    return child_mm;

fail:
    if (child_pml4) {
        for (int l4 = 0; l4 < 256; l4++) {
            uint64_t pml4e = child_pml4[l4];
            if (!(pml4e & PAGE_Present)) continue;
            uint64_t *pml3 = (uint64_t *)Phy_To_Virt(pml4e & PAGE_4K_MASK);
            for (int l3 = 0; l3 < 512; l3++) {
                uint64_t pml3e = pml3[l3];
                if (!(pml3e & PAGE_Present)) continue;
                uint64_t *pml2 = (uint64_t *)Phy_To_Virt(pml3e & PAGE_4K_MASK);
                for (int l2 = 0; l2 < 512; l2++) {
                    uint64_t pml2e = pml2[l2];
                    if (!(pml2e & PAGE_Present)) continue;
                    if (!(pml2e & PAGE_PS)) {
                        uint64_t *pte = (uint64_t *)Phy_To_Virt(pml2e & PAGE_4K_MASK);
                        kfree(pte);
                    }
                }
                kfree(pml2);
            }
            kfree(pml3);
        }
        kfree(child_pml4);
    }
    if (child_mm)   kfree(child_mm);
    if (cr3_out)    *cr3_out = 0;
    return NULL;
}

// ── do_fork ──────────────────────────────────────────────
uint64_t do_fork(pt_regs_t *regs, uint64_t clone_flags,
                 uint64_t stack_start __attribute__((unused)),
                 uint64_t stack_size __attribute__((unused)))
{
    void *raw_alloc = malloc(sizeof(union task_union) + STACK_SIZE);
    task_t *tsk = (task_t *)(((uint64_t)raw_alloc + STACK_SIZE - 1) & ~(STACK_SIZE - 1));
    thread_t *thd = (thread_t *)malloc(sizeof(thread_t));

    if (!raw_alloc || !thd) {
        if (raw_alloc) kfree(raw_alloc);
        if (thd) kfree(thd);
        return -ENOMEM;
    }

    memset(tsk, 0, sizeof(task_t));
    memset(thd, 0, sizeof(thread_t));

    // ── Selective field initialization (NOT *tsk = *current) ──
    // The shallow struct copy was the root cause of the CR2=0x8
    // page fault: it copied stale list nodes (wait_list, io_wait_node),
    // the parent's stack_alloc_base, and the parent's thread pointer
    // — all of which must belong to the CHILD, not the parent.
    tsk->stack_alloc_base = raw_alloc;

    // Inherit from parent (safe value-copy fields, not pointers)
    tsk->flags       = current->flags;
    tsk->addr_limit  = current->addr_limit;

    // EEVDF: fair starting vruntime — pick target CPU first, then use its min_vruntime
    uint32_t target_cpu = sched_pick_cpu();
    {
        uint64_t fair_start = percpu_data[target_cpu].min_vruntime;
        tsk->vruntime = current->vruntime < fair_start ? current->vruntime : fair_start;
    }

    // Inherit signal handlers (shallow copy of sighand array — values, not pointers)
    for (int sig = 0; sig < NSIG; sig++)
        tsk->sighand[sig] = current->sighand[sig];

    // ── Fresh fields (must NOT inherit from parent) ────────
    tsk->signal      = 0;   // child must not inherit parent's pending signals
    tsk->blocked     = current->blocked;  // do_fork inherits parent's blocked mask
    tsk->state       = TASK_UNINTERRUPTIBLE;
    tsk->priority    = 3;
    tsk->cpu         = target_cpu;
    tsk->pid         = atomic_fetch_add((volatile uint64_t *)&pid_counter, 1);
    tsk->thread      = thd;
    tsk->parent      = current;
    tsk->exit_code   = 0;

    // Inherit controlling terminal from parent
    tsk->ctty_type = current->ctty_type;
    tsk->ctty      = current->ctty;

    list_init(&tsk->list);
    list_init(&tsk->wait_list);
    list_init(&tsk->io_wait_node);
    {
        uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
        list_add_to_before(&init_task_union.task.list, &tsk->list);
        spin_unlock_irqrestore(&task_list_lock, tl_flags);
    }

    // Blocker starts clean — child inherits no blocker state
    tsk->blocker.type = BLOCKER_NONE;
    tsk->blocker.check = NULL;
    tsk->blocker.signal_can_wake = false;
    memset(&tsk->blocker_data, 0, sizeof(tsk->blocker_data));

    // FPU: child gets a fresh FPU save area.  The parent's FPU
    // registers are in hardware (not in the fpu_save buffer), so
    // copying the area would give stale data.  A fresh area is
    // correct for the common case (child execs immediately).
    tsk->fpu_save = fpu_area_alloc();

    // Inherit address space (shared initially, replaced for user tasks)
    tsk->mm = current->mm;

    // ── Inherit fd table (deep copy — refcount++) ──────────
    if (current->files)
        tsk->files = files_dup(current->files);

    thd->cr3 = current->thread->cr3;

    if ((regs->cs & 3) == 3) {
        tsk->flags &= ~PF_KTHREAD;
        tsk->addr_limit = 0x00007FFFFFFFFFFF;
        if (current->mm && current->mm->pml4) {
            tsk->mm = fork_mm_copy(current->mm, &thd->cr3);
            if (!tsk->mm) {
                debug_task("fork: pid=%d fork_mm_copy FAILED, falling back to shared mm\n",
                    (int)current->pid);
                tsk->mm = current->mm;
                thd->cr3 = current->thread->cr3;
            }
            if (tsk->mm)
                fork_vma_copy(tsk->mm, current->mm);
        } else if (current->mm) {
            debug_task("fork: pid=%d parent_mm->pml4 is NULL, sharing mm\n",
                (int)current->pid);
        }
    }

    memcpy((void *)((uint64_t)tsk + STACK_SIZE - sizeof(pt_regs_t)),
           regs, sizeof(pt_regs_t));

    // Child process sees fork() return 0
    {
        pt_regs_t *child_regs = (pt_regs_t *)((uint64_t)tsk + STACK_SIZE - sizeof(pt_regs_t));
        child_regs->rax = 0;
    }

    thd->rsp0 = (uint64_t)tsk + STACK_SIZE;
    thd->rsp  = (uint64_t)tsk + STACK_SIZE - sizeof(pt_regs_t);
    thd->fs   = KERNEL_DS;
    thd->gs   = KERNEL_DS;

    thd->rip = regs->rip;
    if (!(tsk->flags & PF_KTHREAD))
        thd->rip = (uint64_t)ret_from_intr;  // child via softirq/preemption check
    // Parent: do NOT change regs->rip — returns via error_code → RESTORE_ALL

    tsk->state = TASK_RUNNING;
    {
        uint64_t flags = spin_lock_irqsave(&percpu_data[tsk->cpu].rq_lock);
        enqueue_task(tsk, &percpu_data[tsk->cpu]);
        spin_unlock_irqrestore(&percpu_data[tsk->cpu].rq_lock, flags);
    }
    sched_notify_remote(tsk);
    return tsk->pid;   // parent sees child PID
}

int kernel_thread(uint64_t (*fn)(uint64_t), uint64_t arg, uint64_t flags)
{
    pt_regs_t regs;
    memset(&regs, 0, sizeof(pt_regs_t));

    regs.rbx = (uint64_t)fn;
    regs.rdx = (uint64_t)arg;

    regs.ds = KERNEL_DS;
    regs.es = KERNEL_DS;
    regs.cs = KERNEL_CS;
    regs.ss = KERNEL_DS;
    regs.rflags = (1 << 9);
    regs.rip = (uint64_t)kernel_thread_func;

    return do_fork(&regs, flags, 0, 0);
}

struct task_struct *create_kthread(uint64_t (*fn)(uint64_t), uint64_t arg,
                                   const char *name)
{
    (void)name;
    int64_t pid = kernel_thread(fn, arg, PF_KTHREAD);
    if (pid < 0)
        return NULL;

    list_t *pos = init_task_union.task.list.next;
    while (pos != &init_task_union.task.list) {
        task_t *t = container_of(pos, task_t, list);
        if (t->pid == pid)
            return t;
        pos = task_list_next(pos);
    }
    return NULL;
}

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

void task_init()
{

    arch_task_init_platform();

    init_mm.start_code = PMMngr.start_code;
    init_mm.end_code = PMMngr.end_code;
    init_mm.start_data = (uint64_t)&_data;
    init_mm.end_data = PMMngr.end_data;
    init_mm.start_rodata = (uint64_t)&_rodata;
    init_mm.end_rodata = (uint64_t)&_erodata;
    init_mm.start_brk = 0;
    init_mm.end_brk = PMMngr.start_brk;
    init_mm.start_stack = _stack_start;

    list_init(&init_mm.vma_list);
    init_mm.mmap_base = 0;

    list_init(&init_task_union.task.list);

    // BSP idle task pointer (for the multicore scheduler).
    percpu_data[0].idle = &init_task_union.task;
    init_task_union.task.cpu = 0;

    // ── Set up fd 0/1/2 on the idle task ─────────────────
    // These will be inherited by the first user task (init.elf).
    {
        files_t *files = files_alloc();
        if (files) {
            current->files = files;  // attach to idle task

            // All three fds go through /dev/tty:
            //   read  → keyboard (ASCII-translated scancodes)
            //   write → framebuffer (GTK window) + serial (terminal)
            vfs_node_t *tty = vfs_lookup("/dev/tty");
            if (tty) {
                file_t *f0 = file_alloc();
                if (f0) { f0->type = FD_DEV; f0->node = tty; f0->flags = O_RDWR;  fd_alloc(files, f0); }

                file_t *f1 = file_alloc();
                if (f1) { f1->type = FD_DEV; f1->node = tty; f1->flags = O_WRONLY; fd_alloc(files, f1); }

                file_t *f2 = file_alloc();
                if (f2) { f2->type = FD_DEV; f2->node = tty; f2->flags = O_WRONLY; fd_alloc(files, f2); }
            }
        }
    }

    // pid_counter starts at 1 → first user task becomes PID=1.
    int64_t init_pid = spawn_user_task("/bin/init", NULL);
    debug_task("init: spawned user-space init, pid=%d\n", (int)init_pid);

    // Spawn the deferred-free reaper kthread BEFORE activating the
    // scheduler.  This guarantees the reaper exists before any zombie
    // can be produced (schedule() returns early while scheduler_ok==0).
    {
        task_t *df = deferred_free_spawn();
    }

    // Activate the scheduler and enter the idle loop.
    // schedule() picks up the user init (PID 1) naturally.
    current->state = TASK_RUNNING;
    this_cpu()->scheduler_ok = 1;

#ifdef OS01_SELFTEST
    // ── Kernel mutex selftest ────────────────────────────────
    // Runs before the idle loop so we can use kernel_thread +
    // schedule().  Two kernel threads increment a shared counter
    // under mutex_lock 1000 times each, verifying mutual exclusion.
    {
        extern void test_kernel_mutex(void);
        test_kernel_mutex();
    }
#endif

#ifdef OS01_SELFTEST
    // ── Deferred-free selftest ───────────────────────────────
    // Must run after scheduler_ok=1 so schedule() works and
    // the reaper kthread can drain work items.
    {
        extern void test_deferred_free(void);
        test_deferred_free();
    }
#endif

    // ── Idle loop ────────────────────────────────────────────
    // hlt pauses the CPU until the next interrupt (timer tick,
    // keyboard IRQ1, serial IRQ4).  The timer ISR sets
    // need_resched; ret_from_intr calls schedule() before iretq.
    // If a task was woken we switch to it; otherwise we loop
    // back to hlt.
    //
    // serial_poll() (IRQ fallback) moved to pit_handler — runs
    // at 100 Hz on every timer tick, so the serial input path
    // still has a fallback even if IOAPIC routing fails.
    while (1) {
        __asm__ __volatile__("hlt");
        if (this_cpu()->need_resched)
            schedule();
    }
}
