#ifndef _ARCH_CPUID_H
#define _ARCH_CPUID_H

#ifdef __x86_64__
#include <kernel/arch/x86_64/cpuid.h>
#elif defined(__aarch64__)
// aarch64 has no CPUID instruction — provide stubs that trap
#include <stdint.h>
static inline void cpuid(uint32_t leaf, uint32_t *eax, uint32_t *ebx,
                          uint32_t *ecx, uint32_t *edx)
{
    (void)leaf;
    (void)eax;
    (void)ebx;
    (void)ecx;
    (void)edx;
    __builtin_trap();
}
static inline void cpuid_count(uint32_t leaf, uint32_t subleaf,
                                uint32_t *eax, uint32_t *ebx,
                                uint32_t *ecx, uint32_t *edx)
{
    (void)leaf;
    (void)subleaf;
    (void)eax;
    (void)ebx;
    (void)ecx;
    (void)edx;
    __builtin_trap();
}
#else
#error "Unsupported architecture"
#endif

#endif /* _ARCH_CPUID_H */
