#ifndef _SYS_SYSCALL_H
#define _SYS_SYSCALL_H

#include <stdint.h>

#define SYS_putchar  0
#define SYS_write    1
#define SYS_exit     2
#define SYS_brk      3
#define SYS_getpid   4
#define SYS_exec     5

static inline int64_t syscall(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    int64_t ret;
    __asm__ volatile ("int $0x80"
        : "=a" (ret)
        : "a" (nr), "D" (arg1), "S" (arg2), "d" (arg3)
        : "memory");
    return ret;
}

static inline int64_t exec(const char *path)
{
    return syscall(SYS_exec, (uint64_t)path, 0, 0);
}

#endif
