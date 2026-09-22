// kernel/compiler_rt/stack_chk_guard.c — kernel-side __stack_chk_guard
// single source (AAGU-4.4).
//
// Why this lives in compiler_rt/: the spec at
// docs/arch/cross-boundary-symbols.md §2.1 mandates "唯一定义点选择:
// kernel/compiler_rt/<name>.c 或 libc/<libname>/<name>.c，二选一".
// The kernel-side guard/fail pair is the kernel's choice; the user-space
// pair lives in libc/ssp/ssp.c (gated by __is_libk so libk.a does NOT
// bring a duplicate definition into the kernel link).
//
// __stack_chk_fail mirrors the libc/ssp/ssp.c print+abort contract on
// every supported arch: disable interrupts, write a one-line diagnostic,
// halt the CPU.  The diagnostic goes through the arch_early_puts facade
// (kernel/include/arch/early_print.h), whose per-arch strong overrides
// live in kernel/arch/<arch>/early_print.c:
//
//   x86_64:    COM1 port I/O (inb 0x3FD / outb 0x3F8), lock-free.
//   aarch64:   PL011 MMIO via pl011_putc() / kputs(), lock-free.
//
// Both overrides are lock-free and NORETURN-safe so the abort path
// works even when the stack around it is corrupt.  No `#ifdef __arch__`
// lives in this TU — the facade is the only arch-aware surface.
//
// Type note: the guard is declared `unsigned long` (LP64-sized on both
// x86_64 and aarch64) so the type matches libc/ssp/ssp.c and
// libc/include/sys/ssp.h's `extern unsigned long __stack_chk_guard;`.
// Using `uint64_t` would be ABI-identical on x86_64/aarch64 LP64 but
// would create a header-vs-source type mismatch with the libc
// declaration (uint64_t is `unsigned long long` in OS01's libc; see
// kernel/Makefile stdint.h injection comment).

#include <stdint.h>
#include <arch/cpu.h>           /* arch_cpu_halt()                       */
#include <arch/irq.h>           /* arch_local_irq_disable()              */
#include <arch/early_print.h>   /* arch_early_puts()                     */

/* Bootloader seed — non-zero for defense-in-depth (an all-zero guard
 * would let an attacker skip the canary check on a brand-new boot
 * context).  kernel_main() replaces it with arch_cycle_counter() as
 * its first statement (see kernel/core/main.c). */
unsigned long __stack_chk_guard = 0xDEADBEEFCAFEBABEUL;

__attribute__((noreturn, no_stack_protector, cold))
void __stack_chk_fail(void)
{
    arch_local_irq_disable();
    arch_early_puts("\n*** Kernel stack smashing detected ***\n");
    while (1) arch_cpu_halt();
    __builtin_unreachable();
}
