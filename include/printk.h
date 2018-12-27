#ifndef __PRINTK_H__
#define __PRINTK_H__

/*格式化输出函数， 来自deeppinkOS*/
/*
 * __builtin***()相关函数是gcc的内置变量和函数
 */
typedef __builtin_va_list va_list;
#define va_start(ap,last)	(__builtin_va_start(ap,last))
#define va_arg(ap,type)		(__builtin_va_arg(ap,type))
#define va_end(ap)

void prink(const char *format,...);
int vsprintf(char *buff, const char *format, va_list args);

#endif



