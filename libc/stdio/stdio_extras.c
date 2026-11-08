#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <stdio/file.h>
#include <stdio/internal.h>

/* ── Formatted output ───────────────────────────────────────────────
 *
 * vprintf writes to the standard output STREAM, so the standard output
 * carries the same state as any other stream: after fclose(stdout) a
 * printf() fails instead of writing to a descriptor the process no
 * longer owns, and a write failure sets stdout's error indicator.
 */
int vprintf(const char *fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

/* dprintf writes to a raw descriptor.  It has no stream state of its
 * own (there is no FILE to hang an indicator on), so a write failure is
 * reported through the return value and errno — never as a fake
 * success. */
/* vdprintf is the core: measure with one va_copy pass, format into a
 * buffer with the original list, then one write_all (EINTR-retrying,
 * short-write-looping) to the raw descriptor.  A truncation past
 * INT_MAX, a failed allocation or a failed write is an error — never
 * a fake success. */
int vdprintf(int fd, const char *fmt, va_list ap) {
    va_list cp;
    va_copy(cp, ap);
    size_t total = vformatter(NULL, 0, fmt, cp, 0);
    va_end(cp);
    if (total == SIZE_MAX || total > INT_MAX)
        return -1;

    char *buf = malloc(total + 1);
    if (!buf)
        return -1;

    size_t n = vformatter(buf, total + 1, fmt, ap, 1);
    if (n == SIZE_MAX) {
        free(buf);
        return -1;
    }

    ssize_t w = write_all(fd, buf, n);
    free(buf);
    return (w < 0) ? -1 : (int)n;
}

int dprintf(int fd, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vdprintf(fd, fmt, ap);
    va_end(ap);
    return r;
}

/* ── The FILE consumers ──────────────────────────────────────────────
 *
 * Every one of them goes through the internal resolver, so the registry
 * check applies uniformly: an unknown, unregistered or already-closed
 * FILE* is rejected instead of being dereferenced as a stream.  That
 * was the whole point of the P1-5 cleanup, and the resolver is now the
 * single place that knows about the sentinels and the registry.
 */

/* fileno_unlocked: return the real fd behind a FILE*.  busybox maps
 * fileno() to this, and wget uses it for shutdown()/poll().  The old
 * stub returned 0 (stdin), which silently broke socket handling. */
int fileno_unlocked(FILE *f)
{
    return __stdio_fd(f);
}

/* getc_unlocked: read one byte from the FILE's fd.  The old stub
 * called getchar() (stdin), so wget's getc(sfp) read the terminal
 * instead of the socket and hung forever. */
int getc_unlocked(FILE *f)
{
    return __stdio_read_byte(f);
}

/* putc_unlocked: write one byte to the FILE's fd. */
int putc_unlocked(int c, FILE *f)
{
    return __stdio_write_byte(f, c);
}

/* fgets_unlocked: read up to n-1 bytes, stopping after a newline. */
char *fgets_unlocked(char *s, int n, FILE *f)
{
    if (!s || n <= 1) return NULL;
    int i = 0;
    while (i < n - 1) {
        int c = __stdio_read_byte(f);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return NULL;
    s[i] = '\0';
    return s;
}
