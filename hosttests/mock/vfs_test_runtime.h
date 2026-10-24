#ifndef OS01_VFS_TEST_RUNTIME_H
#define OS01_VFS_TEST_RUNTIME_H

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
