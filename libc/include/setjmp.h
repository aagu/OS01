#ifndef _SETJMP_H
#define _SETJMP_H 1

#include <libc_arch/setjmp.h>

#ifdef __cplusplus
extern "C" {
#endif

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

#ifdef __cplusplus
}
#endif

#endif
