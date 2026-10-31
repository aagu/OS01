#ifndef KERNEL_ARCH_AARCH64_TASK_ARCH_H
#define KERNEL_ARCH_AARCH64_TASK_ARCH_H

#include <arch/context.h>

/* AArch64 EL1 cooperative switch and initial-stack entry points.
 * C wrappers validate task metadata before invoking the assembly ABI. */
void aarch64_switch_context(task_t *actual_prev, thread_t *prev_ctx,
                            thread_t *next_ctx);
void aarch64_bsp_stack_enter(uint64_t idle_sp) __attribute__((noreturn));
void arch_aarch64_kthread_entry(void) __attribute__((noreturn));
void arch_aarch64_entry_prepare(void);
void arch_aarch64_idle_step(void);
void arch_aarch64_prepare_bsp_handoff(void);
void arch_aarch64_bsp_handoff(void) __attribute__((noreturn));
void aarch64_context_fatal(uint64_t code, uint64_t sp, uint64_t base,
                           uint64_t top) __attribute__((noreturn));

#endif /* KERNEL_ARCH_AARCH64_TASK_ARCH_H */
