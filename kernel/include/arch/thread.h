#ifndef _ARCH_THREAD_H
#define _ARCH_THREAD_H

// arch/regs.h provides `pt_regs_t` (and arch-specific register-layout
// constants). Always include via the facade, not the per-arch path
// directly, so that arch-neutral callers stay neutral.
#include <arch/regs.h>

// ── Thread lifecycle / signal delivery ──────────────────
//
// Two flavors, depending on whether the arch has its own task_arch.c
// or relies on static-inline stubs (aarch64 phase 1).
//
//  • Archs with task_arch.c (x86_64): extern declarations here, real
//    definitions in kernel/arch/<arch>/task_arch.c.
//  • Archs without task_arch.c (aarch64 phase 1): static-inline no-op
//    stubs. Keeps kernel/sched/task.c etc. linkable while aarch64
//    grows TSS / signal-delivery support incrementally.
//
// Adding a new arch: if it ships its own task_arch.c, add it to the
// `#else` branch below. If it's a phase-1 port, model after aarch64.

#ifdef __aarch64__

static inline void  arch_task_init_early(void)              { }
static inline void *arch_task_boot_state(void)               { return (void *)0; }
static inline void  arch_task_init_platform(void)           { }
static inline int   arch_do_signal_delivery(pt_regs_t *regs) { (void)regs; return 0; }
static inline int   arch_signal_pending_fatal(void)         { return 0; }

#else

// Early platform init (TSS, CR3, ...). Runs before interrupt vectors
// and memory/task setup. Called once by the BSP during boot.
void arch_task_init_early(void);

// Returns a pointer to the architecture's per-CPU boot-state blob
// (TSS on x86_64, NULL on archs that don't need one). Used by
// kernel_main to install / restore it during bringup.
void *arch_task_boot_state(void);

// Called once by task_init() during boot. Programs BSP TSS, etc.
void arch_task_init_platform(void);

// Signal delivery — arch-specific because it manipulates the user-mode
// register frame on the kernel stack before returning to userspace.
int  arch_do_signal_delivery(pt_regs_t *regs);
int  arch_signal_pending_fatal(void);

#endif

#endif
