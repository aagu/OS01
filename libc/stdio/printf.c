#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int printf(const char* restrict format, ...) {
    static char buf[4096];
    int len = 0;
    va_list args;

    va_start(args, format);
    len = vsprintf(buf, format, args);
    va_end(args);

    if (len > 0)
        write(1, buf, len);

    return len;
}
