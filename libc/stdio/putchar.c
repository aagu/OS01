#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#if defined(__is_libk)
#include <kernel/printk.h>
#endif

int putchar(int ic) {
#if defined(__is_libk)
    char c = (char) ic;
    putchark(WHITE,BLACK,c);
#else
    char c = (char)ic;
    int64_t ret = write(1, &c, 1);
    if (ret < 0) {
        errno = (int)(-ret);
        return EOF;
    }
#endif
    return (unsigned char)ic;
}
