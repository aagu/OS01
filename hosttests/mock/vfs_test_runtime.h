#ifndef OS01_VFS_TEST_RUNTIME_H
#define OS01_VFS_TEST_RUNTIME_H

#include <stdint.h>

#define OS01_HOST_SPINLOCK_T_PROVIDED
typedef struct { unsigned long lock; } spinlock_T;
void poll_test_spinlock_acquire(spinlock_T *lock) __attribute__((weak));
static inline void spin_init(spinlock_T *l) { if (l) l->lock = 1; }
static inline void spin_lock(spinlock_T *l) { if (l) l->lock = 0; }
static inline void spin_unlock(spinlock_T *l) { if (l) l->lock = 1; }
static inline long spin_trylock(spinlock_T *l) {
    if (!l || l->lock == 0) return 0;
    l->lock = 0;
    return 1;
}
static inline uint64_t spin_lock_irqsave(spinlock_T *l)
{
    if (l) l->lock = 0;
    if (poll_test_spinlock_acquire) poll_test_spinlock_acquire(l);
    return 0;
}
static inline void spin_unlock_irqrestore(spinlock_T *l, uint64_t f)
{
    (void)f;
    if (l) l->lock = 1;
}

#include "test_platform.h"
#include <list.h>
#include <uapi/time.h>

#define KERNEL_TASK_H
#define _KERNEL_PERCPU_H
#define _ARCH_IRQ_H
#define _ARCH_CPU_H
#define _ARCH_SPINLOCK_H
#define _KERNEL_SLAB_H
#define _KERNEL_MEMORY_H
#define _KERNEL_MUTEX_H

typedef struct {
    volatile int64_t owner;
} mutex_t;

static inline void mutex_init(mutex_t *m) { if (m) m->owner = 0; }
static inline void mutex_lock(mutex_t *m) { if (m) m->owner = 1; }
static inline int  mutex_trylock(mutex_t *m) { if (m && m->owner == 0) { m->owner = 1; return 1; } return 0; }
static inline void mutex_unlock(mutex_t *m) { if (m) m->owner = 0; }
static inline int  mutex_lock_interruptible(mutex_t *m) { mutex_lock(m); return 0; }

#define CTTY_NONE          0
#define CTTY_PHYS          1
#define CTTY_PTY           2

struct files_struct;

typedef struct task_struct {
    list_t list;
    volatile int64_t state;
    uint64_t addr_limit;
    int64_t pid;
    pid_t pgrp;
    pid_t session;
    int64_t signal;
    int64_t blocked;
    struct files_struct *files;
    list_t io_wait_node;
    int ctty_type;
    void *ctty;
} task_t;

#include <fs/file.h>

extern task_t *vfs_test_current;
#define current vfs_test_current

void *kmalloc(size_t size);
size_t kfree(void *ptr);

#endif /* OS01_VFS_TEST_RUNTIME_H */
