#ifndef _LIBC_ARCH_AARCH64_ATOMIC_H
#define _LIBC_ARCH_AARCH64_ATOMIC_H

#include <stdint.h>

/* ARMv8.0-A LL/SC acquire-release primitives (no LSE requirement).
 * Matches kernel/include/arch/aarch64/atomic_bitops.h register constraints. */
static inline int libc_atomic_xchg32(volatile int *ptr, int val)
{
    int old_val;
    uint32_t status;
    __asm__ __volatile__(
        "1: ldaxr  %w[old], [%[addr]]\n"
        "   stlxr  %w[status], %w[val], [%[addr]]\n"
        "   cbnz   %w[status], 1b\n"
        : [old]    "=&r"(old_val),
          [status] "=&r"(status)
        : [addr] "r"(ptr),
          [val]  "r"(val)
        : "memory"
    );
    return old_val;
}

static inline int libc_atomic_cas32(volatile int *ptr, int old_val, int new_val)
{
    int cur;
    uint32_t status;
    __asm__ __volatile__(
        "1: ldaxr  %w[cur], [%[addr]]\n"
        "   cmp    %w[cur], %w[old]\n"
        "   b.ne   2f\n"
        "   stlxr  %w[status], %w[newv], [%[addr]]\n"
        "   cbnz   %w[status], 1b\n"
        "   b      3f\n"
        "2: clrex\n"
        "3:\n"
        : [cur]    "=&r"(cur),
          [status] "=&r"(status)
        : [addr] "r"(ptr),
          [old]  "r"(old_val),
          [newv] "r"(new_val)
        : "cc", "memory"
    );
    return cur;
}

static inline void libc_atomic_store32_release(volatile int *ptr, int val)
{
    __asm__ __volatile__(
        "stlr %w[val], [%[addr]]"
        :
        : [val]  "r"(val),
          [addr] "r"(ptr)
        : "memory"
    );
}

#endif /* _LIBC_ARCH_AARCH64_ATOMIC_H */
