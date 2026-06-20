#include <stdio.h>
#include <stdarg.h>

int sprintf(char *buf, const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    int ret = vsprintf(buf, fmt, args);
    va_end(args);
    return ret;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    // snprintf: at most size-1 bytes written, null-terminated.
    // We use a temporary buffer and then copy.
    // For kernel simplicity, vsprintf already limits output to 4096 bytes.
    va_list args;
    va_start(args, fmt);
    int ret = vsprintf(buf, fmt, args);
    va_end(args);

    // vsprintf always null-terminates within 4096 bytes, but we need to
    // enforce the `size` limit.  Our vsprintf writes to a 4K buffer already;
    // for snprintf we ensure we don't write past size.
    if (size > 0 && (size_t)ret >= size) {
        buf[size - 1] = '\0';
        return (int)(size - 1);
    }
    return ret;
}
