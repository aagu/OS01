#ifndef _ARCH_THREAD_H
#define _ARCH_THREAD_H

#ifdef __x86_64__
#include <kernel/arch/x86_64/regs.h>   // provides pt_regs_t

// Architecture-specific task init (TSS, CR3 setup).
// Called once by task_init() during boot.
void arch_task_init_platform(void);

// Signal delivery — arch-specific because it manipulates the
// user-mode register frame on the kernel stack before iretq.
int  do_signal_delivery(pt_regs_t *regs);
int  signal_pending_fatal(void);   // non-zero if a fatal signal is pending
#elif defined(__aarch64__)

// aarch64 exception frame as pushed by the exception vector handler.
// x0-x29 (30 regs), plus SP_EL0, ELR_EL1, SPSR_EL1.
typedef struct pt_regs
{
    uint64_t x0, x1, x2, x3, x4, x5, x6, x7;
    uint64_t x8, x9, x10, x11, x12, x13, x14, x15;
    uint64_t x16, x17, x18, x19, x20, x21, x22, x23;
    uint64_t x24, x25, x26, x27, x28, x29;
    uint64_t sp_el0;
    uint64_t elr_el1;
    uint64_t spsr_el1;
} pt_regs_t;

// arch_task_init_platform placeholder — aarch64 doesn't need TSS setup.
// Declared in this header's x86_64 block. For aarch64 it's a no-op.
static inline void arch_task_init_platform(void) {}

// Signal delivery stubs — aarch64 not yet implemented.
static inline int do_signal_delivery(pt_regs_t *regs) { (void)regs; return 0; }
static inline int signal_pending_fatal(void) { return 0; }
#else
#error "Unsupported architecture"
#endif

#endif
