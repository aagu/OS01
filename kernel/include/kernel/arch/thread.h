#ifndef _ARCH_THREAD_H
#define _ARCH_THREAD_H

#ifdef __x86_64__
#include <kernel/arch/x86_64/regs.h>   // provides pt_regs_t

// Architecture-specific task init (TSS, CR3 setup).
// Called once by task_init() during boot.
void arch_task_init_platform(void);
#elif defined(__aarch64__)
#error "aarch64 thread.h not yet implemented"
#else
#error "Unsupported architecture"
#endif

#endif
