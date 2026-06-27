#ifndef _KERNEL_TTY_H
#define _KERNEL_TTY_H

#include <stdint.h>
#include <stdbool.h>
#include <list.h>
#include <kernel/arch/x86_64/spinlock.h>

#define TTY_BUF_SIZE  256

// ── Line discipline flags ──────────────────────────
#define TTY_L_ICANON  (1 << 0)   // canonical mode (line buffering)
#define TTY_L_ECHO    (1 << 1)   // echo input characters
#define TTY_L_ISIG    (1 << 2)   // generate signals (^C → SIGINT)

// ── TTY structure ──────────────────────────────────
typedef struct tty_struct {
    // ── Input ring buffer ───────────────────────
    // Producer: IRQ context (keyboard handler), task context (tty_read poll)
    // Consumer: task context (tty_read)
    spinlock_T  cooked_lock;
    char        cooked[TTY_BUF_SIZE];
    volatile int   head;           // producer index
    volatile int   tail;           // consumer index

    // ── Canonical line buffer ───────────────────
    char        line[TTY_BUF_SIZE];
    int         line_len;          // bytes in the completed line
    int         read_pos;          // bytes already consumed by tty_read
    bool        line_ready;        // true when line is \n-terminated

    // ── Line discipline ─────────────────────────
    uint8_t     lflag;             // TTY_L_ICANON | TTY_L_ECHO | TTY_L_ISIG

    // ── Read wait queue ─────────────────────────
    // Tasks blocked in tty_read() wait here.
    list_t      read_wait;

    // ── Output callbacks ────────────────────────
    void (*output_char)(char c);   // primary output
    void (*echo_char)(char c);     // echo output (usually same as output_char)

    // ── Foreground process group ────────────────
    int64_t     pgrp;              // PID that receives ^C-generated SIGINT
} tty_t;

// ── API ────────────────────────────────────────────

// Allocate and initialize a TTY.  output_char and echo_char
// may be the same function (dual-write to fb+serial).
tty_t *tty_alloc(void (*output_char)(char), void (*echo_char)(char));

// Push one character into the TTY input buffer.
// Called from IRQ context (keyboard / serial handlers).
// Wakes any task blocked in tty_read().
void tty_push_input(tty_t *tty, char c);

// Read up to `size` bytes from the TTY.  In canonical mode,
// blocks until a complete line is available (terminated by \n).
// Returns number of bytes copied, 0 for EOF, or -EINTR if
// interrupted by a signal.
int tty_read(tty_t *tty, char *buf, int size, bool nonblock);

// Write `size` bytes to the TTY output.  Goes to both
// output_char and echo_char callbacks.
int tty_write(tty_t *tty, const char *buf, int size);

#endif
