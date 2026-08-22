#ifndef _KERNEL_TTY_H
#define _KERNEL_TTY_H

#include <stdint.h>
#include <stdbool.h>
#include <list.h>
#include <kernel/arch/spinlock.h>
#include <kernel/canon.h>
typedef int pid_t;  /* for termios.h userspace declarations */

struct poll_table;  /* forward declaration — full definition in kernel/poll.h */
struct vfs_node;    /* forward declaration — full definition in fs/vfs.h */
#include <termios.h>

#define TTY_BUF_SIZE  256

// ── TTY structure ──────────────────────────────────
typedef struct tty_struct {
    // ── Input ring buffer ───────────────────────
    // Producer: IRQ context (keyboard handler), task context (tty_read poll)
    // Consumer: task context (tty_read)
    spinlock_T  ring_lock;
    char        ring[TTY_BUF_SIZE];
    volatile int   head;           // producer index
    volatile int   tail;           // consumer index

    // ── Canonical line discipline (ICANON mode) ──
    // Pure-logic buffer (kernel/canon.h); host-testable.
    canon_buf_t canon;

    // ── termios (honest storage) ─────────────────
    // TCSETS/TCSETSW store here; TCGETS reports it back.
    // Default is raw (c_lflag == 0) — matches the actual ring behavior.
    struct termios term;

    // ── Read wait queue ─────────────────────────
    // Tasks blocked in tty_read() wait here.
    // Protected by read_wait_lock (IRQ-safe).
    spinlock_T  read_wait_lock;
    list_t      read_wait;

    // ── Poll wait list ──────────────────────────
    // Poll entries (poll_wait_entry_t.node) wait here.
    // Also protected by ring_lock.
    list_t      read_poll;

    // ── Output callbacks ────────────────────────
    void (*output_char)(char c);   // primary output
    void (*echo_char)(char c);     // echo output (usually same as output_char)

    // ── Foreground process group (foreground pgrp) ──
    // Set by TIOCSPGRP ioctl; defaults to 0 (no foreground pgrp).
    // Protected by fg_pgrp_lock (IRQ-safe).
    pid_t       fg_pgrp;         // 前台进程组 ID（§4.1.1 / TIOCSPGRP / §3.4 写入）
    spinlock_T  fg_pgrp_lock;    // 保护 fg_pgrp，IRQ-safe
} tty_t;

// ── API ────────────────────────────────────────────

// Allocate and initialize a TTY.  output_char and echo_char
// may be the same function (dual-write to fb+serial).
tty_t *tty_alloc(void (*output_char)(char), void (*echo_char)(char));

// Push one character into the TTY input buffer.
// Called from IRQ context (keyboard / serial handlers).
// Wakes any task blocked in tty_read().
void tty_push_input(tty_t *tty, char c);

// Read up to `size` bytes from the TTY.  Blocks until data is available.
// Returns number of bytes copied, or -EINTR if interrupted by a signal.
int tty_read(tty_t *tty, char *buf, int size, bool nonblock);

// Write `size` bytes to the TTY output.  Goes to both
// output_char and echo_char callbacks.
int tty_write(tty_t *tty, const char *buf, int size);

// Set and get the console TTY singleton — used by devfs, ioctl, and main.c.
void tty_set_dev_tty(tty_t *tty);
tty_t *get_dev_tty(void);

// TTY ioctl — physical TTY ioctl callback for devfs_ops.ioctl.
// Handles TCGETS/TCSETS/TIOCGWINSZ/TIOCGPGRP/FIONREAD.
int tty_phys_ioctl(struct vfs_node *node, int cmd, void *arg);

// TTY poll — check if input is available.  Returns POLLIN/POLLOUT mask.
// If not ready, registers a poll_wait_entry on read_poll.
uint32_t tty_poll(tty_t *tty, uint32_t requested, struct poll_table *pt);

#endif
