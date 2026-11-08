// libc/stdio/asprintf.c — the varargs spelling of vasprintf (T9).
//
// vasprintf (vasprintf.c) owns the two-pass format core: measure with
// a va_copy, format with the original list, fail loudly on truncation
// past INT_MAX, allocation failure or formatter error (*strp stays
// NULL on every failure).  This file is only the va_list bridge.

#include <stdarg.h>
#include <stdio.h>

int asprintf(char **strp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vasprintf(strp, fmt, ap);
    va_end(ap);
    return r;
}
