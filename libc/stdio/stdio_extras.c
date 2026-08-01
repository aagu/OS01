#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>

int vprintf(const char *fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

int dprintf(int fd, const char *fmt, ...) {
    (void)fd; (void)fmt;
    return 0;
}

/* ── stdio stubs (from busybox_stubs.c) ── */

int ferror_unlocked(void *f) { (void)f; return 0; }
int clearerr(void *f)        { (void)f; return 0; }
int fileno_unlocked(FILE *f) { (void)f; return 0; }

void *fopen(const char *path, const char *mode)
    { (void)path; (void)mode; return NULL; }

int fclose(void *f) { (void)f; return 0; }

void *fdopen(int fd, const char *mode)
    { (void)fd; (void)mode; return NULL; }

char *fgets_unlocked(char *s, int n, void *f)
{
    if (!s || n <= 1) return NULL;
    (void)f;
    int i = 0;
    while (i < n - 1) {
        int64_t ret = syscall(SYS_read, 0, (uint64_t)&s[i], 1);
        if (ret <= 0) break;
        i++;
        if (s[i - 1] == '\n') break;
    }
    if (i == 0) return NULL;
    s[i] = '\0';
    return s;
}
