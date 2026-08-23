#include <kernel/tty.h>
#include <kernel/arch/irq.h>
#include <kernel/task.h>
#include <kernel/printk.h>
#include <kernel/log.h>
#include <kernel/percpu.h>
#include <driver/serial.h>
#include <driver/keyboard.h>
#include <kernel.h>
#include <kernel/poll.h>
#include <kernel/uaccess.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <termios.h>
#include <uapi/stat.h>

// Local: NOT yet in libc/include/termios.h. 0 == "special char disabled".
#define _POSIX_VDISABLE 0

// ═══════════════════════════════════════════════════════
//  Internal helpers
// ═══════════════════════════════════════════════════════

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
    tty->canon.len = 0;
    memset(&tty->term, 0, sizeof(struct termios));
    tty->term.c_iflag = ICRNL;
    tty->term.c_oflag = OPOST | ONLCR;
    tty->term.c_lflag = ISIG;            // default: raw + signal-aware
    tty->term.c_cc[VMIN]   = 1;
    tty->term.c_cc[VTIME]  = 0;
    // Default special chars: VINTR=3 must be explicit or the line
    // discipline in tty_push_input never fires.
    tty->term.c_cc[VINTR]  = 3;          // Ctrl-C
    tty->term.c_cc[VQUIT]  = 28;         // Ctrl-\ (SIGQUIT)
    tty->term.c_cc[VERASE] = 127;        // DEL
    tty->term.c_cc[VKILL]  = 21;         // Ctrl-U
    tty->term.c_cc[VEOF]   = 4;          // Ctrl-D
    // VSUSP / VSTART / VSTOP stay 0 = _POSIX_VDISABLE
    // (VSTART/VSTOP flow control is a later tier).
    list_init(&tty->read_wait);
    list_init(&tty->read_poll);
    spin_init(&tty->read_wait_lock);
    spin_init(&tty->ring_lock);
    tty->fg_pgrp = 0;
    spin_init(&tty->fg_pgrp_lock);

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

    // ── Line discipline: VINTR / VQUIT / VSUSP → signal ──
    // _POSIX_VDISABLE = 0 means "this special char is disabled".
    // Must run BEFORE echo/ring push: signal chars are neither
    // echoed nor queued to the read side.
    if (tty->term.c_lflag & ISIG) {
        cc_t vintr = tty->term.c_cc[VINTR];
        if (vintr != 0 && c == vintr) {
            uint64_t f = spin_lock_irqsave(&tty->fg_pgrp_lock);
            pid_t pg = tty->fg_pgrp;
            spin_unlock_irqrestore(&tty->fg_pgrp_lock, f);
            if (pg != 0) signal_pgrp(pg, SIGINT);
            return;
        }
        cc_t vquit = tty->term.c_cc[VQUIT];
        if (vquit != 0 && c == vquit) {
            uint64_t f = spin_lock_irqsave(&tty->fg_pgrp_lock);
            pid_t pg = tty->fg_pgrp;
            spin_unlock_irqrestore(&tty->fg_pgrp_lock, f);
            if (pg != 0) signal_pgrp(pg, SIGQUIT);
            return;
        }
        cc_t vsusp = tty->term.c_cc[VSUSP];
        if (vsusp != 0 && c == vsusp) {
            // SIGTSTP not implemented: just drop the char.
            log_debug("tty: VSUSP char dropped (SIGTSTP not implemented)\n");
            return;
        }
    }

    // Echo in canonical+echo mode (input-time echo, typewriter style)
    if ((tty->term.c_lflag & ICANON) && (tty->term.c_lflag & ECHO)) {
        if (c == '\n' && (tty->term.c_oflag & ONLCR))
            tty->echo_char('\r');
        tty->echo_char(c);
    }

    if (!tty_ring_push(tty, c))
        return;
    tty_wake_waiters(tty);
}

// ═══════════════════════════════════════════════════════
//  tty_read — blocking read from raw ring buffer
// ═══════════════════════════════════════════════════════
//
//  Blocking protocol (prevents lost wakeup):
//    1. Drain ring buffer into kernel bounce (NO tail/canon advance)
//    2. copy_to_user_ft bounce→user buf
//    3. On _ft success: advance tail/canon (commit)
//    4. On _ft fault: return -1 (data still in ring/canon for retry)
//    5. If drain returned 0 bytes and we should block: schedule()
//    6. On wake: dequeue self, loop back to Step 1
//
//  Task 8 (Cat C): "post-block drain via _ft".  The user pointer is
//  only ever touched by copy_to_user_ft (released lock, no spinlock
//  held, no resource requiring _ft_res cleanup).

int tty_read(tty_t *tty, char *buf, int size, bool nonblock)
{
    if (!tty || !buf || size <= 0)
        return 0;

    for (;;) {
        // ── Phase 1: drain ring/canon → kernel bounce (no commit) ─
        // We stage the bytes under ring_lock but do NOT advance
        // tail / canon yet — that happens after copy_to_user_ft
        // succeeds.  This is the "peek" half of the tty equivalent
        // of pipe_read_internal's 3-phase.
        char kbuf[TTY_BUF_SIZE];
        int n = 0;
        bool canonical = (tty->term.c_lflag & ICANON) != 0;

        {
            uint64_t flags = spin_lock_irqsave(&tty->ring_lock);

            if (canonical) {
                // Move ring → canon buffer (only mutates canon.len
                // + ring tail, NOT user-facing state).
                canon_accumulate(&tty->canon, tty->ring,
                                 &tty->head, &tty->tail, TTY_BUF_SIZE);
                // Find the first '\n' boundary; copy that slice
                // into kbuf but DO NOT advance canon.len yet.
                int i;
                for (i = 0; i < tty->canon.len; i++) {
                    if (tty->canon.buf[i] == '\n')
                        break;
                }
                if (i < tty->canon.len) {
                    int copy = (i + 1) < size ? (i + 1) : size;
                    memcpy(kbuf, tty->canon.buf, copy);
                    n = copy;
                }
            } else {
                // Raw mode: copy bytes ring→kbuf up to size, but DO
                // NOT advance tail yet.
                int avail = (tty->head - tty->tail + TTY_BUF_SIZE) % TTY_BUF_SIZE;
                int want = (avail < size) ? avail : size;
                if (want > 0) {
                    // Handle ring wrap
                    if (tty->tail + want <= TTY_BUF_SIZE) {
                        memcpy(kbuf, tty->ring + tty->tail, want);
                    } else {
                        int first = TTY_BUF_SIZE - tty->tail;
                        memcpy(kbuf, tty->ring + tty->tail, first);
                        memcpy(kbuf + first, tty->ring, want - first);
                    }
                    n = want;
                }
            }

            spin_unlock_irqrestore(&tty->ring_lock, flags);
        }

        if (n > 0) {
            // ── Phase 2: _ft copy kbuf→user ────────────────────
            ssize_t rc = copy_to_user_ft(buf, kbuf, (size_t)n);
            if (rc < 0) {
                // Fault: do NOT advance ring tail / canon state.
                // Data stays in tty buffer for retry on next call.
                return -1;
            }
            // ── Phase 3: commit (advance ring tail / canon) ─────
            uint64_t flags = spin_lock_irqsave(&tty->ring_lock);
            if (canonical) {
                memmove(tty->canon.buf, tty->canon.buf + n,
                        tty->canon.len - n);
                tty->canon.len -= n;
            } else {
                tty->tail = (tty->tail + n) % TTY_BUF_SIZE;
            }
            spin_unlock_irqrestore(&tty->ring_lock, flags);
            return n;
        }

        if (nonblock)
            return 0;

        // ── Signal check before blocking sleep ───────────────
        if (arch_signal_pending_fatal())
            return 0;

        // ── Phase 2 (block path): blocking sleep on wait queue ──
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
        arch_local_irq_enable();

        arch_do_signal_delivery(NULL);

        if (arch_signal_pending_fatal()) {
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

    uint64_t sf = spin_lock_irqsave(&serial_lock);
    for (int i = 0; i < size; i++) {
        tty->output_char(buf[i]);
    }
    spin_unlock_irqrestore(&serial_lock, sf);
    return size;
}

// ── tty_poll — check TTY readiness ───────────────────────
// TTY is always writable.  Readable if ring buffer
// has data.  If not ready and pt is provided, register a
// poll_wait_entry on tty->read_poll for cascade wake when
// tty_push_input() → tty_wake_waiters() fires.

uint32_t tty_poll(tty_t *tty, uint32_t requested, poll_table_t *pt)
{
    uint32_t mask = 0;

    // TTY output is always ready
    mask |= POLLOUT | POLLWRNORM;

    // Check ring buffer
    uint64_t flags = spin_lock_irqsave(&tty->ring_lock);
    if (tty->head != tty->tail) {
        mask |= POLLIN | POLLRDNORM;
    } else if (poll_requested_read(requested) && pt && !pt->triggered) {
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
        // Write direction: kernel termios → user struct.
        tty_t *tty = get_dev_tty();
        if (!tty) return -ENODEV;
        if (!arg) return -EFAULT;
        if (!syscall_check_user_range((uint64_t)arg,
                                      sizeof(struct termios), true))
            return -EFAULT;
        ssize_t r = copy_to_user_ft(arg, &tty->term, sizeof(struct termios));
        if (r < 0) return -EFAULT;
        return 0;
    }
    case TCSETS:
    case TCSETSW: {
        // Read direction: user termios → kernel termios.
        tty_t *tty = get_dev_tty();
        if (!tty) return -ENODEV;
        if (!arg) return -EFAULT;
        if (!syscall_check_user_range((uint64_t)arg,
                                      sizeof(struct termios), false))
            return -EFAULT;
        struct termios kterm;
        if (copy_from_user_ft(&kterm, arg, sizeof(kterm)) < 0)
            return -EFAULT;
        tty->term = kterm;
        return 0;
    }
    case TIOCGWINSZ:
        if (!arg) return -EFAULT;
        if (!syscall_check_user_range((uint64_t)arg,
                                      sizeof(struct winsize), true))
            return -EFAULT;
        struct winsize kws = { .ws_row = 25, .ws_col = 80 };
        if (copy_to_user_ft(arg, &kws, sizeof(kws)) < 0)
            return -EFAULT;
        return 0;
    case TIOCGPGRP: {
        tty_t *tty = get_dev_tty();
        if (!tty) return -ENODEV;
        pid_t *p = (pid_t *)arg;
        if (!p) return -EFAULT;
        if (!syscall_check_user_range((uint64_t)p, sizeof(pid_t), true))
            return -EFAULT;
        uint64_t f = spin_lock_irqsave(&tty->fg_pgrp_lock);
        pid_t kp = tty->fg_pgrp;
        spin_unlock_irqrestore(&tty->fg_pgrp_lock, f);
        if (copy_to_user_ft(p, &kp, sizeof(kp)) < 0)
            return -EFAULT;
        return 0;
    }
    case TIOCSPGRP: {
        tty_t *tty = get_dev_tty();
        if (!tty) return -ENODEV;
        pid_t *p = (pid_t *)arg;
        if (!p) return -EFAULT;
        if (!syscall_check_user_range((uint64_t)p, sizeof(pid_t), false))
            return -EFAULT;
        pid_t new_pg;
        if (copy_from_user_ft(&new_pg, p, sizeof(new_pg)) < 0)
            return -EFAULT;
        if (new_pg < 0) return -EINVAL;
        // v4 放宽：new_pg == 0 OR new_pg exists in caller's session
        if (new_pg != 0 && new_pg != current->pgrp) {
            uint64_t f2 = spin_lock_irqsave(&task_list_lock);
            bool found = false;
            list_t *pos3 = init_task_union.task.list.next;
            while (pos3 != &init_task_union.task.list) {
                task_t *t3 = container_of(pos3, task_t, list);
                pos3 = task_list_next(pos3);
                if (t3->pgrp == new_pg && t3->session == current->session) {
                    found = true; break;
                }
            }
            spin_unlock_irqrestore(&task_list_lock, f2);
            if (!found) return -EPERM;
        }
        uint64_t f = spin_lock_irqsave(&tty->fg_pgrp_lock);
        tty->fg_pgrp = new_pg;
        spin_unlock_irqrestore(&tty->fg_pgrp_lock, f);
        return 0;
    }
    case FIONREAD: {
        tty_t *tty = get_dev_tty();
        if (!arg) return -EFAULT;
        if (!syscall_check_user_range((uint64_t)arg, sizeof(int), true))
            return -EFAULT;
        int avail = tty ? (tty->head - tty->tail + TTY_BUF_SIZE) % TTY_BUF_SIZE : 0;
        if (copy_to_user_ft(arg, &avail, sizeof(avail)) < 0)
            return -EFAULT;
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
