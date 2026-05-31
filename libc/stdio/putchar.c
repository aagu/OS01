#include <stdio.h>
#include <errno.h>
#include <sys/syscall.h>

#if defined(__is_libk)
#include <kernel/printk.h>
#endif

int putchar(int ic) {
#if defined(__is_libk)
	char c = (char) ic;
	putchark(WHITE,BLACK,c);
#else
	int64_t ret = syscall(SYS_putchar, (uint64_t)ic, 0, 0);
	if (ret < 0) {
		errno = (int)(-ret);
		return EOF;
	}
#endif
	return (unsigned char)ic;
}
