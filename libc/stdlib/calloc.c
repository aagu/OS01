#include <stdlib.h>
#include <string.h>

void* calloc (size_t n, size_t size)
{
    size_t total = n * size;
    void * ptr = malloc(total);
    if (ptr == NULL)
        return NULL;
    memset(ptr, 0x0, total);
    return ptr;
}
