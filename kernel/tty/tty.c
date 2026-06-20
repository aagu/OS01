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

// Push one byte into the cooked ring buffer.
// Called from IRQ context (producer) — one writer only.
static bool tty_cooked_push(tty_t *tty, char c)
{
    if (tty_cooked_full(tty))
        return false;
    tty->cooked[tty->head] = c;
    __sync_synchronize();
    tty->head = (tty->head + 1) % TTY_BUF_SIZE;
    return true;
}

// Pop one byte from the cooked ring buffer.
// Called from task context (consumer) — one reader only.
static bool tty_cooked_pop(tty_t *tty, char *c)
{
    if (tty_cooked_empty(tty))
        return false;
    *c = tty->cooked[tty->tail];
    __sync_synchronize();
    tty->tail = (tty->tail + 1) % TTY_BUF_SIZE;
    return true;
}

// ═══════════════════════════════════════════════════════
//  Wait queue helpers
// ═══════════════════════════════════════════════════════
// Protocol: task sets itself INTERRUPTIBLE, adds its
// io_wait_node to the TTY wait list, THEN double-checks
// the buffer.  This ordering prevents the classic lost-
// wakeup race:
//
//   WRONG:  check buffer → add to wait → sleep
//           ^ IRQ between check and add = lost forever
//   RIGHT:  add to wait → check buffer → sleep
//           ^ IRQ after add but before check → wakeup
//             sets state RUNNING, check finds data,
//             schedule() returns immediately (counter>0).

static void tty_wait_enqueue(tty_t *tty)
{
    current->state = TASK_INTERRUPTIBLE;
    // io_wait_node may be {NULL,NULL} from memset on first use —
    // list_add_to_before overwrites both pointers before they're
    // read by anything, so this is safe.
    list_add_to_before(&tty->read_wait, &current->io_wait_node);
}

// Remove current task from the TTY wait list.
// Safe to call even if already removed by tty_wake_waiters() —
// list_del_init is a no-op on a self-pointing node.
static void tty_wait_dequeue(void)
{
    if (!list_is_empty(&current->io_wait_node))
        list_del_init(&current->io_wait_node);
}

// Wake all tasks blocked on this TTY and remove them
// from the wait queue.  Called from IRQ context.
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

// Default output: write to BOTH framebuffer and serial.
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
// Returns -1 if more characters are needed, 0 for EOF,
// or >0 for number of bytes accumulated in tty->line.

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

    return -1;  // need more characters
}

// ═══════════════════════════════════════════════════════
//  tty_read — blocking read with canonical processing
// ═══════════════════════════════════════════════════════

int tty_read(tty_t *tty, char *buf, int size, bool nonblock)
{
    if (!tty || !buf || size <= 0)
        return 0;

    static int rcall = 0;
    rcall++;

    bool canonical = (tty->lflag & TTY_L_ICANON) != 0;

    for (;;) {
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
            // lock-free serial debug so we can see reads even
            // when serial_lock is held by someone else
            for (int di = 0; di < n; di++) {
                char dc = ((char*)buf)[di];
                while (!(inb(0x3F8 + 5) & 0x20)) __asm__("pause");
                outb(0x3F8, dc == '\n' ? 'N' : (dc >= 32 && dc < 127) ? dc : '?');
            }
            while (!(inb(0x3F8 + 5) & 0x20)) __asm__("pause");
            outb(0x3F8, '>');
            return n;
        }

        // Phase 1: process characters from the cooked ring.
        // In canonical mode, characters accumulate in the line
        // buffer until \n arrives (tty_canon_process sets
        // line_ready=true).  Only then do we return data.
        char c;
        while (tty_cooked_pop(tty, &c)) {
            if (canonical) {
                int ret = tty_canon_process(tty, c);
                if (ret >= 0) {
                    // Line complete — copy up to `size` bytes.
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
                // ret == -1: line not yet complete.
                // Stay in Phase 1 — drain more chars from cooked ring.
                continue;
            } else {
                buf[0] = c;
                return 1;
            }
        }

        // No data available.
        if (nonblock)
            return 0;

        // Phase 2a: poll hardware directly.
        // This is the primary input path until keyboard/serial
        // IRQs are confirmed working.  The IRQ handlers also
        // call tty_push_input() — once IRQs are up, the spin
        // loop overhead is negligible because it only runs
        // between keystrokes.
        keyboard_poll();

        // Drain serial port (terminal input via -serial stdio)
        while (serial_received()) {
            char sc = read_serial();
            tty_push_input(tty, sc);
        }

        // If polling produced data, loop back immediately.
        if (!tty_cooked_empty(tty))
            continue;

        // No data yet — spin-poll with occasional yield.
        for (volatile int spin = 0; spin < 500; spin++)
            __asm__ __volatile__("pause");
        schedule();

        continue;
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
