#pragma once
/* BOOT-2 mock arch/cpu.h: privileged machine boundary replaced by the
 * runtime models (GS poison, fake TSC).  Nothing here executes x86
 * privileged instructions. */
#include <stdint.h>
#include <stdbool.h>
#include <config/limits.h>

void arch_cpu_halt(void);
void arch_cpu_pause(void);
static inline void arch_nop(void) {}

uint64_t arch_cycle_counter(void);

/* GS base install → runtime poison model (records + enables reads). */
void boot2_test_install_gs(void *base);
static inline void arch_set_percpu_base(void *ptr) { boot2_test_install_gs(ptr); }

static inline void arch_cpu_enable_nx(void) {}
uint64_t arch_cycle_freq(void);
