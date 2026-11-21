#ifndef _ARCH_CPU_FEATURES_H
#define _ARCH_CPU_FEATURES_H

#include <stdbool.h>
#include <stdint.h>

/*
 * arch/cpu_features.h — Arch-neutral CPU hardware feature bitmap facade.
 *
 * Returns a unified 64-bit bitmap of hardware capabilities detected from
 * architecture-specific identification registers (x86_64 CPUID; aarch64
 * ID_AA64* system registers).
 *
 * Unlike <arch/features.h> (compile-time OS01 subsystem gates) and
 * <arch/cpuid.h> (raw x86 CPUID instruction wrapper), this facade reports
 * runtime-probed hardware CPU features uniformly across architectures.
 */

#define ARCH_CPU_FEAT_FPU              (1ULL << 0) /* Hardware floating-point unit */
#define ARCH_CPU_FEAT_VIRT             (1ULL << 1) /* Hardware virtualization (VMX/SVM or EL2) */
#define ARCH_CPU_FEAT_CACHE_COHERENCY  (1ULL << 2) /* Hardware SMP cache coherency */

/*
 * Probe and return the unified CPU feature bitmap.
 * Results are cached after the first probe; safe to call from any context
 * after early boot, including SMP APs.
 */
uint64_t arch_cpu_features(void);

static inline bool arch_cpu_has_fpu(void)
{
    return (arch_cpu_features() & ARCH_CPU_FEAT_FPU) != 0;
}

static inline bool arch_cpu_has_virt(void)
{
    return (arch_cpu_features() & ARCH_CPU_FEAT_VIRT) != 0;
}

static inline bool arch_cpu_has_cache_coherency(void)
{
    return (arch_cpu_features() & ARCH_CPU_FEAT_CACHE_COHERENCY) != 0;
}

#endif /* _ARCH_CPU_FEATURES_H */
