#ifndef _KERNEL_ARCH_CPUID_H
#define _KERNEL_ARCH_CPUID_H

#include <stdint.h>
#include <kernel/arch/x86_64/regs.h>   // CPUID feature bit definitions

// ──────────────────────────────────────────────
//  CPUID instruction wrapper
// ──────────────────────────────────────────────

static inline void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                         uint32_t *ecx, uint32_t *edx)
{
    __asm__ __volatile__(
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf)
    );
}

// Convenience: CPUID with sub-leaf in ECX
static inline void cpuid_count(uint32_t leaf, uint32_t subleaf,
                               uint32_t *eax, uint32_t *ebx,
                               uint32_t *ecx, uint32_t *edx)
{
    __asm__ __volatile__(
        "cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf), "c"(subleaf)
    );
}

#endif // _KERNEL_ARCH_CPUID_H
