#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
int fflush(void *f) { (void)f; return 0; }
int vfprintf(void *f, const char *fmt, __builtin_va_list ap) {
	(void)f;
	static char buf[4096];
	int len = vsprintf(buf, fmt, ap);
	if (len > 0) write(1, buf, len);
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
int fputc(int c, void *f) { (void)f; return putchar(c); }
int fputs(const char *s, void *f) { (void)f; while(*s) putchar(*s++); return 0; }
