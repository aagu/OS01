// kernel/net/sys_arch.c — lwIP OS adaptation layer for OS01
//
// lwIP expects sys_arch to provide:
//   - Semaphores (sys_sem_new, sys_sem_signal, sys_sem_free)
//   - Mailboxes  (sys_mbox_new, sys_mbox_post, sys_mbox_fetch, sys_mbox_free)
//   - Threads    (sys_thread_new)
//   - Protection (sys_arch_protect, sys_arch_unprotect)
//   - Time       (sys_now)

#include "lwip/sys.h"
#include <kernel/arch/spinlock.h>
#include <kernel/wait.h>
#include <kernel/task.h>
#include <kernel/slab.h>       // kmalloc, kfree
#include <device/timer.h>
#include <string.h>            // strdup

// ═══════════════════════════════════════════════════════════════
//  Semaphores — spinlock + counter + wait_queue
// ═══════════════════════════════════════════════════════════════

typedef struct {
    int           count;
    spinlock_T    lock;
    wait_queue_t  wq;
} os_sem_t;

sys_sem_t sys_sem_new(u8_t count)
{
    os_sem_t *sem = (os_sem_t *)kmalloc(sizeof(os_sem_t));
    if (!sem) return NULL;
    sem->count = (int)count;
    spin_init(&sem->lock);
    wait_queue_init(&sem->wq);
    return (sys_sem_t)sem;
}

void sys_sem_signal(sys_sem_t sem)
{
    os_sem_t *s = (os_sem_t *)sem;
    if (!s) return;
    uint64_t flags = spin_lock_irqsave(&s->lock);
    s->count++;
    spin_unlock_irqrestore(&s->lock, flags);
    wait_queue_wake_one(&s->wq);
}

u32_t sys_arch_sem_wait(sys_sem_t sem, u32_t timeout_ms)
{
    os_sem_t *s = (os_sem_t *)sem;
    if (!s) return SYS_ARCH_TIMEOUT;
    (void)timeout_ms;  // infinite only for now

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

void sys_sem_free(sys_sem_t sem)
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
    int           head;       // producer writes here
    int           tail;       // consumer reads here
    int           count;
    spinlock_T    lock;
    wait_queue_t  wq;         // readers wait here
} os_mbox_t;

sys_mbox_t sys_mbox_new(int size)
{
    (void)size;
    os_mbox_t *mb = (os_mbox_t *)kmalloc(sizeof(os_mbox_t));
    if (!mb) return NULL;
    mb->head = 0;
    mb->tail = 0;
    mb->count = 0;
    spin_init(&mb->lock);
    wait_queue_init(&mb->wq);
    return (sys_mbox_t)mb;
}

void sys_mbox_post(sys_mbox_t mbox, void *msg)
{
    os_mbox_t *mb = (os_mbox_t *)mbox;
    if (!mb) return;

    uint64_t flags = spin_lock_irqsave(&mb->lock);
    if (mb->count >= MBOX_SIZE) {
        // Drop on overflow — should not happen with properly
        // sized mailboxes.  Log and bail.
        spin_unlock_irqrestore(&mb->lock, flags);
        return;
    }
    mb->queue[mb->head] = msg;
    mb->head = (mb->head + 1) % MBOX_SIZE;
    mb->count++;
    spin_unlock_irqrestore(&mb->lock, flags);

    wait_queue_wake_one(&mb->wq);
}

u32_t sys_arch_mbox_fetch(sys_mbox_t mbox, void **msg, u32_t timeout_ms)
{
    os_mbox_t *mb = (os_mbox_t *)mbox;
    if (!mb) return SYS_ARCH_TIMEOUT;
    (void)timeout_ms;  // infinite only

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

u32_t sys_arch_mbox_tryfetch(sys_mbox_t mbox, void **msg)
{
    os_mbox_t *mb = (os_mbox_t *)mbox;
    if (!mb) return SYS_MBOX_EMPTY;

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

void sys_mbox_free(sys_mbox_t mbox)
{
    (void)mbox;  // live forever, same rationale as semaphores
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
    kfree(ctx);      // ctx was kmalloc'd in sys_thread_new
    return 0;
}

sys_thread_t sys_thread_new(const char *name,
                            lwip_thread_fn thread, void *arg,
                            int stacksize, int prio)
{
    (void)stacksize;
    (void)prio;

    // lwIP may pass a stack-local name string; strdup it.
    char *name_copy = strdup(name);
    if (!name_copy) return NULL;

    // Bundle fn+arg into heap-allocated context so lwip_thread_entry
    // can call fn(arg) without UB-prone function pointer casts.
    struct lwip_thread_ctx *ctx = kmalloc(sizeof(*ctx));
    if (!ctx) { kfree(name_copy); return NULL; }
    ctx->fn   = thread;
    ctx->arg  = arg;

    task_t *t = create_kthread(lwip_thread_entry,
                               (uint64_t)(uintptr_t)ctx, name_copy);
    if (!t) {
        kfree(name_copy);
        kfree(ctx);
        return NULL;
    }
    return (sys_thread_t)t;
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
        // IRQ-save is required: a 100 Hz PIT timer tick during a
        // lwIP critical section may set need_resched; ret_from_intr
        // could then switch to another kernel thread that also enters
        // lwIP, deadlocking on lwip_global_lock.
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
    // jiffies increments at 100 Hz (PIT), so 1 jiffy = 10 ms
    return (u32_t)(jiffies * 10);
}
