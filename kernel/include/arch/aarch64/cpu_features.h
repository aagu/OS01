#ifndef _KERNEL_ARCH_AARCH64_CPU_FEATURES_H
#define _KERNEL_ARCH_AARCH64_CPU_FEATURES_H

#include <stdint.h>

/*
 * Decode AArch64 ID_AA64PFR0_EL1 into the unified ARCH_CPU_FEAT_* bitmap.
 * Pure helper exposed for unit testing without executing MRS.
 */
uint64_t aarch64_cpu_features_from_id_regs(uint64_t id_aa64pfr0);

#endif /* _KERNEL_ARCH_AARCH64_CPU_FEATURES_H */
