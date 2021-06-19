#include <stdio.h>
#include <stdlib.h>
#if defined(__is_libk)
#include <kernel/panic.h>
#endif
 
__attribute__((__noreturn__))
void abort(void) {
#if defined(__is_libk)
	// TODO: Add proper kernel panic.
	// printf("kernel: panic: abort()\n");
	kpanic("[kernel panic]\n");
#else
	// TODO: Abnormally terminate the process as if by SIGABRT.
	printf("abort()\n");
#endif
	while (1) { }
	__builtin_unreachable();
}