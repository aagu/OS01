#include <stddef.h>
#include <stdio.h>
#include <string.h>
int fflush(void *f) { (void)f; return 0; }
int vfprintf(void *f, const char *fmt, __builtin_va_list ap) { (void)f; return 0; }
int fprintf(void *f, const char *fmt, ...) { (void)f; (void)fmt; return 0; }
int putchar_unlocked(int c) { return putchar(c); }
int fputc(int c, void *f) { (void)f; return putchar(c); }
int fputs(const char *s, void *f) { (void)f; while(*s) putchar(*s++); return 0; }
