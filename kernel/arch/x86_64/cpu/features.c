#include <arch/cpu_features.h>
#include <arch/x86_64/cpu_features.h>
#include <arch/cpuid.h>
#include <arch/x86_64/regs.h>

uint64_t x86_cpu_features_from_cpuid(uint32_t leaf1_ecx, uint32_t leaf1_edx,
                                     uint32_t ext_max_leaf, uint32_t ext1_ecx)
{
    uint64_t feat = 0;

    if (leaf1_edx & CPUID_FEAT_EDX_FPU)
        feat |= ARCH_CPU_FEAT_FPU;

    if ((leaf1_ecx & CPUID_FEAT_ECX_VMX) ||
        (ext_max_leaf >= 0x80000001U && (ext1_ecx & CPUID_EXT_FEAT_ECX_SVM)))
        feat |= ARCH_CPU_FEAT_VIRT;

    /* Hardware cache coherency is architectural on x86_64. */
    feat |= ARCH_CPU_FEAT_CACHE_COHERENCY;

    return feat;
}

uint64_t arch_cpu_features(void)
{
    static int cached = 0;
    static uint64_t cached_features = 0;

    if (__atomic_load_n(&cached, __ATOMIC_ACQUIRE))
        return __atomic_load_n(&cached_features, __ATOMIC_RELAXED);

    uint32_t eax = 0, ebx = 0, ecx1 = 0, edx1 = 0;
    cpuid(1U, &eax, &ebx, &ecx1, &edx1);

    uint32_t ext_max = 0, ext_ebx = 0, ext_ecx = 0, ext_edx = 0;
    cpuid(0x80000000U, &ext_max, &ext_ebx, &ext_ecx, &ext_edx);

    uint32_t ext1_ecx = 0;
    if (ext_max >= 0x80000001U) {
        uint32_t ext_eax = 0;
        cpuid(0x80000001U, &ext_eax, &ext_ebx, &ext1_ecx, &ext_edx);
    }

    uint64_t feat = x86_cpu_features_from_cpuid(ecx1, edx1, ext_max, ext1_ecx);
    __atomic_store_n(&cached_features, feat, __ATOMIC_RELAXED);
    __atomic_store_n(&cached, 1, __ATOMIC_RELEASE);
    return feat;
}
