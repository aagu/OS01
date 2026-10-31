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

/* ── Kernel-thread constructor hooks (spec §8.1) ─────────────
 * The common constructor in kernel/sched/kthread.c owns allocation,
 * identity, metadata and publication.  The architecture owns the
 * resources that differ per architecture:
 *
 *   arch_kthread_prepare()  real arch context + resources.  Returns
 *                           0, or a negative errno.  On failure it
 *                           must clean up and NULL whatever it
 *                           acquired (files, FPU area) so the common
 *                           rollback only has to free the thread
 *                           struct and the raw stack.
 *   arch_kthread_drop_files() detach and unpin the task's file-table
 *                           reference only.  The thread struct, the
 *                           FPU area and the raw stack stay owned by
 *                           the common rollback / switch tail.
 *
 * aarch64 supplies its own implementations in Phase 3 (shared init_mm,
 * no file table, no FPU area); x86 supplies them in
 * kernel/arch/x86_64/cpu/task_arch.c. */
int arch_kthread_prepare(task_t *t, uint64_t (*fn)(uint64_t), uint64_t arg);
void arch_kthread_drop_files(task_t *t);

/* ── Architecture capability: remote reschedule notification ────
 * x86 can wake a remote CPU with an IPI; aarch64 has no remote
 * reschedule IPI in this phase (spec §7).  A runtime bool does not
 * remove the linker dependency, so this is a compile-time constant and
 * BOTH the <intr/ipi.h> include and the ipi_send() call are gated on
 * it.  1 = the architecture can notify a remote CPU; 0 = local only. */
#ifdef __x86_64__
#define ARCH_HAS_REMOTE_RESCHED 1
#else
#define ARCH_HAS_REMOTE_RESCHED 0
#endif
#endif

#endif /* KERNEL_ARCH_CONTEXT_H */
