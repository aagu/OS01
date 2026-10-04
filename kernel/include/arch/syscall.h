#ifndef _ARCH_SYSCALL_H
#define _ARCH_SYSCALL_H
#include <stdint.h>
int64_t arch_syscall_putchar(uint64_t ch);
__attribute__((noreturn)) void arch_syscall_reboot(int cmd);
#endif
