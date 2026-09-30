/* Host-only runtime surface for compiling the real devfs.c + file.c
 * sources for the gfx file lifecycle test.
 *
 * The runtime skips heavyweight kernel headers via their include
 * guards (sched/task.h, arch/cpu.h, arch/irq.h, percpu/percpu.h,
 * memory/slab.h), then provides a small stub surface for the
 * symbols those modules pull in (task_struct, current, schedule,
 * kmalloc/kfree, ...).  Pattern follows the existing
 * poll_test_runtime.h (see hosttests/cases/test_poll_requested.c).
 *
 * Headers NOT skipped (and therefore declared by production
 * headers): tty/tty.h, tty/pty.h, driver/keyboard.h,
 * driver/serial.h, random/random.h, memory/uaccess.h,
 * memory/vmm.h, fs/vfs.h, fs/file.h, fs/devfs.h.  For each, the
 * test TU must provide a stub implementation of every non-inline
 * function the production code references.
 */
#ifndef OS01_GFX_TEST_RUNTIME_H
#define OS01_GFX_TEST_RUNTIME_H

#include "test_platform.h"
#include <list.h>
#include <uapi/time.h>
#include <termios.h>
#include <block/blockdev.h>
#include <fs/vfs.h>

/* ── Skip heavyweight kernel headers via their include guards ──
 * Skip task.h, cpu.h, irq.h, percpu.h, slab.h.  Everything else
 * (uaccess.h, vmm.h, tty.h, pty.h, keyboard.h, serial.h, random.h,
 * vfs.h, file.h, devfs.h, blockdev.h, debug.h) is included
 * normally, and the test TU provides stub implementations for the
 * symbols they declare. */
#define KERNEL_TASK_H
#define _KERNEL_PERCPU_H
#define _ARCH_IRQ_H
#define _ARCH_CPU_H
#define _KERNEL_SLAB_H
#define _DRIVER_KEYBOARD_H

/* ── task_struct + current ────────────────────────────────
 * sched/task.h is skipped, so we provide a minimal task_struct
 * mirroring the production layout (file.c uses current->files /
 * current->pgrp / current->addr_limit, all via task_struct fields). */
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
} gfx_test_task_union_t;

#include <fs/file.h>

/* ── Sched constants used in file.c / devfs.c ── */
#define TASK_RUNNING       (1 << 0)
#define TASK_INTERRUPTIBLE (1 << 1)
#define CTTY_PTY           2

extern task_t *gfx_test_current;
#define current gfx_test_current

void task_wake(task_t *task);
void schedule(void);
extern spinlock_T task_list_lock;
extern gfx_test_task_union_t init_task_union;
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

/* ── Slab stubs (memory/slab.h is skipped; test_platform.h provides
 *  weak kmalloc/kfree but the production code needs the declarations
 *  to be visible from this header chain). */
void *kmalloc(size_t size);
size_t kfree(void *ptr);

/* ── Uaccess stub symbols (declared extern in memory/uaccess.h) ── */
ssize_t copy_to_user_ft_res(void *dst, const void *src, size_t n,
                            void (*on_fault)(void *), void *arg);
ssize_t copy_from_user_ft_res(void *dst, const void *src, size_t n,
                              void (*on_fault)(void *), void *arg);
int  strnlen_user(const void *user_addr, size_t max);
bool syscall_check_user_range(uint64_t addr, uint64_t len, bool writable);

/* ── VMM stubs (declared extern in memory/vmm.h) ── */
int  user_write_range_begin(uint64_t addr, size_t len);
void user_write_range_end(void);
int  user_read_range_begin(uint64_t addr, size_t len);
void user_read_range_end(void);

/* ── VFS stubs (declared extern in kernel/include/fs/vfs.h) ── */
int  vfs_mount(const char *path, block_device_t *dev,
               vfs_ops_t *ops, void *fs_data);
struct vfs_node *vfs_lookup(const char *path);
struct vfs_node *vfs_lookup_from(const char *path, const char *cwd);
struct vfs_node *vfs_node_get(struct vfs_node *node);
void vfs_node_put(struct vfs_node *node);

/* ── log debug ── */
void log_debug(const char *fmt, ...);

/* ── wait_queue (no-ops for the lifecycle test) ── */
void wait_queue_init(wait_queue_t *wq);
void wait_queue_sleep(wait_queue_t *wq);
void wait_queue_wake_one(wait_queue_t *wq);
void wait_queue_wake_all(wait_queue_t *wq);

#endif
