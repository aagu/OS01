#include <sched/internal.h>
#include <intr/ipi.h>
#include <arch/cpu.h>
#include <arch/irq.h>
#include <arch/spinlock.h>
#include <core/debug.h>
#include <log/log.h>
#include <kernel.h>

/* ── sched_pick_cpu: choose CPU with fewest nr_running ────
 * Called from do_fork() and spawn_user_task() to place
 * new tasks on the least-loaded CPU.
 * Complexity: O(num_cpus).  Acceptable for NR_CPUS ≤ 8.
 */
uint32_t sched_pick_cpu(void)
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
void sched_notify_remote(task_t *tsk)
{
    if ((int)tsk->cpu == (int)cpu_id())
        return;
    percpu_t *dst = &percpu_data[tsk->cpu];
    dst->need_resched = 1;
    __sync_synchronize();
    ipi_send(dst->arch_processor_id, IPI_VECTOR_RESCHED);
}

/* ── sched_balance: pull work from busiest CPU ─────────────
 * Called by schedule() when local runqueue has fewer tasks
 * than the busiest CPU.
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
void sched_balance(percpu_t *rq)
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
            /* Never migrate the user-space init process (pid==1),
             * and NEVER migrate a task that is on a CPU (running or
             * committed by pick).  schedule() re-enqueues current
             * (on_rq=true) while it still runs (on_cpu=1) for
             * vruntime reordering; without the on_cpu check another
             * CPU's balancer steals it and BOTH CPUs operate on the
             * same task (double-book -> stack clobber, RIP=2/1). */
            if (t->pid != user_init_pid &&
                !__atomic_load_n(&t->on_cpu, __ATOMIC_ACQUIRE)) {
                dequeue_task(t, src_rq);
                t->cpu = rq->cpu_id;

                /* Normalize vruntime to target CPU's timeline */
                if (t->vruntime < rq->min_vruntime)
                    t->vruntime = rq->min_vruntime;

                enqueue_task(t, rq);
                taken++;
            }
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
                    rq->cpu_id, taken, src_idx, src_rq->nr_running, rq->nr_running);
    }
}
