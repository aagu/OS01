#include <stdlib.h>
#if defined(__is_libk)
#include <memory/slab.h>
#endif

void free (void * ptr)
{
#if defined(__is_libk)
	kfree(ptr);
#else
	// Bump allocator — free is a no-op
	(void)ptr;
	return;
#endif
}