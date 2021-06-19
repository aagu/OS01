#include <stdlib.h>
#include <string.h>

void* calloc (size_t size)
{
    void * ptr = malloc(size);
    if (ptr == NULL) // alloc failed
        return NULL;
    memset(ptr, 0x0, size);
    return ptr;
}