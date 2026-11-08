#define _GNU_SOURCE
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <fcntl.h>
#include <stdio/file.h>
#include <stdio/internal.h>

/* ── The three standard-stream states ────────────────────────────────
 *
 * The public header keeps the historical sentinel VALUES for stdin,
 * stdout and stderr ((FILE*)1/2/3), so existing user code that compares
 * a FILE* against them still works.  The sentinel is never
 * dereferenced, though: the resolver below maps it onto one of these
 * three real state objects.  That is what makes a standard stream behave
 * exactly like an fopen()-ed stream — it has an fd, capabilities, an EOF
 * indicator, an error indicator, a pushback byte and a last-I/O
 * direction, and fclose() really closes its fd. */
static struct __FILE stdio_stdin_state = {
    .fd = 0, .caps = __STDIO_READ, .pending = -1, .last_op = __STDIO_OP_NONE,
    .owns_fd = 1,
};
static struct __FILE stdio_stdout_state = {
    .fd = 1, .caps = __STDIO_WRITE, .pending = -1, .last_op = __STDIO_OP_NONE,
    .owns_fd = 1,
};
static struct __FILE stdio_stderr_state = {
    .fd = 2, .caps = __STDIO_WRITE, .pending = -1, .last_op = __STDIO_OP_NONE,
    .owns_fd = 1,
};

/* ── Open-file registry ────────────────────────────────────────────
 *
 * Tracks every FILE returned by fopen()/fdopen() until the matching
 * fclose().  Used by the resolver to validate its argument (POSIX:
 * operations on a stream we do not own must fail, not dereference an
 * arbitrary address) and by fclose() to catch use-after-free /
 * double-close.  Fixed-size array (mirrors libc/stdlib/atexit.c's
 * ATEXIT_MAX pattern); slots are reused after fclose().
 *
 * P2-8 contract on OPEN_FILES_MAX:
 *   This libc has no dynamic allocation for the registry (mirrors the
 *   atexit() pattern — bounded resources stay bounded). 32 slots is
 *   enough for OS01's userspace (busybox's typical file count per
 *   process is single-digit; busybox `find` opens many but reuses
 *   the same fd slots via fopen+fclose pairs). Exceeding the limit
 *   is a hard error: register_file() returns -1, callers (fopen /
 *   fdopen) propagate it as ENOMEM (POSIX's closest errno for
 *   "resource exhaustion"). Process startup does NOT pre-allocate;
 *   each slot is created lazily on first register_file().
 *
 *   Re-encode the cap if the workload outgrows it — every consumer
 *   below (fread, fwrite, vfprintf, fflush, fclose) consults the
 *   same resolver, so a regrown cap is transparent.
 */
#define OPEN_FILES_MAX 32
static FILE *open_files[OPEN_FILES_MAX];
static int   open_files_count = 0;

static int register_file(FILE *f)
{
    if (!f) return -1;
    for (int i = 0; i < open_files_count; i++) {
        if (open_files[i] == NULL) {
            open_files[i] = f;
            return 0;
        }
    }
    if (open_files_count >= OPEN_FILES_MAX) return -1;
    open_files[open_files_count++] = f;
    return 0;
}

static void unregister_file(FILE *f)
{
    for (int i = 0; i < open_files_count; i++) {
        if (open_files[i] == f) {
            open_files[i] = NULL;
            return;
        }
    }
}

static int is_open_file(FILE *f)
{
    for (int i = 0; i < open_files_count; i++) {
        if (open_files[i] == f) return 1;
    }
    return 0;
}

/* ── The resolver ───────────────────────────────────────────────────
 *
 * __stdio_resolve() is the single place that maps a FILE* to its state:
 *
 *   stdin/stdout/stderr sentinel → the matching standard-stream state
 *                                 (NULL once that stream was closed)
 *   a registered stream          → its own state
 *   unknown / stale / NULL       → NULL
 *
 * Every FILE consumer in the libc goes through it, directly or through
 * __stdio_fd(), so a typo'd, stale or unregistered FILE* can never be
 * dereferenced as a stream.  This replaces the old file_to_fd(), which
 * could only answer "which fd?" and therefore could not carry the
 * stream's state.
 *
 * Single-threaded by design: OS01's userspace runs one thread per
 * process, so no locking is required.
 */
FILE *__stdio_resolve(FILE *f)
{
    if (!f) return NULL;
    if (f == stdin)  return stdio_stdin_state.closed  ? NULL : &stdio_stdin_state;
    if (f == stdout) return stdio_stdout_state.closed ? NULL : &stdio_stdout_state;
    if (f == stderr) return stdio_stderr_state.closed ? NULL : &stdio_stderr_state;
    if (!is_open_file(f)) return NULL;
    return f;
}

int __stdio_fd(FILE *f)
{
    FILE *st = __stdio_resolve(f);
    return st ? st->fd : -1;
}

/* ── Open modes ─────────────────────────────────────────────────────
 *
 * The mode string decides two things: what the stream may be used for
 * (the capability bits) and, for fopen(), which flags the file is
 * opened with.  T7 owns the complete mode matrix (the "e"/"x"
 * extensions and every invalid combination); this is the base contract:
 * "r", "w" and "a", each optionally followed by "+" and/or "b".
 */
int __stdio_parse_mode(const char *mode, int *open_flags, unsigned *access)
{
    if (!mode || !mode[0]) {
        errno = EINVAL;
        return -1;
    }

    int base;
    switch (mode[0]) {
    case 'r': base = 0; break;
    case 'w': base = 1; break;
    case 'a': base = 2; break;
    default:
        errno = EINVAL;
        return -1;
    }

    int plus = 0;
    for (const char *p = mode + 1; *p; p++) {
        if (*p == '+') {
            if (plus) { errno = EINVAL; return -1; }
            plus = 1;
        } else if (*p == 'b') {
            /* "b" is a no-op on a POSIX system: there is no text mode. */
        } else {
            errno = EINVAL;
            return -1;
        }
    }

    unsigned c;
    int fl;
    switch (base) {
    case 0:                                   /* "r" */
        c = __STDIO_READ;
        fl = O_RDONLY;
        break;
    case 1:                                   /* "w" */
        c = __STDIO_WRITE;
        fl = O_WRONLY | O_CREAT | O_TRUNC;
        break;
    default:                                  /* "a" */
        c = __STDIO_WRITE;
        fl = O_WRONLY | O_CREAT | O_APPEND;
        break;
    }
    if (plus) {
        c |= (c == __STDIO_READ) ? __STDIO_WRITE : __STDIO_READ;
        /* O_RDWR replaces O_RDONLY/O_WRONLY; the create/truncate/append
         * bits are kept. */
        fl = (fl & ~(O_RDONLY | O_WRONLY)) | O_RDWR;
    }

    if (access) *access = c;
    if (open_flags) *open_flags = fl;
    return 0;
}

FILE *fopen(const char *path, const char *mode)
{
    unsigned access;
    int flags;
    if (!path) {
        errno = EINVAL;
        return NULL;
    }
    if (__stdio_parse_mode(mode, &flags, &access) != 0)
        return NULL;

    FILE *st = calloc(1, sizeof(*st));
    if (!st) return NULL;
    st->pending = -1;
    st->caps = (int)access;
    st->fd = open(path, flags, 0666);
    if (st->fd < 0) {
        free(st);
        return NULL;
    }
    st->owns_fd = 1;
    if (register_file(st) != 0) {
        /* P2-8: OPEN_FILES_MAX exhaustion — POSIX has no specific errno
         * for "FILE registry full", but ENOMEM is the closest fit
         * (resource exhaustion). */
        close(st->fd);
        free(st);
        errno = ENOMEM;
        return NULL;
    }
    return st;
}

FILE *fdopen(int fd, const char *mode)
{
    unsigned access;
    int flags;
    if (fd < 0) {
        errno = EBADF;
        return NULL;
    }
    if (__stdio_parse_mode(mode, &flags, &access) != 0)
        return NULL;

    FILE *st = calloc(1, sizeof(*st));
    if (!st) return NULL;
    st->pending = -1;
    st->fd = fd;
    st->caps = (int)access;

    /* POSIX: fdopen "a" makes the stream append.  fdopen does NOT
     * open (or truncate) anything, so the append bit goes onto the
     * descriptor through F_SETFL, PRESERVING whatever status flags the
     * descriptor already carries (O_NONBLOCK and friends must
     * survive).  The kernel-side O_APPEND write path is the real
     * atomic append (T6). */
    if (mode[0] == 'a') {
        int old_flags = fcntl(fd, F_GETFL);
        if (old_flags >= 0)
            fcntl(fd, F_SETFL, old_flags | O_APPEND);
    }

    /* fdopen does NOT open the file, so it must not close the fd when
     * the registration fails — POSIX leaves the fd open on failure. */
    st->owns_fd = 1;
    if (register_file(st) != 0) {
        /* P2-8: same ENOMEM contract as fopen. */
        free(st);
        errno = ENOMEM;
        return NULL;
    }
    return st;
}

int fclose(FILE *f)
{
    FILE *st = __stdio_resolve(f);
    if (!st) return EOF;          /* unknown / already closed */

    int rc = 0;
    if (st->owns_fd && st->fd >= 0) {
        /* The close error is propagated, but the stream is invalidated
         * either way: a stream whose fd could not be closed must not be
         * handed back to the caller. */
        if (close(st->fd) < 0)
            rc = EOF;
    }

    if (st == &stdio_stdin_state || st == &stdio_stdout_state ||
        st == &stdio_stderr_state) {
        /* A standard stream keeps its (static) state object, but the
         * state now says "closed": every later operation on it fails
         * instead of writing to a descriptor that is no longer ours. */
        st->closed = 1;
        st->owns_fd = 0;
        st->fd = -1;
        st->caps = 0;
        st->pending = -1;
    } else {
        unregister_file(st);
        free(st);
    }
    return rc;
}

/* ── Stream I/O helpers ──────────────────────────────────────────────
 *
 * These are the only places where the stream's EOF / error indicators
 * and the last-I/O direction are updated, so every entry point
 * (fread, fwrite, getc, fgetc, putc, fputc, fputs, printf, fprintf)
 * reports the same state.
 */
long __stdio_write(FILE *f, const char *buf, size_t len)
{
    FILE *st = __stdio_resolve(f);
    if (!st) return -1;
    if (!(st->caps & __STDIO_WRITE)) {
        /* Writing to a read-only stream is a programming error, but it
         * must be visible: the error indicator is set and the call
         * fails, rather than silently succeeding. */
        st->error = 1;
        errno = EBADF;
        return -1;
    }
    st->last_op = __STDIO_OP_WRITE;
    if (len == 0) return 0;

    ssize_t w = write_all(st->fd, buf, len);
    if (w < 0) {
        st->error = 1;
        return -1;
    }
    return (long)w;
}

int __stdio_write_byte(FILE *f, int c)
{
    char ch = (char)c;
    if (__stdio_write(f, &ch, 1) < 0)
        return EOF;
    return (unsigned char)c;
}

/* Bulk read.  Returns the number of bytes read (0 at end-of-file), and
 * sets the stream's EOF or error indicator as appropriate.  A pending
 * pushback byte is consumed first. */
static long stream_read(FILE *st, void *buf, size_t len)
{
    unsigned char *p = (unsigned char *)buf;
    size_t got = 0;

    /* The EOF indicator is sticky: a stream that already hit end-of-file
     * must not touch the descriptor again. */
    if (st->eof) return 0;

    if (st->pending >= 0) {
        p[0] = (unsigned char)st->pending;
        st->pending = -1;
        got = 1;
    }

    while (got < len) {
        int64_t n = read(st->fd, p + got, (uint64_t)(len - got));
        if (n < 0) {
            if (errno == EINTR) continue;
            /* An I/O error is NOT an end-of-file: the error indicator is
             * set and the EOF indicator is left alone. */
            st->error = 1;
            break;
        }
        if (n == 0) {
            st->eof = 1;
            break;
        }
        got += (size_t)n;
    }
    return (long)got;
}

int __stdio_read_byte(FILE *f)
{
    FILE *st = __stdio_resolve(f);
    if (!st) return EOF;
    if (!(st->caps & __STDIO_READ)) {
        st->error = 1;
        errno = EBADF;
        return EOF;
    }
    st->last_op = __STDIO_OP_READ;

    /* The EOF indicator is sticky: once a read hit end-of-file, further
     * reads keep returning EOF until clearerr() (or a successful
     * ungetc()) clears it. */
    if (st->eof) return EOF;

    if (st->pending >= 0) {
        int c = (unsigned char)st->pending;
        st->pending = -1;
        return c;
    }

    for (;;) {
        unsigned char c;
        int64_t n = read(st->fd, &c, 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            /* An I/O error is NOT an end-of-file: only the error
             * indicator is set. */
            st->error = 1;
            return EOF;
        }
        if (n == 0) {
            st->eof = 1;
            return EOF;
        }
        return (int)c;
    }
}

/* ── Status indicators ────────────────────────────────────────────── */
int feof(FILE *f)
{
    FILE *st = __stdio_resolve(f);
    return (st && st->eof) ? 1 : 0;
}

int ferror(FILE *f)
{
    FILE *st = __stdio_resolve(f);
    return (st && st->error) ? 1 : 0;
}

void clearerr(FILE *f)
{
    FILE *st = __stdio_resolve(f);
    if (!st) return;
    st->eof = 0;
    st->error = 0;
}

int feof_unlocked(FILE *f)
{
    return feof(f);
}

int ferror_unlocked(FILE *f)
{
    return ferror(f);
}

void clearerr_unlocked(FILE *f)
{
    clearerr(f);
}

/* ungetc: push one byte back onto a readable stream.
 *
 *   - EOF is not a valid pushback character and is rejected;
 *   - only ONE byte may be held at a time;
 *   - a successful ungetc clears the EOF indicator, so a stream that hit
 *     end-of-file can be read again from the pushed-back byte. */
int ungetc(int c, FILE *f)
{
    FILE *st = __stdio_resolve(f);
    if (!st) return EOF;
    if (c == EOF) return EOF;
    if (!(st->caps & __STDIO_READ)) return EOF;
    if (st->pending >= 0) return EOF;

    st->pending = (unsigned char)c;
    st->eof = 0;
    return (unsigned char)c;
}

/* ── Block I/O ────────────────────────────────────────────────────── */
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *f)
{
    if (!ptr || !f) return 0;
    if (size == 0 || nmemb == 0) return 0;
    if (nmemb > SIZE_MAX / size) return 0;

    FILE *st = __stdio_resolve(f);
    if (!st) return 0;
    if (!(st->caps & __STDIO_READ)) {
        st->error = 1;
        errno = EBADF;
        return 0;
    }
    st->last_op = __STDIO_OP_READ;

    long got = stream_read(st, ptr, size * nmemb);
    if (got <= 0) return 0;
    return (size_t)got / size;
}

size_t fwrite(const void *p, size_t s, size_t n, FILE *f)
{
    if (!p || !f) return 0;
    if (s == 0 || n == 0) return 0;
    if (n > SIZE_MAX / s) return 0;

    FILE *st = __stdio_resolve(f);
    if (!st) return 0;
    if (!(st->caps & __STDIO_WRITE)) {
        st->error = 1;
        errno = EBADF;
        return 0;
    }
    st->last_op = __STDIO_OP_WRITE;

    /* Byte loop (write_all's discipline: EINTR retries, a zero write
     * with bytes remaining is an error), but with the POSIX item
     * accounting: the return value counts FULL items, and the bytes
     * that did land stay landed — a short-then-fail write must report
     * the completed items, not zero, and must not lose the successful
     * bytes.  write_all() itself stays all-or-nothing for the printf
     * paths, which have no item size to report against. */
    size_t total = s * n;
    size_t done = 0;
    while (done < total) {
        ssize_t w = write(st->fd, (const char *)p + done,
                          (uint64_t)(total - done));
        if (w < 0) {
            if (errno == EINTR) continue;
            st->error = 1;
            break;
        }
        if (w == 0) {
            st->error = 1;
            break;
        }
        done += (size_t)w;
    }
    return done / s;
}

int fflush(FILE *f)
{
    /* fflush(FILE*): drain any pending writes for the given stream.
     *   - f == NULL           → flush ALL streams (POSIX). No-op here
     *                            because every printf/fwrite path in this
     *                            libc is already unbuffered (per-call
     *                            write_all() or direct write()).
     *   - f is a known stream  → nothing to drain (unbuffered), return 0.
     *   - f is unknown        → not a stream we own; POSIX says return
     *                            EOF. Returning 0 here would silently
     *                            accept garbage pointers (a real footgun
     *                            — typo'd stream names would appear to
     *                            succeed).
     *
     * NULL is the only FILE consumer that bypasses the resolver (there
     * is no single fd to resolve for "flush everything"). */
    if (f == NULL) return 0;
    if (!__stdio_resolve(f)) return EOF;
    return 0;
}

ssize_t write_all(int fd, const char *buf, size_t len)
{
    if (len == 0) return 0;
    size_t off = 0;
    while (off < len) {
        ssize_t r = (ssize_t)write(fd, buf + off, (uint64_t)(len - off));
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1;          /* zero write with bytes remaining */
        off += (size_t)r;
    }
    return (ssize_t)len;
}

/* ── Formatted and character output ───────────────────────────────── */
int vfprintf(FILE *f, const char *fmt, __builtin_va_list ap)
{
    FILE *st = __stdio_resolve(f);
    if (!st) return -1;
    if (!(st->caps & __STDIO_WRITE)) {
        st->error = 1;
        errno = EBADF;
        return -1;
    }

    va_list cp;
    va_copy(cp, ap);
    size_t total = vformatter(NULL, 0, fmt, cp, 0);
    va_end(cp);
    if (total == SIZE_MAX || total > INT_MAX) return -1;

    char *buf = malloc(total + 1);
    if (!buf) return -1;

    size_t n = vformatter(buf, total + 1, fmt, ap, 1);
    int ret;
    if (n == SIZE_MAX) {
        ret = -1;
    } else {
        long w = __stdio_write(f, buf, n);
        ret = (w < 0) ? -1 : (int)n;
    }
    free(buf);
    return ret;
}

int fprintf(FILE *f, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	int ret = vfprintf(f, fmt, ap);
	va_end(ap);
	return ret;
}

int putchar_unlocked(int c) { return putchar(c); }

int fputc(int c, FILE *f)
{
    return __stdio_write_byte(f, c);
}

int fputs(const char *s, FILE *f)
{
    if (!s) return EOF;
    size_t len = strlen(s);
    long w = __stdio_write(f, s, len);
    if (w < 0) return EOF;
    return (int)w;
}
