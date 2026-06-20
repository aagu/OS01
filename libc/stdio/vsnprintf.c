#include <stddef.h>
#include <stdio.h>
int vsnprintf(char *b, unsigned long s, const char *fmt, __builtin_va_list ap) {
    (void)s; return vsprintf(b, fmt, ap);
}
