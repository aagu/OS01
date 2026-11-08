#ifndef _MOCK_BOOT_FAILURE_ARCH_CPU_H
#define _MOCK_BOOT_FAILURE_ARCH_CPU_H

void arch_cpu_halt(void);
static inline void arch_cpu_pause(void) {}
static inline void arch_nop(void) {}

#endif
