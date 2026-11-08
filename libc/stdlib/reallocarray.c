#include <stdlib.h>
#include <stdint.h>

#if !defined(__is_libk)
#include <errno.h>
#endif

// Overflow-checked realloc: the new size is the element count times the
// element size, and the product must not wrap around.  A failing call
// returns NULL and leaves the old block untouched.
//
// The function does not touch the allocator's private block layout, so it
// is shared with the kernel-side libk.  libk has no user-space errno: it
// refuses the overflow with a plain NULL.
void *reallocarray(void *ptr, size_t nmemb, size_t size)
{
    if (nmemb != 0 && size > SIZE_MAX / nmemb) {
#if !defined(__is_libk)
        errno = ENOMEM;
#endif
        return NULL;
    }
    return realloc(ptr, nmemb * size);
}
