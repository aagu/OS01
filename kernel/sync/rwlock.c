#include <kernel/rwlock.h>
#include <kernel/arch/atomic.h>
#include <kernel/arch/cpu.h>     // arch_cpu_pause — arch-neutral spin hint

#define RWLOCK_WRITER       (1ULL << 63)
#define RWLOCK_WAITER_ONE   (1ULL << 32)
#define RWLOCK_READERS      (RWLOCK_WAITER_ONE - 1)
#define RWLOCK_WAITERS      (RWLOCK_WRITER - RWLOCK_WAITER_ONE)

// Architecture-neutral back-off hint inside the CAS spin. On x86_64
// arch_cpu_pause() emits `pause`; on aarch64 it emits `yield`. Both
// are hints to the CPU that we're in a spin loop — improves power /
// SMT behaviour without changing semantics.
static inline void rwlock_relax(void)
{
    arch_cpu_pause();
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
