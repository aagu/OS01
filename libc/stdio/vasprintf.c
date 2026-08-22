#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <limits.h>
#include <stdint.h>
#include "stdio_internal.h"

int vasprintf(char **strp, const char *fmt, va_list ap)
{
    *strp = NULL;

    va_list cp;
    va_copy(cp, ap);
    size_t total = vformatter(NULL, 0, fmt, cp, 0);
    va_end(cp);
    if (total == SIZE_MAX || total > INT_MAX)
        return -1;

    char *buf = malloc(total + 1);
    if (!buf)
        return -1;

    size_t n = vformatter(buf, total + 1, fmt, ap, 1);
    if (n == SIZE_MAX) {
        free(buf);
        *strp = NULL;
        return -1;
    }
    buf[n] = '\0';
    *strp = buf;
    return (int)n;
}
