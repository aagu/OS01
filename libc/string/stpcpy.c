#include <string.h>
char *stpcpy(char *d, const char *s) {
    while (*s) *d++ = *s++;
    *d = 0;
    return d;
}
