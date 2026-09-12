#ifndef _KERNEL_ARCH_REGS_FACADE_H
#define _KERNEL_ARCH_REGS_FACADE_H

// ─────────────────────────────────────────────────────────
//  arch/regs.h — arch-neutral facade for pt_regs_t
//
//  Dispatches to the per-arch register-layout header based on
//  the compiler-defined target macro. Each per-arch header
//  (kernel/include/arch/<arch>/regs.h) defines
//  `pt_regs_t` plus any arch-specific register bit / MSR
//  constants used by .c and .S code.
//
//  Arch-neutral code that needs `pt_regs_t` should include
//  THIS header — not arch/thread.h, which carries
//  scheduler-lifecycle function declarations unrelated to
//  the register layout. The previous arrangement forced any
//  caller of arch/irq.h (which included arch/thread.h for
//  pt_regs_t) to transitively pull in scheduler APIs.
//
//  IMPORTANT: the include guard MUST differ from the per-arch
//  headers' guards. If they collide, the per-arch contents get
//  skipped on the second include — leaving pt_regs_t undefined.
// ─────────────────────────────────────────────────────────

#ifdef __x86_64__
#include <arch/x86_64/regs.h>
#elif defined(__aarch64__)
#include <arch/aarch64/regs.h>
#else
#error "Unsupported architecture"
#endif

#endif /* _KERNEL_ARCH_REGS_FACADE_H */
