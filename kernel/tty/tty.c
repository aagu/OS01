#include <kernel/tty.h>
#include <kernel/task.h>
#include <kernel/printk.h>
#include <kernel/percpu.h>
#include <driver/serial.h>
#include <driver/keyboard.h>
#include <kernel.h>
#include <kernel/poll.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <termios.h>
#include <uapi/stat.h>

// ═══════════════════════════════════════════════════════
//  Internal helpers
// ═══════════════════════════════════════════════════════

static inline bool tty_ring_empty(tty_t *tty)
{
    return tty->head == tty->tail;
}

static inline bool tty_ring_full(tty_t *tty)
{
    int next = (tty->head + 1) % TTY_BUF_SIZE;
    return next == tty->tail;
}

static bool tty_ring_push(tty_t *tty, char c)
{
    uint64_t flags = spin_lock_irqsave(&tty->ring_lock);

    bool ok = false;
    if (!tty_ring_full(tty)) {
        tty->ring[tty->head] = c;
        tty->head = (tty->head + 1) % TTY_BUF_SIZE;
        ok = true;
    }

    spin_unlock_irqrestore(&tty->ring_lock, flags);
    return ok;
}

// ═══════════════════════════════════════════════════════
//  Wake waiters (IRQ path)
// ═══════════════════════════════════════════════════════

static void tty_wake_waiters(tty_t *tty)
{
    // 1. Wake direct blocking reader tasks (tty_read path)
    {
        uint64_t flags = spin_lock_irqsave(&tty->read_wait_lock);
        while (!list_is_empty(&tty->read_wait)) {
            list_t *node = tty->read_wait.next;
            list_del_init(node);
            task_t *t = container_of(node, task_t, io_wait_node);
            task_wake(t);
        }
        spin_unlock_irqrestore(&tty->read_wait_lock, flags);
    }

    // 2. Cascade-wake all poll waiters (fd_poll path)
    // read_poll is protected by ring_lock (see poll_wait in tty_poll).
    {
        uint64_t flags = spin_lock_irqsave(&tty->ring_lock);
        while (!list_is_empty(&tty->read_poll)) {
            list_t *node = tty->read_poll.next;
            list_del_init(node);
            poll_wait_entry_t *e = container_of(node, poll_wait_entry_t, node);
            wait_queue_wake_all(e->poll_wq);
        }
        spin_unlock_irqrestore(&tty->ring_lock, flags);
    }

    // Notify the scheduler that a task was woken — otherwise the
    // current CPU may sit in hlt (idle) until the next timer tick.
    this_cpu()->need_resched = 1;
}

// ═══════════════════════════════════════════════════════
//  Output helpers
// ═══════════════════════════════════════════════════════

static void tty_def_output(char c)
{
    color_printk(WHITE, BLACK, "%c", c);
    write_serial(c);
}

// ═══════════════════════════════════════════════════════
//  Allocate / init
// ═══════════════════════════════════════════════════════

tty_t *tty_alloc(void (*output_char)(char), void (*echo_char)(char))
{
    tty_t *tty = (tty_t *)calloc(1, sizeof(tty_t));
    if (!tty) return NULL;

    tty->head = 0;
    tty->tail = 0;
    list_init(&tty->read_wait);
    list_init(&tty->read_poll);
    spin_init(&tty->read_wait_lock);
    spin_init(&tty->ring_lock);

    tty->output_char = output_char ? output_char : tty_def_output;
    tty->echo_char   = echo_char   ? echo_char   : tty->output_char;

    return tty;
}

// ═══════════════════════════════════════════════════════
//  Push input from IRQ context (keyboard or serial)
// ═══════════════════════════════════════════════════════

void tty_push_input(tty_t *tty, char c)
{
    if (!tty)
        return;
    if (!tty_ring_push(tty, c))
        return;
    tty_wake_waiters(tty);
}

// ═══════════════════════════════════════════════════════
//  tty_read — blocking read from raw ring buffer
// ═══════════════════════════════════════════════════════
//
//  Blocking protocol (prevents lost wakeup):
//    1. Drain ring buffer directly
//    2. Set INTERRUPTIBLE, enqueue, double-check
//    3. If still empty: schedule() — sleeps until tty_wake_waiters()
//    4. On wake: dequeue self, loop back to Phase 1

int tty_read(tty_t *tty, char *buf, int size, bool nonblock)
{
    if (!tty || !buf || size <= 0)
        return 0;

    for (;;) {
        // ── Phase 1: drain the ring buffer directly ──────────
        uint64_t flags = spin_lock_irqsave(&tty->ring_lock);
        int n = 0;
        while (n < size && tty->head != tty->tail) {
            buf[n++] = tty->ring[tty->tail];
            tty->tail = (tty->tail + 1) % TTY_BUF_SIZE;
        }
        spin_unlock_irqrestore(&tty->ring_lock, flags);

        if (n > 0)
            return n;

        if (nonblock)
            return 0;

        // ── Signal check before blocking sleep ───────────────
        if (signal_pending_fatal())
            return 0;

        // ── Phase 2: blocking sleep on wait queue ──────────
        {
            uint64_t wq_flags = spin_lock_irqsave(&tty->read_wait_lock);
            current->state = TASK_INTERRUPTIBLE;
            list_add_to_before(&tty->read_wait, &current->io_wait_node);

            // Double-check: IRQ may have fired on another CPU
            if (tty->head != tty->tail) {
                list_del_init(&current->io_wait_node);
                current->state = TASK_RUNNING;
                spin_unlock_irqrestore(&tty->read_wait_lock, wq_flags);
                continue;
            }
            spin_unlock_irqrestore(&tty->read_wait_lock, wq_flags);
        }

        schedule();

        do_signal_delivery(NULL);

        if (signal_pending_fatal()) {
            uint64_t wq_flags = spin_lock_irqsave(&tty->read_wait_lock);
            if (!list_is_empty(&current->io_wait_node))
                list_del_init(&current->io_wait_node);
            spin_unlock_irqrestore(&tty->read_wait_lock, wq_flags);
            return 0;
        }

        {
            uint64_t wq_flags = spin_lock_irqsave(&tty->read_wait_lock);
            if (!list_is_empty(&current->io_wait_node))
                list_del_init(&current->io_wait_node);
            spin_unlock_irqrestore(&tty->read_wait_lock, wq_flags);
        }
    }
}

// ═══════════════════════════════════════════════════════
//  tty_write — dual output (fb + serial)
// ═══════════════════════════════════════════════════════

int tty_write(tty_t *tty, const char *buf, int size)
{
    if (!tty || !buf || size <= 0)
        return 0;

    for (int i = 0; i < size; i++) {
        tty->output_char(buf[i]);
    }
    return size;
}

// ── tty_poll — check TTY readiness ───────────────────────
// TTY is always writable.  Readable if ring buffer
// has data.  If not ready and pt is provided, register a
// poll_wait_entry on tty->read_poll for cascade wake when
// tty_push_input() → tty_wake_waiters() fires.

uint32_t tty_poll(tty_t *tty, poll_table_t *pt)
{
    uint32_t mask = 0;

    // TTY output is always ready
    mask |= POLLOUT | POLLWRNORM;

    // Check ring buffer
    uint64_t flags = spin_lock_irqsave(&tty->ring_lock);
    if (tty->head != tty->tail) {
        mask |= POLLIN | POLLRDNORM;
    } else if (pt && !pt->triggered) {
        poll_wait(pt, &tty->read_poll, &tty->ring_lock);
    }
    spin_unlock_irqrestore(&tty->ring_lock, flags);

    return mask;
}

// ── tty_phys_ioctl — physical TTY ioctl callback for devfs_ops.ioctl ──
// Handles TCGETS/TCSETS/TIOCGWINSZ/TIOCGPGRP/FIONREAD.

int tty_phys_ioctl(struct vfs_node *node, int cmd, void *arg)
{
    (void)node;
    switch (cmd) {
    case TCGETS: {
        struct termios t;
        memset(&t, 0, sizeof(t));
        t.c_lflag = ICANON | ECHO | ISIG;
        t.c_iflag = ICRNL;
        t.c_oflag = OPOST | ONLCR;
        memcpy(arg, &t, sizeof(t));
        return 0;
    }
    case TCSETS: case TCSETSW: return 0;  // store only, no-op
    case TIOCGWINSZ:
        ((struct winsize *)arg)->ws_row = 25;
        ((struct winsize *)arg)->ws_col = 80;
        return 0;
    case TIOCGPGRP: *(pid_t *)arg = 0; return 0;
    case TIOCSPGRP: return 0;
    case FIONREAD: {
        tty_t *tty = get_dev_tty();
        *(int *)arg = tty ? (tty->head - tty->tail + TTY_BUF_SIZE) % TTY_BUF_SIZE : 0;
        return 0;
    }
    default: return -ENOTTY;
    }
}

// ── Console TTY singleton ────────────────────────
// Set by main.c during init, consumed by dev_tty_read/write.

static tty_t *dev_tty = NULL;

void tty_set_dev_tty(tty_t *tty)
{
    dev_tty = tty;
}

tty_t *get_dev_tty(void)
{
    return dev_tty;
}
