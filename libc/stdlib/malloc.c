#include <stdlib.h>
#if defined(__is_libk)
#include <kernel/slab.h>
#endif

void* malloc (size_t size)
{
#if defined(__is_libk)
	return kmalloc(size);
#else
	// TODO: Implement stdio and the write system call.
    return NULL;
#endif
}