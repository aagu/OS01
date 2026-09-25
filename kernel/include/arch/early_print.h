#ifndef _ARCH_EARLY_PRINT_H
#define _ARCH_EARLY_PRINT_H

// ── Architecture output facade ─────────────────────────────
//
// Lock-free single-character / NUL-string output, intended for fault
// paths (e.g. core/stack_chk.c::__stack_chk_fail) where the
// regular serial driver stack might deadlock on a corrupt lock.
//
// Strong overrides live in kernel/arch/<arch>/early_print.c and are
// pulled in via the per-arch KERNEL_C_SOURCES wildcard
// (kernel/Makefile's $(ARCHDIR)/*.c rule).
//
//   x86_64:  COM1 port I/O (inb 0x3FD / outb 0x3F8), bypasses spinlock.
//   aarch64: PL011 MMIO @ 0x09000000, requires pl011_init() to have run.
//
// Both implementations are NORETURN-safe: they busy-wait on the UART
// without acquiring any lock and without calling into malloc/interrupt
// paths.  Caller is responsible for having disabled IRQs beforehand if
// they want strict ordering.

// Lock-free single-character output.
void arch_early_putc(char c);

// Lock-free NUL-terminated string output.
void arch_early_puts(const char *s);

#endif /* _ARCH_EARLY_PRINT_H */
