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
#include <kernel/arch/irq.h>
#include <kernel/wait.h>
#include <kernel/task.h>
#include <kernel/percpu.h>
#include <kernel/slab.h>      // kmalloc, kfree
#include <device/timer.h>     // jiffies
#include <string.h>           // strdup
#include <errno.h>            // for errno extern

// Kernel-side errno — the kernel doesn't have per-thread errno but
// libk functions pulled in by our code reference it.
int errno;

// ── Timeout support for mbox_fetch ──────────────────────────────

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

    u32_t deadline = 0;
    if (timeout && timeout != (u32_t)-1) {
        deadline = (u32_t)(jiffies * 10) + timeout;
    }

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&s->lock);
        if (s->count > 0) {
            s->count--;
            spin_unlock_irqrestore(&s->lock, flags);
            return 0;
        }
        spin_unlock_irqrestore(&s->lock, flags);

        // Check timeout (busy-poll at 10ms granularity)
        if (timeout > 0 && timeout != (u32_t)-1) {
            u32_t now = (u32_t)(jiffies * 10);
            if (now >= deadline)
                return SYS_ARCH_TIMEOUT;
        }

        // Lost-wakeup-safe sleep (same rationale as mbox_fetch):
        // enqueue while holding s->lock so sys_sem_signal's count++
        // serializes with our enqueue and its wake_one always sees us.
        {
            uint64_t _f = spin_lock_irqsave(&s->lock);
            if (s->count > 0) { spin_unlock_irqrestore(&s->lock, _f); continue; }
            uint64_t _wf = spin_lock_irqsave(&s->wq.lock);
            list_add_to_before(&s->wq.head, &current->io_wait_node);
            current->state = TASK_INTERRUPTIBLE;
            spin_unlock_irqrestore(&s->wq.lock, _wf);
            spin_unlock_irqrestore(&s->lock, _f);

            schedule();
            arch_local_irq_enable();

            if (!list_is_empty(&current->io_wait_node))
                list_del_init(&current->io_wait_node);
            current->state = TASK_RUNNING;
        }
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
    wait_queue_t  not_full_wq;
    volatile int  idle_wakeup;   // lost-wakeup guard for the idle timer
} os_mbox_t;

// ── Interrupt-driven RX wake ─────────────────────────────────
// tcpip_thread blocks in sys_arch_mbox_fetch on the lwIP mbox.
// The NIC IRQ handler only acknowledges RX and requests a persistent
// sys_mbox_wake().  The tcpip thread re-runs net_poll_rx() and drains
// the two-stage buffer — no message needs to be posted (a sentinel
// would flood the mbox).
/* tcpip_init() creates the core mailbox before it starts tcpip_thread.
 * Keep that mailbox permanently registered: application/netconn waits also
 * call sys_arch_mbox_fetch(), so a "currently fetching mailbox" global is
 * inherently racy and can make the NIC IRQ wake an unrelated socket waiter.
 */
static os_mbox_t *g_tcpip_mbox = NULL;

void sys_mbox_wake(void)
{
    os_mbox_t *mb = g_tcpip_mbox;
    if (!mb) return;
    uint64_t flags = spin_lock_irqsave(&mb->lock);
    mb->idle_wakeup = 1;
    spin_unlock_irqrestore(&mb->lock, flags);
    wait_queue_wake_one(&mb->wq);
}

err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
    (void)size;
    os_mbox_t *mb = (os_mbox_t *)kmalloc(sizeof(os_mbox_t));
    if (!mb) return ERR_MEM;
    mb->head = 0;
    mb->tail = 0;
    mb->count = 0;
    mb->idle_wakeup = 0;
    spin_init(&mb->lock);
    wait_queue_init(&mb->wq);
    wait_queue_init(&mb->not_full_wq);
    if (!g_tcpip_mbox)
        g_tcpip_mbox = mb;
    *mbox = (sys_mbox_t)mb;
    return ERR_OK;
}

void sys_mbox_post(sys_mbox_t *mbox, void *msg)
{
    if (!mbox || !*mbox) return;
    os_mbox_t *mb = (os_mbox_t *)*mbox;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&mb->lock);
        if (mb->count < MBOX_SIZE) {
            mb->queue[mb->head] = msg;
            mb->head = (mb->head + 1) % MBOX_SIZE;
            mb->count++;
            spin_unlock_irqrestore(&mb->lock, flags);
            break;
        }

        uint64_t wf = spin_lock_irqsave(&mb->not_full_wq.lock);
        if (mb->count >= MBOX_SIZE) {
            list_add_to_before(&mb->not_full_wq.head, &current->io_wait_node);
            current->state = TASK_INTERRUPTIBLE;
        }
        spin_unlock_irqrestore(&mb->not_full_wq.lock, wf);
        spin_unlock_irqrestore(&mb->lock, flags);

        if (!list_is_empty(&current->io_wait_node)) {
            schedule();
            arch_local_irq_enable();
            if (!list_is_empty(&current->io_wait_node))
                list_del_init(&current->io_wait_node);
            current->state = TASK_RUNNING;
        }
    }

    wait_queue_wake_one(&mb->wq);
}

// ── Periodic wakeup for mbox_fetch ────────────────────────────
// lwIP's tcpip_thread must periodically wake to process buffered
// RX packets (e1000_process_rx / virtio RX run only in its context).
// A periodic timer posts a sentinel; sys_arch_mbox_fetch consumes it
// internally and re-enters the wait loop.  The sentinel MUST NOT be
// NULL — tcpip_thread asserts on NULL ("invalid message").
#define MBOX_IDLE_WAKEUP_JIFFIES  5   // 50ms (1 jiffy = 10ms)
#define MBOX_SENTINEL             ((void *)0x1)

typedef struct {
    os_mbox_t *mb;
    volatile int cancelled;
} mbox_idle_ctx_t;
/* mbox_idle_ctx_t no longer used: the idle-timer callback now takes
   the mailbox pointer directly (see mbox_idle_callback).  Kept only
   as documentation of the old stack-ctx design. */

static void mbox_idle_callback(void *data)
{
    // data is the mailbox pointer itself — NOT a stack-allocated ctx.
    // The old code passed &ictx (a stack local in mbox_fetch); when the
    // timer fired right as mbox_fetch returned (message arrived), the
    // callback dereferenced a dangling stack pointer (use-after-free),
    // woke the WRONG wait queue, and tcpip_thread slept forever — RX
    // buffers accumulated in rxq, RDH caught up to RDT and the NIC
    // stopped receiving.  Waking the mailbox's queue is harmless even
    // if the fetcher already left (wq empty -> no-op).
    os_mbox_t *mb = (os_mbox_t *)data;
    // Lost-wakeup-safe wake: set the flag BEFORE waking so a fetcher
    // that is between its count-check and its wait-queue enqueue
    // cannot miss this tick.  It checks idle_wakeup under mb->lock
    // before sleeping and, if set, clears it and loops instead of
    // blocking.  (A bare wait_queue_wake_one has a race: if the timer
    // fires just before the fetcher enqueues, the wake is lost and
    // tcpip_thread sleeps forever — RX buffers pile up in rxq, RDH
    // catches up to RDT and the NIC stops receiving.)
    uint64_t _f = spin_lock_irqsave(&mb->lock);
    mb->idle_wakeup = 1;
    spin_unlock_irqrestore(&mb->lock, _f);
    wait_queue_wake_one(&mb->wq);
}

u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **msg, u32_t timeout)
{
    if (!mbox || !*mbox) return SYS_ARCH_TIMEOUT;
    os_mbox_t *mb = (os_mbox_t *)*mbox;

    // Periodic wakeup timer — requests a persistent wake at a fixed interval
    // so the tcpip thread polls and processes RX even while waiting for a
    // message. Timer-driven wakeups never call lwIP from IRQ context.
    timer_t *idle_timer = NULL;
    // create_timer()/init_timer() treat the argument as a RELATIVE
    // jiffies offset (expire = jiffies + arg). Passing an absolute
    // `jiffies + N` double-added jiffies (expire = 2*jiffies + N) so
    // the timer never expired and tcpip_thread slept forever waiting
    // for RX that never got polled. Pass the bare offset.  data is the
    // mailbox pointer itself (stable, global) — see mbox_idle_callback.
    idle_timer = create_timer(mbox_idle_callback, mb,
                              MBOX_IDLE_WAKEUP_JIFFIES);
    if (idle_timer) {
        add_timer(idle_timer);
    }

    u32_t deadline_jiffies = 0;
    if (timeout > 0)
        deadline_jiffies = jiffies + (timeout + 9) / 10;

    for (;;) {
        if (arch_signal_pending_fatal()) {
            destroy_timer(idle_timer);
            return SYS_ARCH_TIMEOUT;
        }
        // The idle timer only fires ONCE (do_timer removes it).  Rebuild
        // it at the top of every loop iteration so a second sleep in
        // this same mbox_fetch call is still woken 50ms later.  Without
        // this, tcpip_thread slept forever after the first wake (its RX
        // polling stopped, the RX queue filled, the NIC stopped
        // receiving, and wget hung waiting for the HTTP body).
        if (idle_timer && !idle_timer->active) {
            while (idle_timer->running)
                __asm__ volatile("pause");
            init_timer(idle_timer, mbox_idle_callback, mb,
                       MBOX_IDLE_WAKEUP_JIFFIES);
            add_timer(idle_timer);
        }
        uint64_t flags = spin_lock_irqsave(&mb->lock);
        if (mb->count > 0) {
            void *m = mb->queue[mb->tail];
            mb->tail = (mb->tail + 1) % MBOX_SIZE;
            mb->count--;
            spin_unlock_irqrestore(&mb->lock, flags);
            wait_queue_wake_one(&mb->not_full_wq);

            destroy_timer(idle_timer);
            *msg = m;
            return 0;
        }
        spin_unlock_irqrestore(&mb->lock, flags);

        if (mb == g_tcpip_mbox) {
            extern void net_poll_rx(void);
            net_poll_rx();
        }
        // Double-check: poll may have posted to this same mailbox
        { uint64_t _f2 = spin_lock_irqsave(&mb->lock);
          if (mb->count > 0) { spin_unlock_irqrestore(&mb->lock, _f2); continue; }
          spin_unlock_irqrestore(&mb->lock, _f2); }

        if (timeout > 0 && jiffies >= deadline_jiffies) {
            destroy_timer(idle_timer);
            return SYS_ARCH_TIMEOUT;
        }

        // Lost-wakeup-safe sleep: enqueue onto the wait queue while
        // holding mb->lock.  sys_mbox_post does count++ under the same
        // lock and then calls wait_queue_wake_one(&mb->wq); because
        // our enqueue is serialized with count++, any wake that
        // happens after our count check is guaranteed to see us on
        // the queue.  The old wait_queue_sleep(&mb->wq) left a window:
        // post's count++ → wake_one could run between our count check
        // and our enqueue, and the wake was lost (tcpip_thread slept
        // forever with a message sitting in the mbox).  The idle-timer
        // wake uses the same lock, but ALSO sets mb->idle_wakeup (see
        // mbox_idle_callback) — check it here to close the timer race.
        {
            uint64_t _f = spin_lock_irqsave(&mb->lock);
            if (mb->count > 0) { spin_unlock_irqrestore(&mb->lock, _f); continue; }
            if (mb->idle_wakeup) { mb->idle_wakeup = 0; spin_unlock_irqrestore(&mb->lock, _f); continue; }
            uint64_t _wf = spin_lock_irqsave(&mb->wq.lock);
            list_add_to_before(&mb->wq.head, &current->io_wait_node);
            current->state = TASK_INTERRUPTIBLE;
            spin_unlock_irqrestore(&mb->wq.lock, _wf);
            spin_unlock_irqrestore(&mb->lock, _f);

            schedule();
            arch_local_irq_enable();

            if (!list_is_empty(&current->io_wait_node))
                list_del_init(&current->io_wait_node);
            current->state = TASK_RUNNING;
        }
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
        wait_queue_wake_one(&mb->not_full_wq);
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
    t->state = TASK_RUNNING;
    t->counter = t->priority;
    kfree(name_copy);
    return (sys_thread_t)(uintptr_t)t;
}

// ═══════════════════════════════════════════════════════════════
//  Protection — recursive IRQ-save spinlock
// ═══════════════════════════════════════════════════════════════

static spinlock_T  lwip_global_lock = { .lock = 1 };
static volatile int protect_nest[NR_CPUS];
static uint64_t     protect_flags[NR_CPUS];

sys_prot_t sys_arch_protect(void)
{
    unsigned int cpu = (unsigned int)cpu_id();
    if (protect_nest[cpu] == 0) {
        protect_flags[cpu] = spin_lock_irqsave(&lwip_global_lock);
    }
    protect_nest[cpu]++;
    return protect_flags[cpu];
}

void sys_arch_unprotect(sys_prot_t pval)
{
    (void)pval;
    unsigned int cpu = (unsigned int)cpu_id();
    if (protect_nest[cpu] <= 0) return;
    if (--protect_nest[cpu] == 0)
        spin_unlock_irqrestore(&lwip_global_lock, protect_flags[cpu]);
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
