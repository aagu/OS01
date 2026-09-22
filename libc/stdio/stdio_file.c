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
 * fclose(). */
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
        close(mf->fd);
        free(mf);
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
        free(mf);
        return NULL;
    }
    return mf;
}

int fclose(void *f)
{
    if (!f) return -1;
    /* stdin/stdout/stderr are sentinel values (1/2/3), not mini_file_t.
     * fwrite() special-cases them but fclose() did not — closing stdin
     * dereferenced address 1 and user-faulted (busybox nl crash). */
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
    /* stdout/stderr/stdin are sentinels (fd 1/2/0), not mini_file_t —
     * POSIX: stdout/stderr are not open for reading; stdin is not
     * readable via fread in this unbuffered libc. Return 0 rather than
     * dereferencing the sentinel as a struct address. */
    if (f == stdout || f == stderr || f == stdin) return 0;
    if (!is_open_file(f)) return 0;       /* not a stream we own */
    mini_file_t *mf = (mini_file_t *)f;
    int64_t n = read(mf->fd, ptr, size * nmemb);
    if (n < 0) return 0;
    return (size_t)(n / size);
}

size_t fwrite(const void *p, size_t s, size_t n, void *f)
{
    if (!f || !p) return 0;
    /* stdout/stderr are raw fd 1/2, not wrapped in mini_file_t */
    if (f == stdout || f == stderr) {
        int fd = (f == stderr) ? 2 : 1;
        int64_t written = write(fd, p, s * n);
        return (written < 0) ? 0 : (size_t)(written / s);
    }
    if (!is_open_file(f)) return 0;       /* not a stream we own */
    mini_file_t *mf = (mini_file_t *)f;
    int64_t written = write(mf->fd, p, s * n);
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
     *                            succeed). */
    if (f == NULL || f == stdin || f == stdout || f == stderr)
        return 0;
    if (is_open_file(f))
        return 0;
    return -1;
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
    int fd;
    if (f == stdout)      fd = 1;
    else if (f == stderr) fd = 2;
    else if (f == stdin)  fd = 0;
    else                  fd = ((mini_file_t *)f)->fd;

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
int fputc(int c, void *f)
{
    int fd = fileno_unlocked((FILE *)f);
    unsigned char ch = (unsigned char)c;
    syscall(SYS_write, fd, (uint64_t)&ch, 1);
    return c;
}
int fputs(const char *s, void *f)
{
    int fd = fileno_unlocked((FILE *)f);
    size_t len = 0;
    while (s[len]) len++;
    syscall(SYS_write, fd, (uint64_t)s, (uint64_t)len);
    return 0;
}
