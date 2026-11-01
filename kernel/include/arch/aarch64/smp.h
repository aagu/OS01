#ifndef OS01_AARCH64_SMP_H
#define OS01_AARCH64_SMP_H

#include <stdint.h>
#include <stdbool.h>
#include <arch/aarch64/gic.h>     /* gic_init / gic_cpu_init declared here */

uint32_t smp_boot_aps(void);
void secondary_idle(uint32_t cpu_id) __attribute__((noreturn));
bool test_spinlock_smp(uint32_t active);
void smp_bench_iter(uint32_t cpu_id, uint32_t iterations);

/* The frozen boot ACK mask (P1 spec §3.2): bit i is set only for CPUs
 * the BSP's smp_boot_run confirmed online.  Published once by
 * smp_boot_aps (including the UP and PSCI-unavailable shapes, where it
 * is exactly the BSP bit) and never mutated afterwards; readers use
 * acquire loads.  A CPU outside this mask is never admitted to the
 * scheduler, however late it reports. */
uint64_t aarch64_smp_accepted_mask(void);

#endif
