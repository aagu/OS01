#include <stdio.h>
#include <stdarg.h>

int vprintf(const char *fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

int dprintf(int fd, const char *fmt, ...) {
    (void)fd; (void)fmt;
    return 0;
}
