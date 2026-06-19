/* mock_kernel.c — Kernel stubs for host testing.
 * Compiled as a plain HOST binary without custom libc. */
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

int color_printk(unsigned int FRcolor, unsigned int BKcolor,
                 const char *fmt, ...)
{
    (void)FRcolor; (void)BKcolor;
    va_list args;
    va_start(args, fmt);
    int ret = vprintf(fmt, args);
    va_end(args);
    return ret;
}

int serial_printk(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int ret = vprintf(fmt, args);
    va_end(args);
    return ret;
}

void *kmalloc(size_t size) { return malloc(size); }
size_t kfree(void *addr) { free(addr); return 1; }

void kpanic(const char *msg, ...) {
    va_list args;
    va_start(args, msg);
    vfprintf(stderr, msg, args);
    va_end(args);
    fprintf(stderr, "\nPANIC\n");
    exit(1);
}
