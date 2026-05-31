#ifndef _KERNEL_ARCH_MSR_H
#define _KERNEL_ARCH_MSR_H

#include <stdint.h>

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

// ──────────────────────────────────────────────
//  Key APIC-related MSR numbers
// ──────────────────────────────────────────────

#define IA32_APIC_BASE  0x1B

// IA32_APIC_BASE bit definitions
#define APIC_BASE_BSP          (1UL << 8)   // Bootstrap Processor
#define APIC_BASE_ENABLE       (1UL << 11)  // APIC Global Enable (xAPIC)
#define APIC_BASE_X2APIC       (1UL << 10)  // x2APIC mode enable
#define APIC_BASE_ADDR_MASK    0xFFFFFFFFFFFFF000ULL  // bits 12-63: base address

#endif // _KERNEL_ARCH_MSR_H
