// kernel/tty/canon.c — canonical (ICANON) line discipline, pure logic.
//
// Operates on canon_buf_t + a byte ring (head/tail indices).  No task,
// scheduler, IRQ, or lock dependencies — intentionally host-testable.
//
// Usage (from tty_read, under ring_lock):
//     canon_accumulate(&tty->canon, tty->ring, &tty->head, &tty->tail,
//                      TTY_BUF_SIZE);
//     n = canon_read(&tty->canon, buf, size);

#include <kernel/canon.h>
#include <string.h>

int canon_accumulate(canon_buf_t *cb,
                     const char *ring, volatile int *head, volatile int *tail,
                     int ring_size)
{
    while (*head != *tail && cb->len < CANON_BUF_SIZE) {
        char c = ring[*tail];
        *tail = (*tail + 1) % ring_size;
        cb->buf[cb->len++] = c;
        if (c == '\n')
            break;
    }
    return cb->len;
}

int canon_read(canon_buf_t *cb, char *out, int size)
{
    // Find the FIRST '\n' — the first complete line boundary.  The buffer
    // may hold several lines (e.g. "a\nb" — last char is not '\n' but a
    // complete line exists at the head).
    int i;
    for (i = 0; i < cb->len; i++) {
        if (cb->buf[i] == '\n')
            break;
    }
    if (i == cb->len)
        return 0;   // no complete line yet

    int copy = (i + 1) < size ? (i + 1) : size;
    memcpy(out, cb->buf, copy);
    memmove(cb->buf, cb->buf + copy, cb->len - copy);
    cb->len -= copy;
    return copy;
}
