#include <sched/task.h>
#include <sched/internal.h>
#include <sched/diag.h>
#include <percpu/percpu.h>
#include <kernel.h>
#include <arch/spinlock.h>
#include <arch/irq.h>
#include <arch/cpu.h>
#include <core/debug.h>
#include <core/panic.h>
#include <log/log.h>
#include <memory/slab.h>
#include <string.h>
#include <errno.h>
#include <core/assert.h>

// ── Preemption flag ──────────────────────────────────────
// Now per-CPU (percpu_t.need_resched, offset 8 from GS base).
// Set by timer IRQ on every tick, cleared by schedule() after
// a context switch.  entry.S reads it via %gs:8.

// .lock = { .lock = 1L }: mm_t.lock is a spinlock_T whose own field is
// `lock`.  1 = unlocked; leaving it 0 would deadlock the first task that
// takes it (INIT_TASK points .mm at this struct).
mm_t init_mm = { .lock = { .lock = 1L } };

union task_union init_task_union __attribute__((__section__(".data.init_task"))) = {INIT_TASK(init_task_union.task)};

task_t *init_task[NR_CPUS] = {&init_task_union.task, 0};

// init_thread (the idle task's saved context) is architecture-shaped:
// its definition lives in each architecture's task_arch.c (x86:
// kernel/arch/x86_64/cpu/task_arch.c; aarch64: Phase 3), so a common TU
// never names rsp/rip/fs/cr3.  The x86 system startup that used to live
// in task_init() below moved to kernel/arch/x86_64/cpu/sched_boot.c;
// the common idle publication / scheduler start it calls live in
// kernel/sched/boot.c.

// ── Global task list lock (SMP) ──────────────────────────
// Protects all traversals and modifications to
// init_task_union.task.list.  schedule() paths use
// spin_trylock_irqsave — if the lock is contended they
// skip one cycle (no deadlock possible).
spinlock_T task_list_lock = { .lock = 1L };

// ── Safe task-list iteration ─────────────────────────────
// Writers hold task_list_lock (see task_list_add() called from
// smp_boot_aps, do_fork, spawn_user_task), so concurrent readers
// in schedule() see consistent pointers.  The NULL guard below
// survives any remaining edge cases (e.g. memory corruption).
list_t *task_list_next(list_t *pos)
{
    list_t *next = pos->next;
    if ((uintptr_t)next < 0x1000) {
        log_err("[sched] list corruption at %p (next=%p) — breaking\n",
                (void *)pos, (void *)next);
        return NULL;
    }
    return next;
}

// ── Thread-safe task-list insertion ─────────────────────
// Called from smp_boot_aps() to add AP idle tasks while
// already-booted APs may be scanning the list in schedule().
void task_list_add(task_t *tsk)
{
    list_init(&tsk->list);
    uint64_t flags = spin_lock_irqsave(&task_list_lock);
    list_add_to_before(&init_task_union.task.list, &tsk->list);
    spin_unlock_irqrestore(&task_list_lock, flags);
}

__attribute__((noreturn)) static void idle_task_resume(void);


/* Called on the incoming stack, after the architecture has stopped using
 * prev. A wake between schedule's dequeue and this point observes on_cpu
 * and leaves enqueueing to us. Use the same lock as task_wake so neither
 * side can miss that handoff. No access to prev is allowed after release:
 * a waiter may immediately reap a zombie once on_cpu becomes zero. */
void task_finish_switch(task_t *prev)
{
    percpu_t *rq = &percpu_data[prev->cpu];
    uint64_t flags = spin_lock_irqsave(&rq->rq_lock);
    if (prev->state == TASK_RUNNING && !prev->on_rq && prev != rq->idle) {
        enqueue_task(prev, rq);
        rq->need_resched = 1;
    }
    __atomic_store_n(&prev->on_cpu, 0, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&rq->rq_lock, flags);
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

retry:
    ;
    /*
     * Read t->cpu locklessly — sched_balance may change it concurrently.
     * We re-check both t->cpu and t->on_rq under the acquired rq_lock
     * to close the race window.
     */
    percpu_t *rq = &percpu_data[*(volatile uint32_t *)&t->cpu];
    uint64_t flags = spin_lock_irqsave(&rq->rq_lock);

    /* Serialize the state transition with the final switch-out check. */
    if (rq != &percpu_data[*(volatile uint32_t *)&t->cpu]) {
        spin_unlock_irqrestore(&rq->rq_lock, flags);
        goto retry;
    }
    t->state = TASK_RUNNING;

    /* Re-check on_rq under lock — sched_balance may have enqueued it.
     * ACQUIRE loads: the picker's RELEASE stores of on_rq/on_cpu are
     * guaranteed visible even when we locked a stale rq (t->cpu may
     * have been read before the picker synced it) — without this the
     * wakeup re-enqueues a task that is already committed to another
     * CPU (double-book -> both CPUs run it -> stack clobber). */
    if (__atomic_load_n(&t->on_rq, __ATOMIC_ACQUIRE)) {
        spin_unlock_irqrestore(&rq->rq_lock, flags);
        return;
    }

    /* [FIX-doublebook] Re-check on_cpu: a task that is RUNNING on a
     * CPU (or committed to run by schedule()'s pick, on_cpu=1 set
     * under the rq_lock) must never be woken/re-enqueued. */
    if (__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) {
        spin_unlock_irqrestore(&rq->rq_lock, flags);
        return;
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

pid_t alloc_pid(void)
{
    return (pid_t)atomic_fetch_add((volatile uint64_t *)&pid_counter, 1);
}

// ── User-space init task pointer ─────────────────────────
// Set by spawn_user_task() the first time it creates a user task.
// do_exit() uses this to reparent orphans and protect the init process.
task_t *user_init_task = NULL;
int64_t  user_init_pid = 0;

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
 * Called from schedule() under task_list_lock — the blocker-wakeup
 * backstop.  (The old zombie reaper is gone; this is NOT a reaper.)
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
        // Mask-aware: a blocked signal must NOT wake an interruptible
        // sleeper (POSIX).  do_waitpid still wakes via the condition
        // path above (child ZOMBIE), so this change doesn't affect it.
        if (t->blocker.signal_can_wake && (t->signal & ~t->blocked)) {
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
    arch_local_irq_enable();
    // Woke up — clear blocker and check why
    self->blocker.type = BLOCKER_NONE;
    self->blocker.check = NULL;
    self->state = TASK_RUNNING;

    // Step 6: Check if woken by signal.
    // Re-check the condition first: if it's now met (e.g. child
    // exited AND SIGCHLD was delivered), return 0 — the signal
    // will be handled on the way back to userspace.
    // Mask-aware: only an UNBLOCKED pending signal interrupts (POSIX).
    if (signal_can_wake && (self->signal & ~self->blocked) && !check(self))
        return -EINTR;

    return 0;
}

void schedule(void)
{
    percpu_t *rq = this_cpu();
    if (!rq || !rq->scheduler_ok)
        return;

    // [FIX] Nested-schedule guard (Linux preempt_count idea):
    // the resume path re-opens IRQs before the epilogue returns, so
    // a tick in that window calls do_resched -> schedule() on top of
    // this invocation.  Nested schedule() re-picks/re-switches and
    // corrupts the outer switch frames (RIP=2, rax=0x202 crash).
    // With the flag set, the nested call returns immediately: the
    // outer call has already chosen next and will finish switching.
    if (current->in_schedule)
        return;
    current->in_schedule = 1;
    // On-CPU: our kernel stack is in use.  A waiter must not free us
    // even after do_exit sets ZOMBIE (final schedule is still
    // running on this stack).
    __atomic_store_n(&current->on_cpu, 1, __ATOMIC_RELEASE);

    // [FIX] IRQs must stay OFF for the whole schedule() body.
    // Otherwise the tick's do_resched path re-enters schedule()
    // on top of this invocation (nested schedule), corrupting
    // run-queue state and interrupt frames — the intermittent #PF.
    // Saved in current->thread->sched_flags, NOT a per-CPU global:
    // the global gets overwritten by the next task calling
    // schedule() on this CPU, so on resume this task would restore
    // another task's flags.  Per-task storage survives any number
    // of intervening schedules by other tasks.
    current->thread->sched_flags = arch_local_irq_save();

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

    // ── 3. Wake blocked tasks whose condition is now met ──
    // (was inside the old zombie-reaper critical section; now
    // standalone — this is the blocker-wakeup backstop, NOT a reaper.)
    {
        uint64_t ub_flags = spin_lock_irqsave(&task_list_lock);
        sched_unblock_blocked();
        spin_unlock_irqrestore(&task_list_lock, ub_flags);
    }

    // ── 3.5 Load balancing ───────────────────────────────
    sched_balance(rq);

    // ── 4. Pick next task (rbtree O(log n)) ─────────────────
    task_t *next;
    {
        uint64_t rq_flags = spin_lock_irqsave(&rq->rq_lock);
        next = pick_eevdf(rq);
        if (next && next != rq->idle) {
            dequeue_task(next, rq);
            // [FIX-doublebook] next is COMMITTED to this CPU: from
            // now until switch_to + resume it is off-rq but about to
            // run.  Without on_cpu=1 here, another CPU's task_wake /
            // blocker_wake / sched_unblock_blocked sees on_rq==false
            // in the window and re-enqueues next onto ITS runqueue —
            // both CPUs then run the same task (stack clobber,
            // garbage rbp, RIP=user-data crash).  on_cpu=1 closes
            // the window: task_wake skips on_cpu tasks.
            // RELEASE: paired with task_wake's ACQUIRE on_cpu load.
            __atomic_store_n(&next->on_cpu, 1, __ATOMIC_RELEASE);
            // [FIX-cpu-sync] keep t->cpu == the runqueue it sits on.
            // schedule() never updated next->cpu at pick time, so a
            // task could run on CPU0 with a stale t->cpu=1.  task_wake
            // locks percpu_data[t->cpu] (CPU1's rq) which does NOT
            // serialize against this CPU0 pick (CPU0's rq) — wakeup in
            // the pick->switch_to window re-enqueues next onto CPU1
            // and both CPUs run it.  Syncing cpu under the same lock
            // makes task_wake's lock == this lock -> on_rq/on_cpu
            // re-checks actually serialize.
            next->cpu = rq->cpu_id;
        }
        spin_unlock_irqrestore(&rq->rq_lock, rq_flags);
    }

    // ── 5. Fallback to idle ─────────────────────────────────
    if (!next || next->state != TASK_RUNNING) {
        if (next)
            log_err("sched: orphan task %d (state=%ld), falling back to idle\n",
                    (int)next->pid, (long)next->state);
        next = rq->idle;
        if (!next) {
            arch_local_irq_restore(current->thread->sched_flags);
            current->in_schedule = 0;
            return;
        }
    }

    // ── 6. Preemption guard: a real switch needs a DIFFERENT task.
    //        Every next == current takes this no-switch IRQ-restore /
    //        guard-clear path, irrespective of vruntime — a self
    //        switch_to would only round-trip our own context, and the
    //        architecture tail must never finish/free the running task
    //        (spec §5.1: the scheduler skips every self-switch). Always
    //        skip idle→idle.
    if (next == current) {
        // [P2-5] Baseline bookkeeping kept: the OLD slice-exhausted self
        // path fell through the (formerly conditional) skip and advanced
        // rq->min_vruntime before switching to itself.  The spec skips
        // the self SWITCH, not the advance, so restore it under exactly
        // the old condition — otherwise a CPU-bound task that always
        // re-picks itself never advances its runqueue's min_vruntime
        // and newly created/woken tasks inherit a stale floor.
        if (next != rq->idle && next->vruntime >= next->deadline &&
            next->vruntime > rq->min_vruntime)
            rq->min_vruntime = next->vruntime;

        rq->need_resched = 0;
        arch_local_irq_restore(current->thread->sched_flags);
        current->in_schedule = 0;
        return;
    }

    // ── 6. Update min_vruntime ──────────────────────────────
    if (next != rq->idle && next->vruntime > rq->min_vruntime)
        rq->min_vruntime = next->vruntime;

    rq->need_resched = 0;

    // Idle task thread->rip is 0 until its first switch_to.  Arm the
    // first continuation through the arch hook (x86 writes the common C
    // idle_task_resume, and only while rip is still 0 — never the
    // different assembly idle_resume in entry.S).  When schedule() runs
    // from the idle loop with an empty rbtree, next==current==idle and
    // the preemption guard already returned; the FIRST real switch-in
    // is what needs the continuation.
    if (next == rq->idle)
        arch_task_prepare_first_switch(next, idle_task_resume);

    // [DIAG-5]/[DIAG-5b]: verify our own previous context before
    // switch_to's asm writes through it.  The two x86 checks (sane
    // kernel-heap thread pointer; saved rsp inside our task_union) are
    // arch-owned — see arch_task_check_prev(); common core no longer
    // reads thread->rip / thread->rsp / thread->rsp0 directly.
    arch_task_check_prev(current);
    switch_to(current, next);
    // Resumed here when this task is switched back: we are running
    // again — stack in use, so on_cpu = 1.
    __atomic_store_n(&current->on_cpu, 1, __ATOMIC_RELEASE);
    // IRQs stay DISABLED (cli from switch_to).  Do NOT popfq here:
    // re-opening IRQs before the epilogue ret creates a tick window
    // where do_resched runs a nested schedule() over the live resume
    // frame -> RIP=2 (rbx==rbp, rsp-saved-0xf0 crash signature).
    // Callers restore IRQ state: iret/sysret to userspace, or an
    // explicit arch_local_irq_enable() in kernel-side loops.
    // Leave schedule() — allow a real (non-nested) call next time.
    current->in_schedule = 0;
}

// ── Idle task entry ─────────────────────────────────────────
// Called when switch_to first resumes an idle task that was
// never previously switched away from (thread->rip == 0).
// The idle task is always RUNNING but never on a runqueue —
// schedule() falls back to it when the rbtree is empty.
__attribute__((noreturn)) static void idle_task_resume(void)
{
    percpu_t *cpu = this_cpu();

    // First switch-IN does NOT resume through schedule()'s resume
    // label (thread->rip was 0, so we start here directly).  The
    // in_schedule guard set by that schedule() call is still 1 —
    // clear it, or every future schedule() on this CPU bails out as
    // "nested" and no other task ever runs (terminal hang).
    current->in_schedule = 0;
    // switch_to left IRQs disabled (cli).  hlt() needs IF=1 to be
    // woken by the tick — same as ap_entry() does for APs.
    arch_local_irq_enable();

    while (1) {
        arch_cpu_halt();
        if (cpu->need_resched) {
            schedule();
            // schedule() returns with IRQs disabled; hlt() needs
            // IF=1 to be woken by the tick.
            arch_local_irq_enable();
        }
    }
}
