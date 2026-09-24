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
#include <sys/syscall.h>
#include "stdio_internal.h"

/* Minimal FILE struct lives in stdio.h (shared with stdio_extras.c) */
/* typedef struct { int fd; int mode; } mini_file_t; -- see stdio.h */

/* ── Open-file registry ────────────────────────────────────
 *
 * Tracks every mini_file_t returned by fopen()/fdopen() until the
 * matching fclose(). Used by fflush() to validate its FILE* argument
 * (POSIX: fflush returns EOF on invalid stream) and by fclose() to
 * catch use-after-free / double-close. Fixed-size array (mirrors
 * libc/stdlib/atexit.c's ATEXIT_MAX pattern); slots are reused after
 * fclose().
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
 *   same is_open_file() check, so a regrown cap is transparent.
 */
#define OPEN_FILES_MAX 32
static void *open_files[OPEN_FILES_MAX];
static int   open_files_count = 0;

static int register_file(void *f)
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

static void unregister_file(void *f)
{
    for (int i = 0; i < open_files_count; i++) {
        if (open_files[i] == f) {
            open_files[i] = NULL;
            return;
        }
    }
}

static int is_open_file(void *f)
{
    for (int i = 0; i < open_files_count; i++) {
        if (open_files[i] == f) return 1;
    }
    return 0;
}

/* P1-5: single entry point for "FILE* → fd" — used by every FILE
 * consumer in stdio_file.c (vfprintf, fflush, fread, fwrite, fclose)
 * AND every consumer in stdio_extras.c (fileno_unlocked,
 * getc_unlocked, putc_unlocked, fgets_unlocked, fputc, fputs).
 * Centralises the sentinel short-circuit (stdin/stdout/stderr are
 * owned by libc, not registered) and the registry check (returns -1
 * for unknown pointers instead of dereferencing them as a struct).
 *
 * Returns:
 *   - stdin/stdout/stderr sentinel  → 0/1/2 respectively
 *   - registered FILE*              → mf->fd
 *   - unknown / unregistered FILE* → -1
 *
 * Callers that need to distinguish "unknown pointer" from "real EOF"
 * must check for -1 themselves (vfprintf returns -1; fread/fwrite
 * return 0).
 *
 * Exported via stdio_internal.h so stdio_extras.c can call it
 * without re-implementing the sentinel/registry logic. Re-implementing
 * it (the pre-fix state) is exactly what allowed FILE* consumers in
 * stdio_extras.c to bypass the registry — the bug that the reviewer
 * flagged for the P1-5 cleanup batch.
 *
 * Single-threaded by design: OS01's userspace runs one thread per
 * process. The kernel is SMP, but the libc's registry is only touched
 * from the single userspace thread, so no locking is required. The
 * (single-threaded) interleaving test in test_libc_stdio_registry
 * exercises the slot-reuse / scan-on-register paths under rapid
 * sequential fopen→fclose cycles — the pattern that would surface
 * any algorithmic bug in the registry's bookkeeping.
 */
int file_to_fd(void *f)
{
    if (f == stdin)  return 0;
    if (f == stdout) return 1;
    if (f == stderr) return 2;
    if (!is_open_file(f)) return -1;
    return ((mini_file_t *)f)->fd;
}

void *fopen(const char *path, const char *mode)
{
    mini_file_t *mf = calloc(1, sizeof(*mf));
    if (!mf) return NULL;
    if (mode[0] == 'r') mf->mode = 0;
    else mf->mode = 1;
    int flags = (mf->mode == 0) ? O_RDONLY : (O_WRONLY | O_CREAT | O_TRUNC);
    mf->fd = open(path, flags, 0666);
    if (mf->fd < 0) { free(mf); return NULL; }
    if (register_file(mf) != 0) {
        /* P2-8: OPEN_FILES_MAX exhaustion — POSIX has no specific errno
         * for "FILE registry full", but ENOMEM is the closest fit
         * (resource exhaustion). */
        close(mf->fd);
        free(mf);
        errno = ENOMEM;
        return NULL;
    }
    return mf;
}

void *fdopen(int fd, const char *mode)
{
    mini_file_t *mf = calloc(1, sizeof(*mf));
    if (!mf) return NULL;
    mf->fd = fd;
    mf->mode = (mode[0] == 'r') ? 0 : 1;
    if (register_file(mf) != 0) {
        /* P2-8: same ENOMEM contract as fopen. */
        free(mf);
        errno = ENOMEM;
        return NULL;
    }
    return mf;
}

int fclose(void *f)
{
    if (!f) return -1;
    /* stdin/stdout/stderr are sentinel values (1/2/3), not mini_file_t.
     * fwrite() special-cases them but fclose() did not — closing stdin
     * dereferenced address 1 and user-faulted (busybox nl crash).
     *
     * P1-5: route through file_to_fd() — sentinel match returns a
     * non-negative fd (0/1/2) but the file isn't in the registry, so we
     * still need the explicit sentinel short-circuit. We use
     * file_to_fd's sentinel detection (fd 0/1/2 with the original
     * pointer matching stdin/stdout/stderr) by checking the pointer
     * identity, not the fd value, since a registered FILE could also
     * have fd=0/1/2. */
    if (f == stdin || f == stdout || f == stderr)
        return 0;
    if (!is_open_file(f)) return -1;   /* not ours / use-after-free */
    mini_file_t *mf = (mini_file_t *)f;
    unregister_file(mf);
    close(mf->fd);
    free(mf);
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, void *f)
{
    if (!f || !ptr) return 0;
    int fd = file_to_fd(f);
    if (fd < 0) return 0;
    if (fd == 1 || fd == 2) return 0;     /* stdout/stderr: not readable */
    int64_t n = read(fd, ptr, size * nmemb);
    if (n < 0) return 0;
    return (size_t)(n / size);
}

size_t fwrite(const void *p, size_t s, size_t n, void *f)
{
    if (!f || !p) return 0;
    int fd = file_to_fd(f);
    if (fd < 0) return 0;
    if (fd == 0) return 0;                /* stdin: not writable */
    int64_t written = write(fd, p, s * n);
    if (written < 0) return 0;
    return (size_t)(written / s);
}

int fflush(void *f)
{
    /* fflush(FILE*): drain any pending writes for the given stream.
     *   - f == NULL           → flush ALL streams (POSIX). No-op here
     *                            because every printf/fwrite path in this
     *                            libc is already unbuffered (per-call
     *                            write_all() or direct write()).
     *   - f is a sentinel     → stdin/stdout/stderr are libc-owned
     *                            constant values ((FILE*)1/2/3); nothing
     *                            to drain, return 0.
     *   - f is a registered   → real fopen()/fdopen() FILE*. Nothing
     *     FILE*                  to drain (unbuffered), return 0.
     *   - f is unknown        → not a stream we own; POSIX says return
     *                            EOF. Returning 0 here would silently
     *                            accept garbage pointers (a real footgun
     *                            — typo'd stream names would appear to
     *                            succeed).
     *
     * P1-5: NULL is the only FILE consumer that bypasses file_to_fd()
     * (no fd to resolve for "flush everything"). Everything else routes
     * through file_to_fd(), so a typo'd FILE* returns -1 from a single
     * shared implementation site. */
    if (f == NULL) return 0;
    if (file_to_fd(f) < 0) return -1;
    return 0;
}

ssize_t write_all(int fd, const char *buf, size_t len)
{
    if (len == 0) return 0;
    size_t off = 0;
    while (off < len) {
        ssize_t r = write(fd, buf + off, len - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1;          /* zero write with bytes remaining */
        off += (size_t)r;
    }
    return (ssize_t)len;
}

int vfprintf(void *f, const char *fmt, __builtin_va_list ap)
{
    /* P1-5: route through file_to_fd() so an unregistered FILE* is
     * rejected (returns -1 → total < 0 path below) instead of being
     * dereferenced as a mini_file_t. Previously this function was the
     * sole FILE consumer that bypassed the registry check. */
    int fd = file_to_fd(f);
    if (fd < 0) return -1;

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
        ssize_t w = write_all(fd, buf, n);
        ret = (w < 0) ? -1 : (int)n;
    }
    free(buf);
    return ret;
}
int fprintf(void *f, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	int ret = vfprintf(f, fmt, ap);
	va_end(ap);
	return ret;
}
int putchar_unlocked(int c) { return putchar(c); }
/* P1-5 (review round 2): fputc / fputs used to call fileno_unlocked()
 * (defined in stdio_extras.c), which in turn bypassed the registry and
 * dereferenced any non-sentinel pointer as a mini_file_t. After round 2
 * both layers route through file_to_fd(), so the entire chain
 * (fputc → file_to_fd → is_open_file → mini_file_t->fd) is gated by the
 * registry check. Unknown / stale FILE* returns -1, the syscall arg is
 * -1, and the kernel rejects it cleanly instead of writing to an
 * arbitrary fd. */
int fputc(int c, void *f)
{
    int fd = file_to_fd(f);
    if (fd < 0) return EOF;
    unsigned char ch = (unsigned char)c;
    syscall(SYS_write, fd, (uint64_t)&ch, 1);
    return c;
}
int fputs(const char *s, void *f)
{
    if (!s) return EOF;
    int fd = file_to_fd(f);
    if (fd < 0) return EOF;
    size_t len = 0;
    while (s[len]) len++;
    syscall(SYS_write, fd, (uint64_t)s, (uint64_t)len);
    return 0;
}
