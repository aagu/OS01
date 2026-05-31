#ifndef _KERNEL_ARCH_CPUID_H
#define _KERNEL_ARCH_CPUID_H

#include <stdint.h>

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

// ──────────────────────────────────────────────
//  CPU feature bit definitions (CPU leaf 1)
// ──────────────────────────────────────────────

// EDX bits
#define CPUID_FEAT_EDX_APIC    (1 << 9)   // APIC supported on chip

// ECX bits
#define CPUID_FEAT_ECX_X2APIC  (1 << 21)  // x2APIC supported

#endif // _KERNEL_ARCH_CPUID_H
