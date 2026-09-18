/* kernel/arch/aarch64/libc_stub.c — libc alloc/free shims for aarch64.
 *
 * kernel/time/timer.c uses calloc/free (lines 25, 134) for timer
 * object allocation. x86_64 pulls these from the libc sysroot
 * (kernel/Makefile:99); aarch64 phase 2 deliberately does NOT
 * use the libc sysroot (kernel/Makefile:93-99 guards -isystem
 * under ifeq x86_64). This stub provides minimal in-kernel
 * replacements that delegate to the slab allocator.
 *
 * Mirrors the arch/aarch64/subsys_stub.c pattern (Phase 2 #1).
 * Phase 2 follow-up: replace with real libc when one lands.
 */

#include <stdint.h>
#include <stddef.h>

/* Forward decls to avoid pulling in libc headers (which don't
 * resolve cleanly on aarch64 phase 2). The slab allocator lives
 * in kernel/arch/aarch64/slab_stub.c; memset is in libc/include/string.h
 * (resolvable via -I libc/include from Task 2.2 commit 12d3720). */
extern void *kmalloc(size_t size);
extern void kfree(void *ptr);
extern void *memset(void *s, int c, size_t n);

void *calloc(size_t nmemb, size_t size)
{
    /* overflow check omitted: kernel/time/timer.c's only callers
     * pass (1, sizeof(timer_t)) where 1 * sizeof(timer_t) cannot
     * overflow on any plausible kernel config. */
    size_t total = nmemb * size;
    void *p = kmalloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

void free(void *ptr)
{
    kfree(ptr);
}