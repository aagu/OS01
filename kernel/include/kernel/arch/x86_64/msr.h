#ifndef _KERNEL_ARCH_MSR_H
#define _KERNEL_ARCH_MSR_H

#include <stdint.h>
#include <kernel/arch/x86_64/regs.h>   // MSR numbers, APIC_BASE_* bits

// ──────────────────────────────────────────────
//  MSR (Model-Specific Register) access
// ──────────────────────────────────────────────

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t low, high;
    __asm__ __volatile__(
        "rdmsr"
        : "=a"(low), "=d"(high)
        : "c"(msr)
    );
    return ((uint64_t)high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t value)
{
    uint32_t low  = (uint32_t)(value & 0xFFFFFFFF);
    uint32_t high = (uint32_t)(value >> 32);
    __asm__ __volatile__(
        "wrmsr"
        :
        : "a"(low), "d"(high), "c"(msr)
    );
}

#endif // _KERNEL_ARCH_MSR_H
