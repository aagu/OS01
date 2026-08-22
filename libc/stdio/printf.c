#include <limits.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include "stdio_internal.h"

static int vprintf_core(const char *format, va_list ap)
{
    va_list cp;
    va_copy(cp, ap);
    size_t total = vformatter(NULL, 0, format, cp, 0);
    va_end(cp);
    if (total == SIZE_MAX || total > INT_MAX)
        return -1;

    char *buf = malloc(total + 1);
    if (!buf)
        return -1;

    size_t n = vformatter(buf, total + 1, format, ap, 1);
    int ret;
    if (n == SIZE_MAX) {
        ret = -1;
    } else {
        ssize_t w = write_all(1, buf, n);
        ret = (w < 0) ? -1 : (int)n;
    }
    free(buf);
    return ret;
}

/* vprintf is provided by stdio_extras.c (delegates to the safe vfprintf). */

int printf(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int r = vprintf_core(format, ap);
    va_end(ap);
    return r;
}
