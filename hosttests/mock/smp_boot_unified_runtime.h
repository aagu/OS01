/* hosttests/mock/smp_boot_unified_runtime.h — preinclude runtime mock for
 * the smp-boot-unified production object (kernel/core/smp.c).
 *
 * Included via `-include` ONLY when compiling
 * $(TEST_BLD)/smp_boot_unified_production.o; the test translation unit
 * owns its own traceable definitions and does NOT use this header.
 *
 * The machine boundary is replaced, not reimplemented:
 *   - the real <arch/irq.h> installs a static-inline sti/no-op on the
 *     host (or pulls raw asm); skipping it lets the production object
 *     call the test TU's traceable arch_local_irq_enable() as a plain
 *     extern function;
 *   - the real <arch/barrier.h> maps arch_mb() to raw mfence/dmb; the
 *     test needs to observe the barrier as a call, so arch_mb() is
 *     routed through smp_test_arch_mb().
 */
#ifndef SMP_BOOT_UNIFIED_RUNTIME_H
#define SMP_BOOT_UNIFIED_RUNTIME_H

/* Skip kernel/include/arch/irq.h (guard _ARCH_IRQ_H): any transitive
 * include sees this extern declaration instead of the static inline. */
#define _ARCH_IRQ_H 1
void arch_local_irq_enable(void);

/* Skip kernel/include/arch/barrier.h (guard _ARCH_BARRIER_H) and map the
 * barrier onto a traceable test function. */
#define _ARCH_BARRIER_H 1
void smp_test_arch_mb(void);
#define arch_mb() smp_test_arch_mb()

#endif /* SMP_BOOT_UNIFIED_RUNTIME_H */
