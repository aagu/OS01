#ifndef _ARCH_MSR_H
#define _ARCH_MSR_H

#ifdef __x86_64__
#include <kernel/arch/x86_64/msr.h>
#elif defined(__aarch64__)
// aarch64 has no x86-style MSRs — provide stubs that trap
#include <stdint.h>
static inline uint64_t rdmsr(uint32_t msr)
{
    (void)msr;
    __builtin_trap();
    return 0;
}
static inline void wrmsr(uint32_t msr, uint64_t value)
{
    (void)msr;
    (void)value;
    __builtin_trap();
}
#else
#error "Unsupported architecture"
#endif

#endif /* _ARCH_MSR_H */
