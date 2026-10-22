#include <sync/mutex.h>
#include <sched/task.h>
#include <arch/atomic.h>
#include <errno.h>
#include <arch/irq.h>

void mutex_init(mutex_t *m)
{
    m->owner = 0;
    wait_queue_init(&m->wq);
}

static int mutex_acquire(mutex_t *m, int interruptible)
{
    for (;;) {
        if (arch_atomic_cas((volatile uint64_t *)&m->owner, 0,
                            (uint64_t)current->pid))
            return 0;

        // Arm before retrying: an unlock between the failed CAS and
        // enqueue may have woken an empty queue. Keep local IRQs off so
        // preemption cannot sleep us before the condition recheck.
        uint64_t irq_flags = arch_local_irq_save();
        wait_queue_arm(&m->wq);
        int acquired = arch_atomic_cas((volatile uint64_t *)&m->owner, 0,
                                      (uint64_t)current->pid);
        if (!acquired && !(interruptible && current->signal))
            schedule();

        // Wake-one may remove this same node on another CPU.
        uint64_t flags = spin_lock_irqsave(&m->wq.lock);
        wait_queue_disarm();
        spin_unlock_irqrestore(&m->wq.lock, flags);
        arch_local_irq_restore(irq_flags);
        if (acquired)
            return 0;
        if (interruptible && current->signal) {
            // We may have consumed unlock's wake-one before cancellation.
            // Let the next waiter retry rather than leave a free lock idle.
            wait_queue_wake_one(&m->wq);
            return -EINTR;
        }
    }
}

void mutex_lock(mutex_t *m)
{
    (void)mutex_acquire(m, 0);
}

int mutex_trylock(mutex_t *m)
{
    return arch_atomic_cas((volatile uint64_t *)&m->owner, 0, (uint64_t)current->pid);
}

void mutex_unlock(mutex_t *m)
{
    arch_atomic_write((volatile uint64_t *)&m->owner, 0);  // xchgq provides full barrier
    wait_queue_wake_one(&m->wq);
}

int mutex_lock_interruptible(mutex_t *m)
{
    return mutex_acquire(m, 1);
}
