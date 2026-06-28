#include <kernel/wait.h>
#include <kernel.h>
#include <kernel/percpu.h>

void wait_queue_init(wait_queue_t *wq)
{
    list_init(&wq->head);
    spin_init(&wq->lock);
}

void wait_queue_sleep(wait_queue_t *wq)
{
    // Enqueue + re-check pattern: grab lock, add self,
    // release, then set INTERRUPTIBLE and schedule.
    // The double-check of the caller's condition happens
    // OUTSIDE this function (same as tty_read does after
    // schedule() returns).
    uint64_t flags = spin_lock_irqsave(&wq->lock);
    list_add_to_before(&wq->head, &current->io_wait_node);
    spin_unlock_irqrestore(&wq->lock, flags);

    current->state = TASK_INTERRUPTIBLE;
    schedule();

    // Clean up if we were woken by something other than
    // wait_queue_wake_one (signal, etc.).  The normal path
    // already did list_del_init on our node.
    if (!list_is_empty(&current->io_wait_node))
        list_del_init(&current->io_wait_node);
    current->state = TASK_RUNNING;
}

void wait_queue_wake_one(wait_queue_t *wq)
{
    uint64_t flags = spin_lock_irqsave(&wq->lock);
    if (!list_is_empty(&wq->head)) {
        list_t *node = wq->head.next;
        list_del_init(node);
        task_t *t = container_of(node, task_t, io_wait_node);
        t->state = TASK_RUNNING;
    }
    spin_unlock_irqrestore(&wq->lock, flags);
}

void wait_queue_wake_all(wait_queue_t *wq)
{
    uint64_t flags = spin_lock_irqsave(&wq->lock);
    while (!list_is_empty(&wq->head)) {
        list_t *node = wq->head.next;
        list_del_init(node);
        task_t *t = container_of(node, task_t, io_wait_node);
        t->state = TASK_RUNNING;
    }
    spin_unlock_irqrestore(&wq->lock, flags);
}
