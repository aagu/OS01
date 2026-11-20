#ifndef _LIBC_ARCH_AARCH64_SYSCALL_H
#define _LIBC_ARCH_AARCH64_SYSCALL_H

#include <stdint.h>

/* AArch64 EL0 native syscall: `svc #0`, number in x8, args in x0..x5,
 * return in x0 (matches kernel/arch/aarch64/intr/syscall.c). */
static inline int64_t __arch_syscall3(uint64_t nr,
                                      uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    register uint64_t x8 __asm__("x8") = nr;
    register uint64_t x0 __asm__("x0") = arg1;
    register uint64_t x1 __asm__("x1") = arg2;
    register uint64_t x2 __asm__("x2") = arg3;
    __asm__ volatile ("svc #0"
        : "=r" (x0)
        : "0" (x0), "r" (x1), "r" (x2), "r" (x8)
        : "memory", "cc");
    return (int64_t)x0;
}

static inline int64_t __arch_syscall4(uint64_t nr,
                                      uint64_t arg1, uint64_t arg2, uint64_t arg3,
                                      uint64_t arg4)
{
    register uint64_t x8 __asm__("x8") = nr;
    register uint64_t x0 __asm__("x0") = arg1;
    register uint64_t x1 __asm__("x1") = arg2;
    register uint64_t x2 __asm__("x2") = arg3;
    register uint64_t x3 __asm__("x3") = arg4;
    __asm__ volatile ("svc #0"
        : "=r" (x0)
        : "0" (x0), "r" (x1), "r" (x2), "r" (x3), "r" (x8)
        : "memory", "cc");
    return (int64_t)x0;
}

static inline int64_t __arch_syscall6(uint64_t nr,
                                      uint64_t arg1, uint64_t arg2, uint64_t arg3,
                                      uint64_t arg4, uint64_t arg5, uint64_t arg6)
{
    register uint64_t x8 __asm__("x8") = nr;
    register uint64_t x0 __asm__("x0") = arg1;
    register uint64_t x1 __asm__("x1") = arg2;
    register uint64_t x2 __asm__("x2") = arg3;
    register uint64_t x3 __asm__("x3") = arg4;
    register uint64_t x4 __asm__("x4") = arg5;
    register uint64_t x5 __asm__("x5") = arg6;
    __asm__ volatile ("svc #0"
        : "=r" (x0)
        : "0" (x0), "r" (x1), "r" (x2), "r" (x3), "r" (x4), "r" (x5),
          "r" (x8)
        : "memory", "cc");
    return (int64_t)x0;
}

#endif /* _LIBC_ARCH_AARCH64_SYSCALL_H */
