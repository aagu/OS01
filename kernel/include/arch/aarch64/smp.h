#ifndef OS01_AARCH64_SMP_H
#define OS01_AARCH64_SMP_H

#include <stdint.h>
#include <stdbool.h>
#include <arch/aarch64/gic.h>     /* gic_init / gic_cpu_init declared here */

uint32_t smp_boot_aps(void);
void secondary_idle(uint32_t cpu_id) __attribute__((noreturn));
bool test_spinlock_smp(uint32_t active);
void smp_bench_iter(uint32_t cpu_id, uint32_t iterations);

#endif
