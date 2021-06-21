#include <string.h>

char* strcpy(char* dest, const char* src)
{
    if (dest == NULL || src == NULL)
        return NULL;
    do
    {
        *dest++ = *src++;
    } while (*src != 0);
    
    return dest;
}