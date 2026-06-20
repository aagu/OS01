#include <stddef.h>
#include <stdlib.h>
long long strtoll(const char *s, char **e, int b) {
    return (long long)strtol(s, e, b);
}
unsigned long long strtoull(const char *s, char **e, int b) {
    return (unsigned long long)strtoul(s, e, b);
}
