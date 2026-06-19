#ifndef _KERNEL_PANIC_H
#define _KERNEL_PANIC_H

#include <stdarg.h>

void kpanic(const char * msg,...);

#endif