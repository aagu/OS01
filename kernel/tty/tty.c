#include <kernel/tty.h>
#include <kernel/task.h>
#include <kernel/printk.h>
#include <driver/serial.h>
#include <driver/keyboard.h>
#include <kernel.h>
#include <kernel/arch/x86_64/hw.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

// ═══════════════════════════════════════════════════════
//  Internal helpers
// ═══════════════════════════════════════════════════════

static inline bool tty_cooked_empty(tty_t *tty)
{
    return tty->head == tty->tail;
}

static inline bool tty_cooked_full(tty_t *tty)
{
    int next = (tty->head + 1) % TTY_BUF_SIZE;
    return next == tty->tail;
}

static bool tty_cooked_push(tty_t *tty, char c)
{
    uint64_t flags = spin_lock_irqsave(&tty->cooked_lock);

    bool ok = false;
    if (!tty_cooked_full(tty)) {
        tty->cooked[tty->head] = c;
        tty->head = (tty->head + 1) % TTY_BUF_SIZE;
        ok = true;
    }

    spin_unlock_irqrestore(&tty->cooked_lock, flags);
    return ok;
}

static bool tty_cooked_pop(tty_t *tty, char *c)
{
    uint64_t flags = spin_lock_irqsave(&tty->cooked_lock);

    bool ok = false;
    if (!tty_cooked_empty(tty)) {
        *c = tty->cooked[tty->tail];
        tty->tail = (tty->tail + 1) % TTY_BUF_SIZE;
        ok = true;
    }

    spin_unlock_irqrestore(&tty->cooked_lock, flags);
    return ok;
}

// ═══════════════════════════════════════════════════════
//  Wake waiters (IRQ path)
// ═══════════════════════════════════════════════════════

static void tty_wake_waiters(tty_t *tty)
{
    while (!list_is_empty(&tty->read_wait)) {
        list_t *node = tty->read_wait.next;
        list_del_init(node);
        task_t *t = container_of(node, task_t, io_wait_node);
        t->state = TASK_RUNNING;
    }
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
    tty->line_len = 0;
    tty->read_pos = 0;
    tty->lflag = TTY_L_ICANON | TTY_L_ECHO | TTY_L_ISIG;
    tty->pgrp = 0;
    list_init(&tty->read_wait);
    spin_init(&tty->cooked_lock);

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
    if (!tty_cooked_push(tty, c))
        return;
    tty_wake_waiters(tty);
}

// ═══════════════════════════════════════════════════════
//  Canonical mode processing
// ═══════════════════════════════════════════════════════

static int tty_canon_process(tty_t *tty, char c)
{
    if (c == '\n' || c == '\r') {
        tty->line[tty->line_len++] = '\n';
        tty->line[tty->line_len] = '\0';
        tty->line_ready = true;
        if (tty->lflag & TTY_L_ECHO) {
            tty->echo_char('\r');
            tty->echo_char('\n');
        }
        return tty->line_len;
    }

    if (c == '\b' || c == 0x7F) {
        if (tty->line_len > 0) {
            tty->line_len--;
            if (tty->lflag & TTY_L_ECHO) {
                tty->echo_char('\b');
                tty->echo_char(' ');
                tty->echo_char('\b');
            }
        }
        return -1;
    }

    if (c == 0x03 && (tty->lflag & TTY_L_ISIG)) {
        tty->line_len = 0;
        tty->line_ready = false;
        if (tty->lflag & TTY_L_ECHO) {
            tty->echo_char('^');
            tty->echo_char('C');
            tty->echo_char('\r');
            tty->echo_char('\n');
        }
        return -1;
    }

    if (c == 0x04 && tty->line_len == 0)
        return 0;

    if (c >= ' ') {
        if (tty->line_len < TTY_BUF_SIZE - 1) {
            tty->line[tty->line_len++] = c;
            if (tty->lflag & TTY_L_ECHO)
                tty->echo_char(c);
        }
    }

    return -1;
}

// ═══════════════════════════════════════════════════════
//  tty_read — blocking read with canonical processing
// ═══════════════════════════════════════════════════════
//
//  Blocking protocol (prevents lost wakeup):
//    1. Drain cooked ring
//    2. Poll hardware once
//    3. If no data: set INTERRUPTIBLE, enqueue, double-check
//    4. If still empty: schedule() — sleeps until tty_wake_waiters()
//    5. On wake: dequeue self, poll hardware once (IRQ fallback), loop

int tty_read(tty_t *tty, char *buf, int size, bool nonblock)
{
    if (!tty || !buf || size <= 0)
        return 0;

    bool canonical = (tty->lflag & TTY_L_ICANON) != 0;

    for (;;) {
        // ── Return pending partial line from previous read ──
        if (canonical && tty->line_ready && tty->read_pos < tty->line_len) {
            int avail = tty->line_len - tty->read_pos;
            int n = (avail < size) ? avail : size;
            memcpy(buf, tty->line + tty->read_pos, n);
            tty->read_pos += n;
            if (tty->read_pos >= tty->line_len) {
                tty->line_len = 0;
                tty->read_pos = 0;
                tty->line_ready = false;
            }
            return n;
        }

        // ── Phase 1: drain the cooked ring buffer ──────────
        char c;
        while (tty_cooked_pop(tty, &c)) {
            if (canonical) {
                int ret = tty_canon_process(tty, c);
                if (ret >= 0) {
                    tty->read_pos = 0;
                    int n = (ret < size) ? ret : size;
                    memcpy(buf, tty->line, n);
                    tty->read_pos = n;
                    if (n >= ret) {
                        tty->line_len = 0;
                        tty->read_pos = 0;
                        tty->line_ready = false;
                    }
                    return n;
                }
                continue;
            } else {
                buf[0] = c;
                return 1;
            }
        }

        if (nonblock)
            return 0;

        // ── Phase 2: poll hardware once before sleeping ────
        keyboard_poll();
        if (!tty_cooked_empty(tty))
            continue;

        // ── Phase 3: blocking sleep on wait queue ──────────
        // Set INTERRUPTIBLE FIRST, then enqueue.  If an IRQ fires
        // between these two steps, the waker won't find us on
        // read_wait (no-op), but our double-check catches the data.
        // If the IRQ fires after enqueue, the waker removes us and
        // sets RUNNING — then either the double-check finds data
        // or schedule() returns immediately (state==RUNNING).
        current->state = TASK_INTERRUPTIBLE;
        list_add_to_before(&tty->read_wait, &current->io_wait_node);

        // Double-check: IRQ may have fired during the steps above.
        if (!tty_cooked_empty(tty)) {
            list_del_init(&current->io_wait_node);
            current->state = TASK_RUNNING;
            continue;
        }

        // Truly sleep — woken by tty_wake_waiters() from IRQ
        // context when new input arrives.
        schedule();

        // tty_wake_waiters() calls list_del_init on our node,
        // leaving io_wait_node self-pointing.  If we were woken
        // by something else, clean up.
        if (!list_is_empty(&current->io_wait_node))
            list_del_init(&current->io_wait_node);

        // Poll hardware once after waking — fallback if IRQs
        // are not delivering input (e.g. wrong IOAPIC routing).
        keyboard_poll();
    }
}

// ═══════════════════════════════════════════════════════
//  tty_write — dual output (fb + serial)
// ═══════════════════════════════════════════════════════

int tty_write(tty_t *tty, const char *buf, int size)
{
    if (!tty || !buf || size <= 0)
        return 0;

    for (int i = 0; i < size; i++)
        tty->output_char(buf[i]);
    return size;
}
