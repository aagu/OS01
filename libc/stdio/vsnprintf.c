#include <stddef.h>
#include <stdio.h>
#include <string.h>

int vsnprintf(char *b, unsigned long s, const char *fmt, __builtin_va_list ap) {
    /* NULL buffer with size 0: compute length without writing */
    if (!b && s == 0) {
        static char dummy[4096];
        return vsprintf(dummy, fmt, ap);
    }
    /* Normal case: write up to s-1 chars + null */
    int len = vsprintf(b, fmt, ap);
    if ((unsigned long)len >= s && s > 0)
        b[s - 1] = '\0';
    return len;
}
