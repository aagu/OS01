#include <stddef.h>
#include <string.h>
void *mempcpy(void *d, const void *s, size_t n) {
    return (char*)memcpy(d, s, n) + n;
}
