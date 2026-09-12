#include <kernel/rwlock.h>
#include <kernel/seqlock.h>

int test_rwlock_basic(void)
{
    rwlock_t lock;
    rwlock_init(&lock);

    if (!rwlock_try_read_lock(&lock))
        return -1;
    if (rwlock_try_write_lock(&lock))
        return -2;
    rwlock_read_unlock(&lock);

    if (!rwlock_try_write_lock(&lock))
        return -3;
    if (rwlock_try_read_lock(&lock))
        return -4;
    rwlock_write_unlock(&lock);

    return 0;
}

int test_seqlock_basic(void)
{
    seqlock_t lock;
    seqlock_init(&lock);

    uint64_t seq = seqlock_read_begin(&lock);
    if (seqlock_read_retry(&lock, seq))
        return -1;

    seqlock_write_lock(&lock);
    seqlock_write_unlock(&lock);

    if (!seqlock_read_retry(&lock, seq))
        return -2;

    seq = seqlock_read_begin(&lock);
    if (seqlock_read_retry(&lock, seq))
        return -3;

    return 0;
}
