// kernel/include/kernel/arch/subsys.h
#ifndef _ARCH_SUBSYS_H
#define _ARCH_SUBSYS_H

#include <stdint.h>

// RSDP address — set by kernel_main before arch_register_subsys
extern uint64_t arch_boot_rsdp;

// Register arch subsystems for phases 3-6
void arch_register_subsys(void);

// Per-CPU subsystem registration (after SMP bringup)
void arch_register_subsys_percpu(void);

#endif
