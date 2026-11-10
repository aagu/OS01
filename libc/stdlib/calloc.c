#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#if !defined(__is_libk)
#include <errno.h>
#endif

void* calloc (size_t n, size_t size)
{
    size_t total;
    void * ptr;

    // The element count times the element size must not wrap around: a
    // wrapped total would be allocated (and zeroed) as a much smaller
    // block than the caller asked for.
    if (n != 0 && size > SIZE_MAX / n) {
#if !defined(__is_libk)
        errno = ENOMEM;
#endif
        return NULL;
    }
    total = n * size;

    ptr = malloc(total);
    if (ptr == NULL)
        return NULL;
    memset(ptr, 0x0, total);
    return ptr;
}
