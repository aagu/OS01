/* Host-only runtime surface for compiling the real poll/select sources. */
#ifndef OS01_POLL_TEST_RUNTIME_H
#define OS01_POLL_TEST_RUNTIME_H

#include "test_platform.h"
#include <list.h>
#include <uapi/time.h>

/* Keep heavyweight scheduler/architecture headers out of host objects. */
#define KERNEL_TASK_H
#define _KERNEL_PERCPU_H
#define _ARCH_IRQ_H
#define _ARCH_CPU_H
#define _KERNEL_SLAB_H

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

typedef struct {
    task_t task;
} poll_test_task_union_t;

#include <kernel/file.h>

#define TASK_RUNNING       (1 << 0)
#define TASK_INTERRUPTIBLE (1 << 1)
#define CTTY_PTY           2

extern task_t *poll_test_current;
#define current poll_test_current

void task_wake(task_t *task);
void schedule(void);
extern spinlock_T task_list_lock;
extern poll_test_task_union_t init_task_union;
list_t *task_list_next(list_t *pos);
int signal_pgrp(pid_t target, int sig);
static inline void arch_local_irq_enable(void) {}
static inline uint64_t arch_cycle_counter(void) { return 0; }
static inline int arch_signal_pending_fatal(void) { return 0; }
static inline int arch_do_signal_delivery(void *regs)
{
    (void)regs;
    return 0;
}

void *kmalloc(size_t size);
size_t kfree(void *ptr);

#endif
