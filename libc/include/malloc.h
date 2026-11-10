#ifndef _MALLOC_H
#define _MALLOC_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
extern void *malloc(size_t);
extern void free(void *);
extern void *calloc(size_t, size_t);
extern void *realloc(void *, size_t);
#ifndef __is_libk
/* Aligned allocation is a user-libc capability: the kernel side (libk)
 * keeps the plain slab allocator and does not define these symbols, so
 * the declarations are hidden in __is_libk mode.
 *
 * posix_memalign requires an alignment that is a power of two and a
 * multiple of sizeof(void *); it reports a failure through its return
 * value (EINVAL / ENOMEM) and never through errno.
 * aligned_alloc additionally requires a size that is a multiple of the
 * alignment. */
extern int posix_memalign(void **, size_t, size_t);
extern void *aligned_alloc(size_t, size_t);
#endif
#ifdef __cplusplus
}
#endif
#endif
