#include <arch/cpu_features.h>
#include <arch/aarch64/cpu_features.h>
#include <arch/aarch64/regs.h>

uint64_t aarch64_cpu_features_from_id_regs(uint64_t id_aa64pfr0)
{
    uint64_t feat = 0;

    uint64_t fp_field = (id_aa64pfr0 & ID_AA64PFR0_EL1_FP_MASK) >> ID_AA64PFR0_EL1_FP_SHIFT;
    if (fp_field != ID_AA64PFR0_EL1_FP_NONE)
        feat |= ARCH_CPU_FEAT_FPU;

    uint64_t el2_field = (id_aa64pfr0 & ID_AA64PFR0_EL1_EL2_MASK) >> ID_AA64PFR0_EL1_EL2_SHIFT;
    if (el2_field != 0)
        feat |= ARCH_CPU_FEAT_VIRT;

    /* ARMv8-A Inner-Shareable hardware cache coherency is architectural. */
    feat |= ARCH_CPU_FEAT_CACHE_COHERENCY;

    return feat;
}

#ifndef OS01_HOST_TEST
uint64_t arch_cpu_features(void)
{
    static int cached = 0;
    static uint64_t cached_features = 0;

    if (__atomic_load_n(&cached, __ATOMIC_ACQUIRE))
        return __atomic_load_n(&cached_features, __ATOMIC_RELAXED);

    uint64_t pfr0 = 0;
    __asm__ __volatile__("mrs %0, id_aa64pfr0_el1" : "=r"(pfr0));

    uint64_t feat = aarch64_cpu_features_from_id_regs(pfr0);
    __atomic_store_n(&cached_features, feat, __ATOMIC_RELAXED);
    __atomic_store_n(&cached, 1, __ATOMIC_RELEASE);
    return feat;
}
#endif
