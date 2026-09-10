// kernel/sched/arch_kernel_thread_entry.c
//
// arch-neutral default for arch_kernel_thread_entry.
//
// On x86_64, kernel/arch/x86_64/thread_entry.S provides a strong
// override that pops the saved pt_regs frame and calls fn(arg).
// On other arches, this weak default catches the symbol so the
// linker resolves; the trampoline will be replaced once kthread
// bringup lands on that arch.

#include <kernel/printk.h>

__attribute__((weak)) void arch_kernel_thread_entry(void);

__attribute__((weak)) void arch_kernel_thread_entry(void)
{
    // Default: panic — never reached on x86_64 (overridden).
    serial_printk("PANIC: arch_kernel_thread_entry: no per-arch override\n");
    for (;;) { /* spin */ }
}
