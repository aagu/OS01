// libc/stdio/sscanf.c — the variadic wrapper around the scanf core.
//
// The scanning itself lives in libc/stdio/vsscanf.c (T12): the format
// parser, the input cursor and the typed stores.  This file only does
// the va_start / va_end dance, so that the core can be reused by any
// caller that already owns a va_list.

#include <stdarg.h>
#include <stdio.h>

int sscanf(const char *str, const char *fmt, ...)
{
    va_list args;
    int n;

    va_start(args, fmt);
    n = vsscanf(str, fmt, args);
    va_end(args);
    return n;
}
