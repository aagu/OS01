#ifndef OS01_AARCH64_SMP_H
#define OS01_AARCH64_SMP_H

#include <stdint.h>
#include <stdbool.h>
#include <arch/aarch64/gic.h>     /* gic_init / gic_cpu_init declared here */

/* smp_boot_aps()/secondary_idle() moved to the unified arch/smp.h
 * facade (spec 2026-11-19): the aarch64 strong overrides
 * (arch_smp_boot_aps, arch_smp_last_status, arch_smp_ap_*,
 * arch_smp_post_boot_hook) live in arch/aarch64/smp/smp.c and are
 * declared by <arch/smp.h>. */
bool test_spinlock_smp(uint32_t active);
void smp_bench_iter(uint32_t cpu_id, uint32_t iterations);

/* The frozen boot ACK mask (P1 spec §3.2): bit i is set only for CPUs
 * the BSP's smp_boot_run confirmed online.  Published once by
 * arch_smp_boot_aps on its single success path (all requested CPUs
 * acknowledged) and never mutated afterwards; readers use acquire
 * loads.  A CPU outside this mask is never admitted to the scheduler,
 * however late it reports. */
uint64_t aarch64_smp_accepted_mask(void);

#endif
