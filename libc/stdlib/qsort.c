#include <stdlib.h>
#include <string.h>

// Simple quicksort implementation
static void swap(char *a, char *b, size_t size)
{
    char tmp[64];
    while (size > 0) {
        size_t chunk = size > 64 ? 64 : size;
        memcpy(tmp, a, chunk);
        memcpy(a, b, chunk);
        memcpy(b, tmp, chunk);
        a += chunk;
        b += chunk;
        size -= chunk;
    }
}

void qsort(void *base, size_t nmemb, size_t size,
           int (*compar)(const void *, const void *))
{
    if (nmemb < 2) return;

    char *arr = (char *)base;

    // Use last element as pivot
    char *pivot = arr + (nmemb - 1) * size;
    char *i = arr;

    for (size_t j = 0; j < nmemb - 1; j++) {
        char *elem = arr + j * size;
        if (compar(elem, pivot) < 0) {
            if (i != elem)
                swap(i, elem, size);
            i += size;
        }
    }

    swap(i, pivot, size);

    // Recursively sort partitions
    size_t left_count = (size_t)(i - arr) / size;
    if (left_count > 1)
        qsort(arr, left_count, size, compar);

    size_t right_count = nmemb - left_count - 1;
    if (right_count > 1)
        qsort(i + size, right_count, size, compar);
}
