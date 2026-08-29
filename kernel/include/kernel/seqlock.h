#ifndef _KERNEL_SEQLOCK_H
#define _KERNEL_SEQLOCK_H

#include <stdint.h>
#include <kernel/arch/spinlock.h>

/*
 * Readers copy their data between seqlock_read_begin() and
 * seqlock_read_retry().  Writers must update every field while holding the
 * write lock and must not sleep in that critical section.
 */
typedef struct {
    volatile uint64_t sequence;
    spinlock_T        writer_lock;
} seqlock_t;

void     seqlock_init(seqlock_t *lock);
uint64_t seqlock_read_begin(const seqlock_t *lock);
int      seqlock_read_retry(const seqlock_t *lock, uint64_t start);
void     seqlock_write_lock(seqlock_t *lock);
void     seqlock_write_unlock(seqlock_t *lock);

#endif /* _KERNEL_SEQLOCK_H */
