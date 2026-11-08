// kernel/include/arch/subsys.h
#ifndef _ARCH_SUBSYS_H
#define _ARCH_SUBSYS_H

#include <stdint.h>

// RSDP address — set by kernel_main before arch_register_subsys
extern uint64_t arch_boot_rsdp;

// Register arch subsystems for phases 3-6
void arch_register_subsys(void);

#endif
