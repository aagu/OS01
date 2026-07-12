#include <kernel/mutex.h>
#include <kernel/task.h>
#include <kernel/arch/x86_64/cpu.h>
#include <errno.h>

void mutex_init(mutex_t *m)
{
    m->owner = 0;
    wait_queue_init(&m->wq);
}

void mutex_lock(mutex_t *m)
{
    while (atomic_cas((volatile uint64_t *)&m->owner, 0, (uint64_t)current->pid) == 0) {
        wait_queue_sleep(&m->wq);
    }
}

int mutex_trylock(mutex_t *m)
{
    return atomic_cas((volatile uint64_t *)&m->owner, 0, (uint64_t)current->pid);
}

void mutex_unlock(mutex_t *m)
{
    atomic_write((volatile uint64_t *)&m->owner, 0);  // xchgq provides full barrier
    wait_queue_wake_one(&m->wq);
}

int mutex_lock_interruptible(mutex_t *m)
{
    while (atomic_cas((volatile uint64_t *)&m->owner, 0, (uint64_t)current->pid) == 0) {
        if (current->signal)
            return -EINTR;
        wait_queue_sleep(&m->wq);
        if (current->signal)
            return -EINTR;
    }
    return 0;
}
