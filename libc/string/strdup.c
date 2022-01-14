#include <string.h>
// #include <stdint.h>
#include <stdlib.h>

char * strdup(const char *src)
{
    size_t len = strlen(src);
    void *new = malloc(len);

    if (new == NULL)
        return NULL;
    
    return (char *) memcpy(new, src, len);
}