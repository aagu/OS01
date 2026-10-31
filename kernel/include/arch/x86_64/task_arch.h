#ifndef KERNEL_ARCH_X86_64_TASK_ARCH_H
#define KERNEL_ARCH_X86_64_TASK_ARCH_H

/* x86_64 kernel-thread constructor hooks.
 *
 * The frozen declarations live in <arch/context.h> (series index
 * "Frozen Interface Contract"); this x86-local header only documents the
 * x86 half and re-declares the two hooks with their frozen names.  The
 * common constructor in kernel/sched/kthread.c owns allocation,
 * identity, metadata and publication.  The x86 half owns the resources
 * that are architecture-specific:
 *
 *   - the inherited mm / CR3 (a kernel thread runs in the kernel
 *     address space, exactly like the task that created it),
 *   - the copied file table (files_dup),
 *   - the FPU save area,
 *   - the pt_regs first-entry frame the thread_entry.S trampoline
 *     pops (rbx = fn, rdx = arg, rip = arch_kernel_thread_entry).
 *
 * aarch64 has its own implementation in
 * kernel/arch/aarch64/cpu/task_arch.c (Phase 3).
 */

#include <sched/task.h>

/* Prepare the architecture context of a new kernel thread.
 *
 * Returns 0 on success, or a negative errno.  On failure every resource
 * this hook acquired (file table, FPU save area) has already been
 * released and NULLed, so the common rollback only has to free the
 * thread struct and the raw stack. */
int arch_kthread_prepare(task_t *t, uint64_t (*fn)(uint64_t), uint64_t arg);

/* Detach and unpin the task's file-table reference.
 *
 * The detach happens under task_list_lock (the file table is published
 * under that lock); the unpin happens outside it, because files_unpin()
 * drops the last reference and may free the table — which must not
 * happen under task_list_lock. */
void arch_kthread_drop_files(task_t *t);

#endif /* KERNEL_ARCH_X86_64_TASK_ARCH_H */
