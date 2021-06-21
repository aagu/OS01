#include <stdlib.h>
#if defined(__is_libk)
#include <kernel/slab.h>
#endif

void free (void * ptr)
{
#if defined(__is_libk)
	kfree(ptr);
#else
	// TODO: Implement stdio and the write system call.
    return;
#endif
}