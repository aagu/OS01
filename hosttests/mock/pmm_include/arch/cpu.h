/* test/mock/pmm_include/kernel/arch/cpu.h — host shadow of
 * kernel/arch/cpu.h for compiling the REAL kernel/memory/pmm.c in
 * the host test suite (test_pmm_ram_rel_index).
 *
 * The production header's arch_cpu_halt() executes hlt(); on the
 * host a fatal path must trap visibly instead. pmm.c only consumes
 * arch_cpu_halt from this header.
 */
#ifndef _ARCH_CPU_H
#define _ARCH_CPU_H

static inline void arch_cpu_halt(void) { __builtin_trap(); }

#endif /* _ARCH_CPU_H */
