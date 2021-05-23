#include <stdio.h>

#if defined(__is_libk)
#include <kernel/printk.h>
#endif

int putchar(int ic) {
#if defined(__is_libk)
	char c = (char) ic;
	putchark(WHITE,BLACK,c);
#else
	// TODO: Implement stdio and the write system call.
#endif
	return ic;
}
