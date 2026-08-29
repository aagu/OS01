#include <kernel/rwlock.h>
#include <kernel/arch/atomic.h>

#define RWLOCK_WRITER       (1ULL << 63)
#define RWLOCK_WAITER_ONE   (1ULL << 32)
#define RWLOCK_READERS      (RWLOCK_WAITER_ONE - 1)
#define RWLOCK_WAITERS      (RWLOCK_WRITER - RWLOCK_WAITER_ONE)

static inline void rwlock_relax(void)
{
#if defined(__x86_64__)
    __asm__ __volatile__("pause" ::: "memory");
#elif defined(__aarch64__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    __asm__ __volatile__("" ::: "memory");
#endif
}

void rwlock_init(rwlock_t *lock)
{
    lock->state = 0;
}

int rwlock_try_read_lock(rwlock_t *lock)
{
    uint64_t state = arch_atomic_read(&lock->state);

    if (state & (RWLOCK_WRITER | RWLOCK_WAITERS))
        return 0;
    return arch_atomic_cas(&lock->state, state, state + 1);
}

void rwlock_read_lock(rwlock_t *lock)
{
    while (!rwlock_try_read_lock(lock))
        rwlock_relax();
}

void rwlock_read_unlock(rwlock_t *lock)
{
    arch_atomic_fetch_sub(&lock->state, 1);
}

int rwlock_try_write_lock(rwlock_t *lock)
{
    return arch_atomic_cas(&lock->state, 0, RWLOCK_WRITER);
}

void rwlock_write_lock(rwlock_t *lock)
{
    /* Register once.  The count survives a preceding writer's unlock, so
     * readers cannot barge between queued writers. */
    for (;;) {
        uint64_t state = arch_atomic_read(&lock->state);
        if (arch_atomic_cas(&lock->state, state, state + RWLOCK_WAITER_ONE))
            break;
    }

    for (;;) {
        uint64_t state = arch_atomic_read(&lock->state);
        if ((state & (RWLOCK_WRITER | RWLOCK_READERS)) == 0 &&
            arch_atomic_cas(&lock->state, state,
                            (state - RWLOCK_WAITER_ONE) | RWLOCK_WRITER))
            return;
        rwlock_relax();
    }
}

void rwlock_write_unlock(rwlock_t *lock)
{
    for (;;) {
        uint64_t state = arch_atomic_read(&lock->state);
        if (arch_atomic_cas(&lock->state, state, state & ~RWLOCK_WRITER))
            return;
    }
}
