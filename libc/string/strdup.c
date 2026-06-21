#include <string.h>
// #include <stdint.h>
#include <stdlib.h>

char * strdup(const char *src)
{
    size_t len = strlen(src);
    void *new = malloc(len + 1);
    if (new == NULL)
        return NULL;
    memcpy(new, src, len + 1);
    return (char *)new;
}