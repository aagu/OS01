#ifndef _MALLOC_H
#define _MALLOC_H
#include <stddef.h>
extern void *malloc(size_t);
extern void free(void *);
extern void *calloc(size_t, size_t);
extern void *realloc(void *, size_t);
#endif
