#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdio/file.h>

#if defined(__is_libk)
#include <core/printk.h>
#endif

int putchar(int ic) {
#if defined(__is_libk)
    char c = (char) ic;
    putchark(WHITE,BLACK,c);
#else
    /* putchar writes to the standard output STREAM, so a failed write
     * is visible as stdout's error indicator and as EOF — the caller
     * never sees a fake success. */
    if (__stdio_write_byte(stdout, ic) == EOF)
        return EOF;
#endif
    return (unsigned char)ic;
}
