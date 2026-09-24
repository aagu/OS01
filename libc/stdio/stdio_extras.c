#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/syscall.h>
#include "stdio_internal.h"   /* file_to_fd() — P1-5 truth source */

int vprintf(const char *fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

int dprintf(int fd, const char *fmt, ...) {
    (void)fd; (void)fmt;
    return 0;
}

/* ── stdio (from busybox_stubs.c, fixed for fd correctness) ── */

int ferror_unlocked(void *f) { (void)f; return 0; }
int clearerr(void *f)        { (void)f; return 0; }

/* fileno_unlocked: return the real fd behind a FILE*.  busybox maps
 * fileno() to this, and wget uses it for shutdown()/poll().  The old
 * stub returned 0 (stdin), which silently broke socket handling.
 *
 * P1-5 (review round 2): routed through file_to_fd() instead of
 * re-implementing the sentinel/registry logic inline. The inline
 * version silently bypassed is_open_file() and dereferenced any non-NULL
 * pointer as a mini_file_t, so an unregistered FILE* (or a stale one
 * post-fclose) would yield an arbitrary fd — a real footgun that this
 * shared resolver closes across EVERY FILE consumer in the libc. */
int fileno_unlocked(FILE *f)
{
    return file_to_fd((void *)f);
}

/* getc_unlocked: read one byte from the FILE's fd.  The old stub
 * called getchar() (stdin), so wget's getc(sfp) read the terminal
 * instead of the socket and hung forever.
 *
 * P1-5: same routing — unknown FILE* → -1 → read returns <= 0 → EOF.
 * This is the same defensive contract fread uses for unregistered
 * streams, applied to the byte-at-a-time unlocked path. */
int getc_unlocked(void *f)
{
    int fd = file_to_fd(f);
    if (fd < 0) return EOF;
    unsigned char c;
    int64_t ret = syscall(SYS_read, fd, (uint64_t)&c, 1);
    if (ret <= 0) return EOF;
    return (int)c;
}

/* P1-5: putc_unlocked and fgets_unlocked route through file_to_fd()
 * too, not the pre-fix inline resolver in fileno_unlocked(). The
 * indirection is now flat (file_to_fd is the one place that knows
 * about sentinels + the registry), so a future change to the
 * registry — say, switching to a hash table — only has to update
 * file_to_fd, not six consumer call sites. */
int putc_unlocked(int c, void *f)
{
    int fd = file_to_fd(f);
    if (fd < 0) return EOF;
    unsigned char ch = (unsigned char)c;
    syscall(SYS_write, fd, (uint64_t)&ch, 1);
    return c;
}

char *fgets_unlocked(char *s, int n, void *f)
{
    if (!s || n <= 1) return NULL;
    int fd = file_to_fd(f);
    if (fd < 0) return NULL;
    int i = 0;
    while (i < n - 1) {
        int64_t ret = syscall(SYS_read, fd, (uint64_t)&s[i], 1);
        if (ret <= 0) break;
        i++;
        if (s[i - 1] == '\n') break;
    }
    if (i == 0) return NULL;
    s[i] = '\0';
    return s;
}
