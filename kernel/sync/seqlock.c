#include <kernel/seqlock.h>
#include <kernel/arch/atomic.h>

void seqlock_init(seqlock_t *lock)
{
    lock->sequence = 0;
    spin_init(&lock->writer_lock);
}

uint64_t seqlock_read_begin(const seqlock_t *lock)
{
    uint64_t sequence;

    do {
        sequence = arch_atomic_read((volatile uint64_t *)&lock->sequence);
    } while (sequence & 1);

    return sequence;
}

int seqlock_read_retry(const seqlock_t *lock, uint64_t start)
{
    __asm__ __volatile__("" ::: "memory");
    return arch_atomic_read((volatile uint64_t *)&lock->sequence) != start;
}

void seqlock_write_lock(seqlock_t *lock)
{
    spin_lock(&lock->writer_lock);
    arch_atomic_fetch_add(&lock->sequence, 1);
}

void seqlock_write_unlock(seqlock_t *lock)
{
    arch_atomic_fetch_add(&lock->sequence, 1);
    spin_unlock(&lock->writer_lock);
}
