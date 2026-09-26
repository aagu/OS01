#ifndef _KERNEL_ARCH_X86_64_ATOMIC_BITOPS_H
#define _KERNEL_ARCH_X86_64_ATOMIC_BITOPS_H

#include <stdint.h>

/* x86_64 strong override for arch_atomic_or_u64 / arch_atomic_and_u64.
 * Reference: kernel/arch/x86_64/cpu/atomic.c (deleted in this task).
 * Memory order: acquire-release (full fence on LOCK prefix in practice).
 * Hot path: must remain inlined even at -O0; previous CI flake recorded
 * in kernel/intr/softirq.c historical comment. */
static inline __attribute__((always_inline)) void
arch_atomic_or_u64(uint64_t *addr, uint64_t mask) {
    __asm__ __volatile__("lock orq %0, (%1)"
                         :: "r"(mask), "r"(addr) : "memory");
}

static inline __attribute__((always_inline)) void
arch_atomic_and_u64(uint64_t *addr, uint64_t mask) {
    __asm__ __volatile__("lock andq %0, (%1)"
                         :: "r"(mask), "r"(addr) : "memory");
}

#endif
