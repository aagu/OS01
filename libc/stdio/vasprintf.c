#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

int vasprintf(char **strp, const char *fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int len = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (len < 0) { *strp = NULL; return -1; }
    *strp = malloc(len + 1);
    if (!*strp) return -1;
    return vsnprintf(*strp, len + 1, fmt, ap);
}
