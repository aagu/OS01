// kernel/arch/aarch64/early_print.c — strong override for the
// arch_early_putc / arch_early_puts facade in
// kernel/include/arch/early_print.h.
//
// aarch64 implementation: thin shim over the existing PL011 driver
// (kernel/arch/aarch64/pl011.c).  pl011_init() must have run before
// any output is emitted — aarch64 phase 1 invokes it from
// kernel/arch/aarch64/main.c well before user code can trip the
// canary, so the precondition holds for the only call site today
// (compiler_rt/stack_chk_guard.c::__stack_chk_fail).
//
// pl011_putc() is itself lock-free: it polls PL011_FR.TXFF and writes
// one byte at a time.  That matches the contract documented in
// include/arch/early_print.h — no spinlock, no malloc, safe from a
// fault handler.
//
// Compiled by kernel/Makefile's $(ARCHDIR)/*.c wildcard (no explicit
// KERNEL_C_SOURCES entry needed).

#include <stdint.h>

/* pl011.c declares these as plain extern "C" functions; we re-declare
 * them here rather than including the .c file to keep the dependency
 * one-way (early_print.c → pl011.c, not the reverse). */
void pl011_putc(char c);
void kputs(const char *s);

void arch_early_putc(char c)
{
    pl011_putc(c);
}

void arch_early_puts(const char *s)
{
    kputs(s);
}
