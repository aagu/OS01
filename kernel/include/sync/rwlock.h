#ifndef _KERNEL_RWLOCK_H
#define _KERNEL_RWLOCK_H

#include <stdint.h>

/*
 * A spinning reader/writer lock.  The low 32 bits count readers; the next
 * 31 bits count queued writers; the top bit records an active writer.
 * Readers may nest read locks if they balance every acquisition; write locks
 * are non-recursive.  Neither kind may span an operation that can sleep.
 */
typedef struct {
    volatile uint64_t state;
} rwlock_t;

void rwlock_init(rwlock_t *lock);
void rwlock_read_lock(rwlock_t *lock);
int  rwlock_try_read_lock(rwlock_t *lock);
void rwlock_read_unlock(rwlock_t *lock);
void rwlock_write_lock(rwlock_t *lock);
int  rwlock_try_write_lock(rwlock_t *lock);
void rwlock_write_unlock(rwlock_t *lock);

#endif /* _KERNEL_RWLOCK_H */
