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
// __stack_chk_fail mirrors the libc/ssp/ssp.c print+abort contract:
// disable interrupts, print a one-line diagnostic, then halt the CPU.
// The diagnostic output deliberately bypasses the full driver/serial.h
// stack (which transitively pulls in <tty/tty.h> + <termios.h>) so the
// file builds on the freestanding aarch64 profile, and so the abort
// path stays lock-free even when the stack around it is corrupt.
// x86_64 emits the diagnostic to COM1 (0x3F8); aarch64 halts without
// printing because the aarch64 whitelist does not yet pull in any TU
// that uses -fstack-protector (aarch64 kernel builds without canary
// instrumentation in phase 1, see mk/profiles/aarch64-clang.mk).
//
// Type note: the guard is declared `unsigned long` (LP64-sized on both
// x86_64 and aarch64) so the type matches libc/ssp/ssp.c and
// libc/include/sys/ssp.h's `extern unsigned long __stack_chk_guard;`.
// Using `uint64_t` would be ABI-identical on x86_64/aarch64 LP64 but
// would create a header-vs-source type mismatch with the libc
// declaration (uint64_t is `unsigned long long` in OS01's libc; see
// kernel/Makefile stdint.h injection comment).

#include <stdint.h>
#include <arch/cpu.h>       /* arch_cpu_halt(), arch_cpu_pause()       */
#include <arch/irq.h>       /* arch_local_irq_disable()                 */

#ifdef __x86_64__
#include <arch/x86_64/hw.h>  /* inb(), outb() — port I/O                */
#endif

/* Bootloader seed — non-zero for defense-in-depth (an all-zero guard
 * would let an attacker skip the canary check on a brand-new boot
 * context).  kernel_main() replaces it with arch_cycle_counter() as
 * its first statement (see kernel/core/main.c). */
unsigned long __stack_chk_guard = 0xDEADBEEFCAFEBABEUL;

/* Lock-free single-byte serial output.  Deliberately does NOT call
 * write_serial() (which acquires serial_lock): the canary fail path
 * is allowed to fire from a stack-smashing context where locks may
 * be in inconsistent states, and the diagnostic has to ship whatever
 * the UART can take right now. */
static void stack_chk_putc(char c)
{
#ifdef __x86_64__
    /* Wait for COM1 transmitter holding register empty (LSR bit 5). */
    while ((inb(0x3FD) & 0x20) == 0)
        arch_cpu_pause();
    outb((uint16_t)0x3F8, (uint8_t)c);
#else
    (void)c;  /* aarch64 phase-1 has no canary users; see file header. */
#endif
}

__attribute__((noreturn, no_stack_protector, cold))
void __stack_chk_fail(void)
{
    arch_local_irq_disable();
    {
        const char *p = "\n*** Kernel stack smashing detected ***\n";
        for (; *p; p++) stack_chk_putc(*p);
    }
    while (1) arch_cpu_halt();
    __builtin_unreachable();
}
