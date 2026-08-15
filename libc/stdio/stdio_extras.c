#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/syscall.h>

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
 * stub returned 0 (stdin), which silently broke socket handling. */
int fileno_unlocked(FILE *f)
{
    if (f == stdin)  return 0;
    if (f == stdout) return 1;
    if (f == stderr) return 2;
    mini_file_t *mf = (mini_file_t *)f;
    return mf ? mf->fd : -1;
}

/* getc_unlocked: read one byte from the FILE's fd.  The old stub
 * called getchar() (stdin), so wget's getc(sfp) read the terminal
 * instead of the socket and hung forever. */
int getc_unlocked(void *f)
{
    int fd = fileno_unlocked((FILE *)f);
    unsigned char c;
    int64_t ret = syscall(SYS_read, fd, (uint64_t)&c, 1);
    if (ret <= 0) return EOF;
    return (int)c;
}

int putc_unlocked(int c, void *f)
{
    int fd = fileno_unlocked((FILE *)f);
    unsigned char ch = (unsigned char)c;
    syscall(SYS_write, fd, (uint64_t)&ch, 1);
    return c;
}

char *fgets_unlocked(char *s, int n, void *f)
{
    if (!s || n <= 1) return NULL;
    int fd = fileno_unlocked((FILE *)f);
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
