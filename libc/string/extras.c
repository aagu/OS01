#include <string.h>
#include <stdlib.h>
#include <stddef.h>

char *strchr(const char *s, int c)
{
    while (*s) {
        if (*s == (char)c)
            return (char *)s;
        s++;
    }
    if ((char)c == '\0')
        return (char *)s;
    return NULL;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    while (*s) {
        if (*s == (char)c)
            last = s;
        s++;
    }
    if ((char)c == '\0')
        return (char *)s;
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    if (!*needle)
        return (char *)haystack;

    while (*haystack) {
        const char *h = haystack;
        const char *n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n)
            return (char *)haystack;
        haystack++;
    }
    return NULL;
}

char *strtok(char *str, const char *delim)
{
    static char *save_ptr = NULL;
    return strtok_r(str, delim, &save_ptr);
}

char *strtok_r(char *str, const char *delim, char **save_ptr)
{
    char *end;

    if (str == NULL)
        str = *save_ptr;

    if (*str == '\0') {
        *save_ptr = str;
        return NULL;
    }

    // Scan leading delimiters
    str += strspn(str, delim);
    if (*str == '\0') {
        *save_ptr = str;
        return NULL;
    }

    // Find end of token
    end = str + strcspn(str, delim);
    if (*end == '\0') {
        *save_ptr = end;
        return str;
    }

    *end = '\0';
    *save_ptr = end + 1;
    return str;
}

char *strcat(char *dest, const char *src)
{
    char *d = dest;
    while (*d) d++;
    while (*src) *d++ = *src++;
    *d = '\0';
    return dest;
}

char *strncat(char *dest, const char *src, size_t n)
{
    char *d = dest;
    while (*d) d++;
    while (n-- && *src) *d++ = *src++;
    *d = '\0';
    return dest;
}

char *strncpy(char *dest, const char *src, size_t n)
{
    size_t i;
    for (i = 0; i < n && src[i]; i++)
        dest[i] = src[i];
    for (; i < n; i++)
        dest[i] = '\0';
    return dest;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (unsigned char)c)
            return (void *)(p + i);
    }
    return NULL;
}

size_t strspn(const char *s, const char *accept)
{
    size_t count = 0;
    while (*s) {
        const char *a = accept;
        while (*a && *a != *s) a++;
        if (!*a) break;
        count++;
        s++;
    }
    return count;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t count = 0;
    while (*s) {
        const char *r = reject;
        while (*r && *r != *s) r++;
        if (*r) break;
        count++;
        s++;
    }
    return count;
}

int strcasecmp(const char *s1, const char *s2) {
    while (*s1 && *s2) {
        char c1 = *s1, c2 = *s2;
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) break;
        s1++; s2++;
    }
    char c1 = *s1, c2 = *s2;
    if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
    if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
    return (int)(unsigned char)c1 - (int)(unsigned char)c2;
}

int strncasecmp(const char *s1, const char *s2, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c1 = s1[i], c2 = s2[i];
        if (c1 >= 'A' && c1 <= 'Z') c1 += 32;
        if (c2 >= 'A' && c2 <= 'Z') c2 += 32;
        if (c1 != c2) return (int)(unsigned char)c1 - (int)(unsigned char)c2;
        if (c1 == 0) break;
    }
    return 0;
}

char *strndup(const char *s, size_t n) {
    size_t len = 0;
    while (len < n && s[len]) len++;
    char *p = malloc(len + 1);
    if (!p) return NULL;
    for (size_t i = 0; i < len; i++) p[i] = s[i];
    p[len] = '\0';
    return p;
}

char *strpbrk(const char *s, const char *accept) {
    while (*s) {
        const char *a = accept;
        while (*a) {
            if (*s == *a) return (char *)s;
            a++;
        }
        s++;
    }
    return NULL;
}

void *bsearch(const void *key, const void *base, size_t nmemb,
              size_t size, int (*cmp)(const void *, const void *)) {
    size_t lo = 0, hi = nmemb;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        int r = cmp(key, (const char *)base + mid * size);
        if (r == 0) return (void *)((const char *)base + mid * size);
        if (r < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}
