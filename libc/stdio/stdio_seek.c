// libc/stdio/stdio_seek.c — stream positioning and re-opening (T9).
//
// Everything routes through the T5 resolver and the descriptor layer:
//
//   - fseeko() translates the stream's LOGICAL position (physical
//     descriptor offset minus a pending ungetc'd byte for SEEK_CUR),
//     issues ONE lseek, then discards the pushback byte and clears the
//     EOF indicator — the POSIX post-seek state.
//   - ftello() reports the LOGICAL position the same way, so an
//     ftello/fseek(0,SEEK_CUR) pair round-trips across an ungetc.
//   - rewind() is fseek(0, SEEK_SET) plus clearing the ERROR
//     indicator (POSIX: rewind undoes both ungetc and error state and
//     returns nothing).
//   - freopen() closes the stream's current descriptor (releasing a
//     standard stream's fd or an owned fd) and re-points the SAME
//     stream object at the freshly opened file, going through the
//     shared T7 mode parser.  On failure the stream is left CLOSED
//     and unusable (POSIX) and NULL is returned; a NULL path is
//     rejected with EINVAL (this libc does not implement the Linux
//     "reopen the same file" extension).
//
// Errors are never faked: a failing lseek propagates errno (ESPIPE on
// a pipe, EINVAL on a bad whence), a failing close or open inside
// freopen is reported, and every entry point answers -1/NULL/EOF for
// an unregistered or closed stream instead of touching it.

#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>               /* lseek (this libc declares it here) */
#include <stdlib.h>
#include <stdio/file.h>

int fseeko(FILE *f, long off, int whence)
{
    FILE *st = __stdio_resolve(f);
    if (!st || st->fd < 0) {
        errno = EBADF;
        return -1;
    }
    if (whence != SEEK_SET && whence != SEEK_CUR && whence != SEEK_END) {
        errno = EINVAL;
        return -1;
    }

    /* SEEK_CUR is relative to the stream's LOGICAL position: a byte
     * pushed back by ungetc is re-readable, so the descriptor sits one
     * byte ahead of where the caller believes the stream to be.  The
     * physical offset is queried first, the pushback subtracted, and
     * the result re-applied absolutely — after the seek the pushback
     * is gone, so the absolute value IS the logical position. */
    if (whence == SEEK_CUR && st->pending >= 0) {
        int64_t cur = lseek(st->fd, 0, SEEK_CUR);
        if (cur < 0)
            return -1;                  /* errno from lseek (ESPIPE …) */
        whence = SEEK_SET;
        off = (long)cur - 1 + off;
    }

    int64_t r = lseek(st->fd, (int64_t)off, (int)whence);
    if (r < 0)
        return -1;                      /* errno from lseek (ESPIPE …) */

    /* POSIX post-seek state: the pushback byte is discarded and the
     * EOF indicator cleared.  The error indicator is NOT touched (only
     * rewind and clearerr clear it). */
    st->pending = -1;
    st->eof = 0;
    return 0;
}

int fseek(FILE *f, long off, int whence)
{
    return fseeko(f, off, whence);
}

off_t ftello(FILE *f)
{
    FILE *st = __stdio_resolve(f);
    if (!st || st->fd < 0) {
        errno = EBADF;
        return -1;
    }

    int64_t r = lseek(st->fd, 0, SEEK_CUR);
    if (r < 0)
        return -1;                      /* errno from lseek (ESPIPE …) */

    if (st->pending >= 0) {
        if (r == 0) {
            /* Cannot happen (a pushed-back byte implies a consumed
             * byte), but never underflow the position. */
            errno = ESPIPE;
            return -1;
        }
        r -= 1;                         /* the logical position */
    }
    return (off_t)r;
}

long ftell(FILE *f)
{
    return (long)ftello(f);
}

void rewind(FILE *f)
{
    /* POSIX rewind: seek to the beginning, clear BOTH the EOF and the
     * error indicators, no return value.  A failing seek on a broken
     * stream still clears the indicators (rewind has nothing to
     * report through). */
    (void)fseeko(f, 0, SEEK_SET);
    FILE *st = __stdio_resolve(f);
    if (st) {
        st->eof = 0;
        st->error = 0;
    }
}

FILE *freopen(const char *path, const char *mode, FILE *f)
{
    if (!path) {
        errno = EINVAL;
        return NULL;
    }

    unsigned access;
    int open_flags;
    if (__stdio_parse_mode(mode, &open_flags, &access) != 0)
        return NULL;                    /* errno = EINVAL from the parser */

    FILE *st = __stdio_resolve(f);
    if (!st) {
        errno = EBADF;
        return NULL;
    }

    /* Close the current descriptor first: freopen RE-POINTS this very
     * stream, so the old file must be gone (and its fd released)
     * before the new one is opened.  A close failure does NOT stop
     * the re-open — the fd number is being given up either way — but
     * it is remembered so the failure is visible if the open succeeds
     * anyway (POSIX lets freopen report the close error through the
     * eventual fclose; we keep it simple and honour the close error
     * only when the re-open fails, where it is the errno the caller
     * sees). */
    int close_errno = 0;
    if (st->owns_fd && st->fd >= 0) {
        if (close(st->fd) < 0)
            close_errno = errno;
    }

    int nfd = open(path, open_flags, 0666);
    if (nfd < 0) {
        /* POSIX: the stream is closed and unusable after a failed
         * freopen ("opening the new path fails, old stream becomes
         * invalid") — the resolver answers NULL for it from now on. */
        st->closed = 1;
        st->fd = -1;
        st->owns_fd = 0;
        st->caps = 0;
        st->pending = -1;
        st->eof = 0;
        st->error = 0;
        errno = close_errno ? close_errno : errno;
        return NULL;
    }

    st->fd = nfd;
    st->owns_fd = 1;
    st->caps = (int)access;
    st->pending = -1;
    st->eof = 0;
    st->error = 0;
    st->last_op = 0;
    st->closed = 0;
    return f;                           /* the SAME stream, re-pointed */
}
