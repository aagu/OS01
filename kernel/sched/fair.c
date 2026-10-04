#include <sched/internal.h>
#include <core/assert.h>
#include <kernel.h>

/* ── update_curr: advance vruntime by 1 tick ──────── */
void update_curr(task_t *task)
{
    if (!task || task == this_cpu()->idle)
        return;
    task->vruntime += 1;
    if (task->vruntime >= task->deadline)
        this_cpu()->need_resched = 1;
}

/* ── rbtree comparator: order by deadline ─────────── */
int cmp_deadline(rbtree_node_t *a, rbtree_node_t *b)
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
void enqueue_task(task_t *task, percpu_t *rq)
{
    /*
     * Idle tasks must never appear on a runqueue.  They are
     * always RUNNING and selected only as a last resort when
     * pick_eevdf() finds the rbtree empty.
     */
    ASSERT(task != rq->idle);

    task->deadline = task->vruntime + EEVDF_MIN_SLICE;
    // [FIX-atomic] RELEASE: paired with task_wake's ACQUIRE load
    // (on_rq) so a concurrent wakeup never sees a stale false.
    __atomic_store_n(&task->on_rq, 1, __ATOMIC_RELEASE);
    rbtree_node_t *conflict = rbtree_insert(&rq->run_queue, &task->rb_node, cmp_deadline);
    ASSERT(conflict == NULL);
    rq->nr_running++;
}

void dequeue_task(task_t *task, percpu_t *rq)
{
    rbtree_erase(&rq->run_queue, &task->rb_node);
    // [FIX-atomic] RELEASE store (see enqueue_task).
    __atomic_store_n(&task->on_rq, 0, __ATOMIC_RELEASE);
    rq->nr_running--;
}

/* ── pick_eevdf: select next task O(log n) ─────────── */
task_t *pick_eevdf(percpu_t *rq)
{
    if (rbtree_empty(&rq->run_queue))
        return rq->idle;
    rbtree_node_t *node = rbtree_first(&rq->run_queue);
    task_t *t = container_of(node, task_t, rb_node);
    if (t->vruntime > rq->min_vruntime + EEVDF_LATENCY)
        rq->min_vruntime = t->vruntime;
    return t;
}
