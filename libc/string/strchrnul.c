#include <stddef.h>
char *strchrnul(const char *s, int c) {
    while (*s && *s != c) s++;
    return (char*)s;
}
