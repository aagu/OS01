/* test/mock/slab_color_stub.c — host-side color_printk stub for slab tests.
 *
 * Both kernel/memory/slab.c and kernel/memory/pmm.c call
 * color_printk(FRcolor, BKcolor, fmt, ...) for debug/error messages.
 * On the host, mock_kernel.c normally provides it as a vprintf to stdout.
 *
 * But for the slab tests we link REAL kernel/memory/slab.c, which
 * itself defines kmalloc / kfree (slab.c:153 / slab.c:249). Linking
 * mock_kernel.o alongside real slab.c duplicates those symbols.
 * Dropping mock_kernel.o means we lose color_printk — so this stub
 * fills that gap.
 *
 * The body is a vprintf to host stderr; the slab tests only invoke
 * error paths (alloc_pages fail in kmalloc_create, recursive call,
 * size > 1048576, double-free, ...) when invariants break, so
 * surface messages on stderr instead of stdout so they don't get
 * swallowed by the runner's grep filters.
 */
#include <stdarg.h>
#include <stdio.h>

int color_printk(unsigned int FRcolor, unsigned int BKcolor,
                 const char *fmt, ...)
{
    (void)FRcolor;
    (void)BKcolor;
    va_list args;
    va_start(args, fmt);
    int n = vfprintf(stderr, fmt, args);
    va_end(args);
    return n;
}