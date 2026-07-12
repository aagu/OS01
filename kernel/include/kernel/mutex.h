#ifndef _KERNEL_MUTEX_H
#define _KERNEL_MUTEX_H

#include <kernel/wait.h>
#include <stdint.h>

typedef struct {
    volatile int64_t owner;  // 0=free, >0=holder PID
    wait_queue_t wq;
} mutex_t;

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);
int  mutex_trylock(mutex_t *m);       // returns 1 on success
void mutex_unlock(mutex_t *m);
int  mutex_lock_interruptible(mutex_t *m);  // returns -EINTR on signal

#endif
