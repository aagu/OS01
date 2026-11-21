#ifndef _KERNEL_ARCH_X86_64_CPU_FEATURES_H
#define _KERNEL_ARCH_X86_64_CPU_FEATURES_H

#include <stdint.h>

/*
 * Decode x86_64 CPUID feature registers into the unified ARCH_CPU_FEAT_*
 * bitmap. Pure helper exposed for unit testing without executing CPUID.
 */
uint64_t x86_cpu_features_from_cpuid(uint32_t leaf1_ecx, uint32_t leaf1_edx,
                                     uint32_t ext_max_leaf, uint32_t ext1_ecx);

#endif /* _KERNEL_ARCH_X86_64_CPU_FEATURES_H */
