#ifndef __PRINTK_H__
#define __PRINTK_H__

#include "types.h"

/*格式化输出函数， 来自deeppinkOS*/
/*
 * __builtin***()相关函数是gcc的内置变量和函数
 */
typedef __builtin_va_list va_list;
#define va_start(ap,last)	(__builtin_va_start(ap,last))
#define va_arg(ap,type)		(__builtin_va_arg(ap,type))
#define va_end(ap)

#define PANIC(msg) panic(msg, __FILE__, __LINE__);
#define ASSERT(b) ((b) ? (void)0 : panic_assert(__FILE__, __LINE__, #b))

extern void panic(const char *message, const char *file, int32 line);
extern void panic_assert(const char *file, int32 line, const char *desc);

void printk(const char *format,...);
int sprintf(char *s, const char *format, ...);
int vsprintf(char *buff, const char *format, va_list args);

#endif



