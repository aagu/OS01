#ifndef _STDDEF_H
#define _STDDEF_H 1

#include <sys/cdefs.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NULL ((void *)0)

#define offsetof(type, member) __builtin_offsetof(type, member)

typedef __SIZE_TYPE__ size_t;
typedef __PTRDIFF_TYPE__ ptrdiff_t;

#ifdef __cplusplus
}
#endif

#endif
