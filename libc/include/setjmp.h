#ifndef _SETJMP_H
#define _SETJMP_H 1

typedef unsigned long jmp_buf[16];

#ifdef __cplusplus
extern "C" {
#endif

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

#ifdef __cplusplus
}
#endif

#endif
