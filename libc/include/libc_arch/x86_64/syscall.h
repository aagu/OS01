#ifndef _LIBC_ARCH_X86_64_SYSCALL_H
#define _LIBC_ARCH_X86_64_SYSCALL_H

#include <stdint.h>

static inline int64_t __arch_syscall3(uint64_t nr,
                                      uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    int64_t ret;
    __asm__ volatile ("int $0x80"
        : "=a" (ret)
        : "a" (nr), "D" (arg1), "S" (arg2), "d" (arg3)
        : "memory");
    return ret;
}

static inline int64_t __arch_syscall4(uint64_t nr,
                                      uint64_t arg1, uint64_t arg2, uint64_t arg3,
                                      uint64_t arg4)
{
    int64_t ret;
    register uint64_t r10 __asm__("r10") = arg4;
    __asm__ volatile ("int $0x80"
        : "=a" (ret)
        : "a" (nr), "D" (arg1), "S" (arg2), "d" (arg3), "r" (r10)
        : "memory");
    return ret;
}

static inline int64_t __arch_syscall6(uint64_t nr,
                                      uint64_t arg1, uint64_t arg2, uint64_t arg3,
                                      uint64_t arg4, uint64_t arg5, uint64_t arg6)
{
    int64_t ret;
    register uint64_t r10 __asm__("r10") = arg4;
    register uint64_t r8  __asm__("r8")  = arg5;
    register uint64_t r9  __asm__("r9")  = arg6;
    __asm__ volatile ("int $0x80"
        : "=a" (ret)
        : "a" (nr), "D" (arg1), "S" (arg2), "d" (arg3),
          "r" (r10), "r" (r8), "r" (r9)
        : "memory");
    return ret;
}

#endif /* _LIBC_ARCH_X86_64_SYSCALL_H */
