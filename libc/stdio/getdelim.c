// libc/stdio/getdelim.c — dynamic delimited line reading (T8).
//
// getdelim() reads from a stream until `delim` (inclusive), end-of-file
// or error, growing the caller's buffer as needed.  getline() is the
// '\n'-delimited form.  This is what BusyBox's line-based applets
// (head, tail, ...) call through xmalloc_fgetline() when
// HAVE_GETLINE=1 — before T8 the libc only DECLARED getline, and
// BusyBox's libbb fallback had to stand in.
//
// Contract (POSIX + the plan's T8 checklist):
//   - *lineptr == NULL or *n == 0: a buffer is allocated (128 bytes to
//     start); the caller frees it.
//   - The buffer grows by doubling; all arithmetic is overflow-safe and
//     the accumulated length is capped at SSIZE_MAX (EINVAL beyond it).
//   - Return: bytes read INCLUDING the delimiter; a final unterminated
//     segment counts too.  -1 only when nothing was read and the stream
//     hit EOF (eof indicator set) or an I/O error (error indicator set,
//     errno from the read) or the arguments are invalid (EINVAL) or an
//     allocation/boundary failed (ENOMEM/EINVAL).
//   - On ANY allocation failure the caller's buffer stays valid and
//     freeable, *lineptr/*n describe exactly what is in it (the bytes
//     read so far are preserved), and the failure leaves them
//     consistent — the caller may retry or free without leaking.
//   - Single-threaded by design (no locking, like the rest of this
//     libc).

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <sys/types.h>

/* Grow to hold one more byte plus the terminating NUL.  Returns the
 * (possibly moved) buffer, or NULL on failure — in which case *bufp
 * still points at the last valid allocation. */
static char *grow_line(char **bufp, size_t *capp, size_t need)
{
    size_t cap = *capp;
    size_t new_cap;

    if (cap >= need) return *bufp;

    if (cap == 0) {
        new_cap = 128;
    } else {
        /* Double, saturating at the largest capacity getdelim will
         * ever hand out: SSIZE_MAX bytes of line + the NUL. */
        if (cap > (size_t)SSIZE_MAX / 2)
            new_cap = (size_t)SSIZE_MAX + 1;
        else
            new_cap = cap * 2;
    }
    if (new_cap < need) new_cap = need;
    if (new_cap > (size_t)SSIZE_MAX + 1) new_cap = (size_t)SSIZE_MAX + 1;

    char *grown = realloc(*bufp, new_cap);
    if (!grown) return NULL;            /* *bufp unchanged and still valid */
    *bufp = grown;
    *capp = new_cap;
    return grown;
}

ssize_t getdelim(char **lineptr, size_t *n, int delim, FILE *f)
{
    if (!lineptr || !n || !f) {
        errno = EINVAL;
        return -1;
    }
    if (*n > (size_t)SSIZE_MAX + 1) {
        /* A capacity the SSIZE_MAX return could never describe is a
         * caller bug: reject it up front, without a read. */
        errno = EINVAL;
        return -1;
    }

    char *buf = *lineptr;
    size_t cap = *n;

    if (!buf || cap == 0) {
        buf = NULL;
        cap = 0;
        if (!grow_line(&buf, &cap, 1)) {
            errno = ENOMEM;
            return -1;
        }
    }

    size_t len = 0;
    for (;;) {
        int c = fgetc(f);
        if (c == EOF) {
            if (ferror(f)) {
                /* A real I/O error (not end-of-file): errno carries the
                 * read's failure; the bytes read so far stay in the
                 * caller's buffer. */
                *lineptr = buf;
                *n = cap;
                return -1;
            }
            if (len == 0) {
                /* End-of-file before any byte: the canonical -1.  The
                 * stream's eof indicator is set (fgetc did it); the
                 * caller's buffer is returned with its capacity so an
                 * EOF retry with the same buffer keeps working. */
                *lineptr = buf;
                *n = cap;
                return -1;
            }
            break;                      /* final segment without delim */
        }

        /* Room for the byte AND the terminating NUL. */
        if (cap < len + 2) {
            /* The internal SSIZE_MAX boundary: one more byte would
             * make the line (and the ssize_t return) overflow. */
            if (len >= (size_t)SSIZE_MAX) {
                ungetc(c, f);           /* nothing that was read is lost */
                *lineptr = buf;
                *n = cap;
                errno = EINVAL;
                return -1;
            }
            if (!grow_line(&buf, &cap, len + 2)) {
                /* Push the byte we just consumed back onto the stream
                 * before giving up: the caller may retry with a bigger
                 * budget, and NOTHING that was read may be lost. */
                ungetc(c, f);
                *lineptr = buf;
                *n = cap;
                errno = ENOMEM;
                return -1;
            }
        }

        buf[len++] = (char)c;
        if (c == delim) break;
    }

    buf[len] = '\0';
    *lineptr = buf;
    *n = cap;
    return (ssize_t)len;
}

ssize_t getline(char **lineptr, size_t *n, FILE *f)
{
    return getdelim(lineptr, n, '\n', f);
}
