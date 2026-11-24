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

/* Size-targeted failure injection (default off).  A test that needs
 * one specific allocation to fail sets the size and the ordinal, e.g.
 * sizeof(vma_t) / 1, instead of counting every unrelated kmalloc. */
int host_mock_kmalloc_fail_size  = 0;
int host_mock_kmalloc_fail_count = 0;

void *kmalloc(size_t size)
{
    if (host_mock_kmalloc_fail_size > 0 &&
        size == (size_t)host_mock_kmalloc_fail_size &&
        host_mock_kmalloc_fail_count > 0) {
        if (--host_mock_kmalloc_fail_count == 0)
            return NULL;
    }
    return malloc(size);
}
size_t kfree(void *addr) { free(addr); return 1; }

void kpanic(const char *msg, ...) {
    va_list args;
    va_start(args, msg);
    vfprintf(stderr, msg, args);
    va_end(args);
    fprintf(stderr, "\nPANIC\n");
    exit(1);
}
