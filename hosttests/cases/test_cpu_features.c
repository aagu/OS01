/* @os01-test
 * id: test_cpu_features
 * layer: host
 * subsystem: boot
 * timing: strict
 * tier: pr
 * link: cpu-features
 * scenarios: [arch.cpu_features]
 */
/* hosttests/cases/test_cpu_features.c — Unit tests for the unified
 * arch_cpu_features() facade and x86_64 / aarch64 register decoders.
 */

#include "test_framework.h"
#include <stdint.h>

#include <arch/cpu_features.h>
#include <arch/x86_64/cpu_features.h>
#include <arch/aarch64/cpu_features.h>
#include <arch/x86_64/regs.h>

#define A64_PFR0_EL2(v) (((uint64_t)(v) & 0xFULL) << 8)
#define A64_PFR0_FP(v)  (((uint64_t)(v) & 0xFULL) << 16)

TEST_FUNC(test_feature_bit_layout)
{
    uint64_t all = ARCH_CPU_FEAT_FPU | ARCH_CPU_FEAT_VIRT | ARCH_CPU_FEAT_CACHE_COHERENCY;
    assert_true(ARCH_CPU_FEAT_FPU != 0ULL);
    assert_true(ARCH_CPU_FEAT_VIRT != 0ULL);
    assert_true(ARCH_CPU_FEAT_CACHE_COHERENCY != 0ULL);
    assert_eq(0ULL, ARCH_CPU_FEAT_FPU & ARCH_CPU_FEAT_VIRT);
    assert_eq(0ULL, ARCH_CPU_FEAT_FPU & ARCH_CPU_FEAT_CACHE_COHERENCY);
    assert_eq(0ULL, ARCH_CPU_FEAT_VIRT & ARCH_CPU_FEAT_CACHE_COHERENCY);
    assert_eq(0x7ULL, all);
}

TEST_FUNC(test_x86_cpuid_decode)
{
    uint64_t f;

    f = x86_cpu_features_from_cpuid(0U, 0U, 0U, 0U);
    assert_eq(ARCH_CPU_FEAT_CACHE_COHERENCY, f);

    f = x86_cpu_features_from_cpuid(0U, CPUID_FEAT_EDX_FPU, 0U, 0U);
    assert_eq(ARCH_CPU_FEAT_FPU | ARCH_CPU_FEAT_CACHE_COHERENCY, f);

    /* Intel VMX */
    f = x86_cpu_features_from_cpuid(CPUID_FEAT_ECX_VMX, CPUID_FEAT_EDX_FPU, 0U, 0U);
    assert_eq(ARCH_CPU_FEAT_FPU | ARCH_CPU_FEAT_VIRT | ARCH_CPU_FEAT_CACHE_COHERENCY, f);

    /* AMD SVM with valid extended leaf */
    f = x86_cpu_features_from_cpuid(0U, CPUID_FEAT_EDX_FPU,
                                    0x80000001U, CPUID_EXT_FEAT_ECX_SVM);
    assert_eq(ARCH_CPU_FEAT_FPU | ARCH_CPU_FEAT_VIRT | ARCH_CPU_FEAT_CACHE_COHERENCY, f);

    /* AMD SVM bit ignored when ext_max_leaf < 0x80000001 */
    f = x86_cpu_features_from_cpuid(0U, CPUID_FEAT_EDX_FPU,
                                    0x80000000U, CPUID_EXT_FEAT_ECX_SVM);
    assert_eq(ARCH_CPU_FEAT_FPU | ARCH_CPU_FEAT_CACHE_COHERENCY, f);
}

TEST_FUNC(test_aarch64_id_reg_decode)
{
    uint64_t f;

    /* FP=0xF (none), EL2=0 (none) */
    f = aarch64_cpu_features_from_id_regs(A64_PFR0_FP(0xF) | A64_PFR0_EL2(0));
    assert_eq(ARCH_CPU_FEAT_CACHE_COHERENCY, f);

    /* FP=0 (present), EL2=0 */
    f = aarch64_cpu_features_from_id_regs(A64_PFR0_FP(0) | A64_PFR0_EL2(0));
    assert_eq(ARCH_CPU_FEAT_FPU | ARCH_CPU_FEAT_CACHE_COHERENCY, f);

    /* FP=1 (FP16), EL2=1 (AArch64 EL2) */
    f = aarch64_cpu_features_from_id_regs(A64_PFR0_FP(1) | A64_PFR0_EL2(1));
    assert_eq(ARCH_CPU_FEAT_FPU | ARCH_CPU_FEAT_VIRT | ARCH_CPU_FEAT_CACHE_COHERENCY, f);

    /* FP=0xF (none), EL2=2 (AArch64+AArch32 EL2) */
    f = aarch64_cpu_features_from_id_regs(A64_PFR0_FP(0xF) | A64_PFR0_EL2(2));
    assert_eq(ARCH_CPU_FEAT_VIRT | ARCH_CPU_FEAT_CACHE_COHERENCY, f);
}

TEST_FUNC(test_arch_cpu_features_live)
{
    uint64_t f1 = arch_cpu_features();
    uint64_t f2 = arch_cpu_features();

    assert_eq(f1, f2);
    assert_true(arch_cpu_has_fpu());
    assert_true(arch_cpu_has_cache_coherency());
    assert_eq(0ULL, f1 & ~(ARCH_CPU_FEAT_FPU | ARCH_CPU_FEAT_VIRT | ARCH_CPU_FEAT_CACHE_COHERENCY));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_feature_bit_layout),
    TEST_ENTRY(test_x86_cpuid_decode),
    TEST_ENTRY(test_aarch64_id_reg_decode),
    TEST_ENTRY(test_arch_cpu_features_live),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
