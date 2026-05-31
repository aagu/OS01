#include <stdlib.h>
#include <errno.h>
#include <sys/syscall.h>
#if defined(__is_libk)
#include <kernel/slab.h>
#endif

void* malloc (size_t size)
{
#if defined(__is_libk)
	return kmalloc(size);
#else
	// Simple bump allocator via SYS_brk
	static uint64_t heap_top = 0;

	if (size == 0)
		return NULL;

	if (heap_top == 0) {
		// First call: query current break as heap base
		heap_top = (uint64_t)syscall(SYS_brk, 0, 0, 0);
		if (heap_top == 0)
			return NULL;
	}

	// Align to 16 bytes
	size = (size + 15) & ~(size_t)15;

	uint64_t new_top = heap_top + size;
	if (new_top < heap_top) {
		errno = ENOMEM;  // overflow
		return NULL;
	}

	uint64_t actual = (uint64_t)syscall(SYS_brk, new_top, 0, 0);
	if (actual < new_top) {
		errno = ENOMEM;
		return NULL;
	}

	void *ptr = (void *)heap_top;
	heap_top = actual;
	return ptr;
#endif
}