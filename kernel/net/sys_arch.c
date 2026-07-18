// kernel/net/sys_arch.c — lwIP OS adaptation layer for OS01
//
// lwIP expects sys_arch to provide:
//   - Semaphores (sys_sem_new, sys_sem_signal, sys_sem_free)
//   - Mailboxes  (sys_mbox_new, sys_mbox_post, sys_mbox_fetch, sys_mbox_free)
//   - Threads    (sys_thread_new)
//   - Protection (sys_arch_protect, sys_arch_unprotect)
//   - Time       (sys_now)
//
// lwIP 2.2.0 API: functions take sys_sem_t*/sys_mbox_t* (pointer to handle).
// We allocate structs via kmalloc and store pointers in *sem/*mbox.

#include "lwip/sys.h"       // SYS_ARCH_TIMEOUT, SYS_MBOX_EMPTY, err_t
#include <kernel/arch/spinlock.h>
#include <kernel/wait.h>
#include <kernel/task.h>
#include <kernel/slab.h>      // kmalloc, kfree
#include <device/timer.h>     // jiffies
#include <string.h>           // strdup
#include <errno.h>            // for errno extern

// Kernel-side errno — the kernel doesn't have per-thread errno but
// libk functions pulled in by our code reference it.
int errno;

// ═══════════════════════════════════════════════════════════════
//  Semaphores — spinlock + counter + wait_queue
// ═══════════════════════════════════════════════════════════════

typedef struct {
    int           count;
    spinlock_T    lock;
    wait_queue_t  wq;
} os_sem_t;

err_t sys_sem_new(sys_sem_t *sem, u8_t count)
{
    os_sem_t *s = (os_sem_t *)kmalloc(sizeof(os_sem_t));
    if (!s) return ERR_MEM;
    s->count = (int)count;
    spin_init(&s->lock);
    wait_queue_init(&s->wq);
    *sem = (sys_sem_t)s;
    return ERR_OK;
}

void sys_sem_signal(sys_sem_t *sem)
{
    if (!sem || !*sem) return;
    os_sem_t *s = (os_sem_t *)*sem;
    uint64_t flags = spin_lock_irqsave(&s->lock);
    s->count++;
    spin_unlock_irqrestore(&s->lock, flags);
    wait_queue_wake_one(&s->wq);
}

u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout)
{
    if (!sem || !*sem) return SYS_ARCH_TIMEOUT;
    os_sem_t *s = (os_sem_t *)*sem;
    (void)timeout;  // infinite only

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&s->lock);
        if (s->count > 0) {
            s->count--;
            spin_unlock_irqrestore(&s->lock, flags);
            return 0;
        }
        spin_unlock_irqrestore(&s->lock, flags);
        wait_queue_sleep(&s->wq);
    }
}

void sys_sem_free(sys_sem_t *sem)
{
    // lwIP semaphores are created once at init and never freed.
    // We allocate them with kmalloc but free is a no-op — they
    // live for the lifetime of the kernel.
    (void)sem;
}

// ═══════════════════════════════════════════════════════════════
//  Mailboxes — ring buffer (64 slots) + wait_queue
// ═══════════════════════════════════════════════════════════════

#define MBOX_SIZE 64

typedef struct {
    void         *queue[MBOX_SIZE];
    int           head;
    int           tail;
    int           count;
    spinlock_T    lock;
    wait_queue_t  wq;
} os_mbox_t;

err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
    (void)size;
    os_mbox_t *mb = (os_mbox_t *)kmalloc(sizeof(os_mbox_t));
    if (!mb) return ERR_MEM;
    mb->head = 0;
    mb->tail = 0;
    mb->count = 0;
    spin_init(&mb->lock);
    wait_queue_init(&mb->wq);
    *mbox = (sys_mbox_t)mb;
    return ERR_OK;
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg)
{
    if (!mbox || !*mbox) return;
    os_mbox_t *mb = (os_mbox_t *)*mbox;

    uint64_t flags = spin_lock_irqsave(&mb->lock);
    if (mb->count >= MBOX_SIZE) {
        spin_unlock_irqrestore(&mb->lock, flags);
        return;
    }
    mb->queue[mb->head] = msg;
    mb->head = (mb->head + 1) % MBOX_SIZE;
    mb->count++;
    spin_unlock_irqrestore(&mb->lock, flags);

    wait_queue_wake_one(&mb->wq);
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout)
{
    if (!mbox || !*mbox) return SYS_ARCH_TIMEOUT;
    os_mbox_t *mb = (os_mbox_t *)*mbox;
    (void)timeout;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&mb->lock);
        if (mb->count > 0) {
            *msg = mb->queue[mb->tail];
            mb->tail = (mb->tail + 1) % MBOX_SIZE;
            mb->count--;
            spin_unlock_irqrestore(&mb->lock, flags);
            return 0;
        }
        spin_unlock_irqrestore(&mb->lock, flags);
        wait_queue_sleep(&mb->wq);
    }
}

u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **msg)
{
    if (!mbox || !*mbox) return SYS_MBOX_EMPTY;
    os_mbox_t *mb = (os_mbox_t *)*mbox;

    uint64_t flags = spin_lock_irqsave(&mb->lock);
    if (mb->count > 0) {
        *msg = mb->queue[mb->tail];
        mb->tail = (mb->tail + 1) % MBOX_SIZE;
        mb->count--;
        spin_unlock_irqrestore(&mb->lock, flags);
        return 0;
    }
    spin_unlock_irqrestore(&mb->lock, flags);
    return SYS_MBOX_EMPTY;
}

void sys_mbox_free(sys_mbox_t *mbox)
{
    (void)mbox;
}

// ── mbox trypost (lwIP calls these directly, not via macros) ──

err_t sys_mbox_trypost(sys_mbox_t *mbox, void *msg)
{
    if (!mbox || !*mbox) return ERR_MEM;
    os_mbox_t *mb = (os_mbox_t *)*mbox;

    uint64_t flags = spin_lock_irqsave(&mb->lock);
    if (mb->count >= MBOX_SIZE) {
        spin_unlock_irqrestore(&mb->lock, flags);
        return ERR_MEM;
    }
    mb->queue[mb->head] = msg;
    mb->head = (mb->head + 1) % MBOX_SIZE;
    mb->count++;
    spin_unlock_irqrestore(&mb->lock, flags);

    wait_queue_wake_one(&mb->wq);
    return ERR_OK;
}

err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *msg)
{
    // Same as trypost — our spin_lock_irqsave is ISR-safe.
    return sys_mbox_trypost(mbox, msg);
}

// ── Mutex (lwIP calls these as real functions when LWIP_TCPIP_CORE_LOCKING) ──

err_t sys_mutex_new(sys_mutex_t *mutex)
{
    return sys_sem_new(mutex, 1);
}

void sys_mutex_lock(sys_mutex_t *mutex)
{
    sys_arch_sem_wait(mutex, 0);
}

void sys_mutex_unlock(sys_mutex_t *mutex)
{
    sys_sem_signal(mutex);
}

// ═══════════════════════════════════════════════════════════════
//  Thread — create_kthread wrapper
// ═══════════════════════════════════════════════════════════════

struct lwip_thread_ctx {
    lwip_thread_fn fn;
    void          *arg;
};

static uint64_t lwip_thread_entry(uint64_t arg)
{
    struct lwip_thread_ctx *ctx = (struct lwip_thread_ctx *)(uintptr_t)arg;
    ctx->fn(ctx->arg);
    kfree(ctx);
    return 0;
}

sys_thread_t sys_thread_new(const char *name,
                            lwip_thread_fn thread, void *arg,
                            int stacksize, int prio)
{
    (void)stacksize;
    (void)prio;

    char *name_copy = strdup(name);
    if (!name_copy) return 0;

    struct lwip_thread_ctx *ctx = kmalloc(sizeof(*ctx));
    if (!ctx) { kfree(name_copy); return 0; }
    ctx->fn   = thread;
    ctx->arg  = arg;

    task_t *t = create_kthread(lwip_thread_entry,
                               (uint64_t)(uintptr_t)ctx, name_copy);
    if (!t) {
        kfree(name_copy);
        kfree(ctx);
        return 0;
    }
    (void)name_copy; return (sys_thread_t)(uintptr_t)t;
}

// ═══════════════════════════════════════════════════════════════
//  Protection — recursive IRQ-save spinlock
// ═══════════════════════════════════════════════════════════════

static spinlock_T  lwip_global_lock = { .lock = 1 };
static volatile int protect_nest = 0;
static uint64_t     protect_flags = 0;

sys_prot_t sys_arch_protect(void)
{
    if (protect_nest == 0) {
        protect_flags = spin_lock_irqsave(&lwip_global_lock);
    }
    protect_nest++;
    return protect_flags;
}

void sys_arch_unprotect(sys_prot_t pval)
{
    (void)pval;
    if (protect_nest <= 0) return;
    if (--protect_nest == 0)
        spin_unlock_irqrestore(&lwip_global_lock, protect_flags);
}

// ═══════════════════════════════════════════════════════════════
//  Time
// ═══════════════════════════════════════════════════════════════

u32_t sys_now(void)
{
    return (u32_t)(jiffies * 10);
}

// ═══════════════════════════════════════════════════════════════
//  Misc
// ═══════════════════════════════════════════════════════════════

void sys_init(void)
{
    // lwIP calls this once at startup. No action needed.
}

void sys_msleep(u32_t ms)
{
    // We don't support ms-level sleep yet. lwIP uses this sparingly.
    // For now, busy-wait is the fallback (called only in init paths).
    (void)ms;
}
