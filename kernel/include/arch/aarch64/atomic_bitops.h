#ifndef _KERNEL_ARCH_AARCH64_ATOMIC_BITOPS_H
#define _KERNEL_ARCH_AARCH64_ATOMIC_BITOPS_H

#include <stdint.h>

/* aarch64 strong override for arch_atomic_or_u64 / arch_atomic_and_u64.
 * Reference: kernel/arch/aarch64/cpu/atomic.c (deleted in this task).
 * LR/SC retry loop (ldaxr+stlxr+cbnz). Memory order: acquire-release.
 * Three independent =&r early-clobber constraints for [old]/[new_val]/[status]
 * (stlxr writes only the low 32 bits of [status]; sharing a register with
 * [new_val] would clobber the value being stored). */
static inline __attribute__((always_inline)) void
arch_atomic_or_u64(uint64_t *addr, uint64_t mask) {
    uint64_t old, new_val;
    uint32_t status;
    __asm__ __volatile__(
        "1: ldaxr   %[old],     [%[addr]]\n"
        "   orr    %[new_val], %[old], %[mask]\n"
        "   stlxr  %w[status], %[new_val], [%[addr]]\n"
        "   cbnz   %w[status], 1b\n"
        : [old]      "=&r"(old),
          [new_val]  "=&r"(new_val),
          [status]   "=&r"(status)
        : [addr] "r"(addr),
          [mask] "r"(mask)
        : "memory");
}

static inline __attribute__((always_inline)) void
arch_atomic_and_u64(uint64_t *addr, uint64_t mask) {
    uint64_t old, new_val;
    uint32_t status;
    __asm__ __volatile__(
        "1: ldaxr   %[old],     [%[addr]]\n"
        "   and    %[new_val], %[old], %[mask]\n"
        "   stlxr  %w[status], %[new_val], [%[addr]]\n"
        "   cbnz   %w[status], 1b\n"
        : [old]      "=&r"(old),
          [new_val]  "=&r"(new_val),
          [status]   "=&r"(status)
        : [addr] "r"(addr),
          [mask] "r"(mask)
        : "memory");
}

#endif
