#include <string.h>

char* strcpy(char* dest, const char* src)
{
    char *original = dest;
    if (dest == NULL || src == NULL)
        return NULL;
    while ((*dest++ = *src++) != '\0')
        ;
    return original;
}
