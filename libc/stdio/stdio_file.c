#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/syscall.h>

/* Minimal FILE struct lives in stdio.h (shared with stdio_extras.c) */
/* typedef struct { int fd; int mode; } mini_file_t; -- see stdio.h */

void *fopen(const char *path, const char *mode)
{
    mini_file_t *mf = calloc(1, sizeof(*mf));
    if (!mf) return NULL;
    if (mode[0] == 'r') mf->mode = 0;
    else mf->mode = 1;
    int flags = (mf->mode == 0) ? O_RDONLY : (O_WRONLY | O_CREAT | O_TRUNC);
    mf->fd = open(path, flags, 0666);
    if (mf->fd < 0) { free(mf); return NULL; }
    return mf;
}

void *fdopen(int fd, const char *mode)
{
    mini_file_t *mf = calloc(1, sizeof(*mf));
    if (!mf) return NULL;
    mf->fd = fd;
    mf->mode = (mode[0] == 'r') ? 0 : 1;
    return mf;
}

int fclose(void *f)
{
    if (!f) return -1;
    mini_file_t *mf = (mini_file_t *)f;
    close(mf->fd);
    free(mf);
    return 0;
}

size_t fread(void *ptr, size_t size, size_t nmemb, void *f)
{
    if (!f || !ptr) return 0;
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
    mini_file_t *mf = (mini_file_t *)f;
    int64_t written = write(mf->fd, p, s * n);
    if (written < 0) return 0;
    return (size_t)(written / s);
}

int fflush(void *f) { (void)f; return 0; }
int vfprintf(void *f, const char *fmt, __builtin_va_list ap) {
	static char buf[4096];
	int len = vsprintf(buf, fmt, ap);
	if (len > 0) {
		int fd = fileno_unlocked((FILE *)f);
		syscall(SYS_write, fd, (uint64_t)buf, (uint64_t)len);
	}
	return len;
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
