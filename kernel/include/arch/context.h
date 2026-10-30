#ifndef KERNEL_ARCH_CONTEXT_H
#define KERNEL_ARCH_CONTEXT_H

/* Architecture context facade (aarch64 EL1 scheduler spec §4/§5.1).
 *
 * Single entry point for everything architecture-specific about a task's
 * execution context: the thread_t layout, the `current` accessor, the
 * switch operation, context validation and the architecture part of a
 * hang/watchdog dump.  Common scheduler code includes ONLY this header —
 * never a per-arch path directly — so it stays arch-neutral.
 *
 * STACK_SIZE is defined here (outside the __ASSEMBLER__ guard) so both C
 * and the Phase 3 assembly see the same task_union stack size.  It must
 * stay 32 KiB on both architectures (spec §6).
 *
 * `current` is valid ONLY while running on a task_union stack: never on
 * the BSP boot stack, never on an AP boot/work stack, never on an
 * exception (IST) stack.  There is no task_t at those addresses — masking
 * the SP there yields garbage, not a task (spec §6).
 */
#define STACK_SIZE (32 * 1024)

#ifndef __ASSEMBLER__
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
struct task_struct;
typedef struct task_struct task_t;
#endif

#ifdef __x86_64__
# include <arch/x86_64/context.h>
#elif defined(__aarch64__)
# include <arch/aarch64/context.h>
#else
# error "Unsupported architecture"
#endif

#ifndef __ASSEMBLER__
/* current is a per-arch static inline; x86 switch is a macro. */
#ifdef __aarch64__
void arch_task_switch(task_t *prev, task_t *next);
#endif
bool arch_task_context_valid(task_t *t, const char **reason);
void arch_dump_task_context(task_t *t);
void arch_task_check_prev(task_t *t);
void arch_task_prepare_first_switch(task_t *t, void (*idle_entry)(void));
#endif

#endif /* KERNEL_ARCH_CONTEXT_H */
