#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
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
	syscall(SYS_exit, 1, 0, 0);
	__builtin_unreachable();
#endif
}