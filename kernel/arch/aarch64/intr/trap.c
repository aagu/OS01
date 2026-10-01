/* aarch64 phase 1: EL1 IRQ dispatch (Task 2.2) + EL1 sync diagnostics
 * (spec 2026-09-30-aarch64-el1-sync-diagnostics-design.md, Task 1).
 *
 * entry.S's `el1_irq_entry` saves the full 31 GPR + sp_el0 + elr + spsr
 * frame into a `struct pt_regs` and calls `el1_irq(regs)` (this function).
 * We delegate to the GIC driver's generic dispatch, which looks up the
 * INTID in the registered handler table and invokes the matching
 * handler with `regs` for inspection.
 *
 * entry.S's `el1_sync_entry` (Task 1, offset 0x200) snapshots ESR_EL1 and
 * FAR_EL1 before calling `aarch64_el1_sync_fatal(regs, esr, far)`. That
 * function is noreturn: it disables IRQs, claims the BSS `first_fault`
 * flag via arch_atomic_cas, prints one fixed-width lowercase diagnostic
 * line if it is the first reporter, and parks the CPU in `wfi`.
 *
 * Sync exceptions / data aborts land in the same vector table (slots 5/8).
 * Slot 5 is wired to el1_sync_entry; the rest remain `b .` placeholders.
 *
 * `arch_install_exception_vectors()` stays no-op: VBAR_EL1 is installed
 * by main.c (msr vbar_el1, ...) immediately after pl011_init, which is
 * why this file does not touch the system register.
 */

#include <stdint.h>
#include <stdbool.h>
#include <arch/regs.h>
#include <arch/aarch64/sync_fault.h>
#include <arch/aarch64/gic.h>
#include <arch/atomic.h>
#include <arch/cpu.h>
#include <arch/early_print.h>
#include <arch/irq.h>

/* entry.S el1_irq_entry 的 C 落点：全量保存的 pt_regs + driver dispatch。 */
void el1_irq(struct pt_regs *regs)
{
    gic_dev_dispatch(gic_dev_current(), regs);
}

void arch_install_exception_vectors(void)
{
    /* no-op; VBAR set in main.c via entry.S's exception_vectors */
}

/* ──────────────────────────────────────────────────────────────
 *  EL1h sync diagnostics — Task 1 (spec §3, §4)
 *
 *  The diagnostic must remain usable from the earliest point after
 *  high-half VBAR is installed: no malloc, no spinlocks, no scheduler,
 *  no GIC, no ordinary logger.  Only the lock-free PL011-backed
 *  arch_early_put{cs} facade and arch_atomic_cas may be touched.
 *
 *  IRQs are masked twice on purpose: `arch_local_irq_disable` covers
 *  the IRQ bit via the documented daifset #2 immediate, then a full
 *  `DAIFSet #0xf` brings D/A/F up alongside IRQ.  Spec §4 requires
 *  the wider mask so a nested SError or FIQ cannot reorder the
 *  diagnostic against the caller.
 *
 *  First-fault claim: BSS `first_fault` starts at 0 (zero-init).  The
 *  first reporter wins via arch_atomic_cas and prints the spec line.
 *  Losers halt silently to avoid racing the UART.  SMP-safe by virtue
 *  of LDAXR/STLXR (acquire-release) inside arch_atomic_cas.
 * ────────────────────────────────────────────────────────────── */

/* BSS (zero-initialised).  Volatile because the writer and reader live
 * in the same TU but the diagnostic runs with IRQs off and the CAS
 * must not be elided. */
volatile uint64_t first_fault;

/* Local output helpers — no_stack_protector so the canary prologue is
 * not emitted (this path is noreturn-safe and stack canaries would
 * require a guard reload we deliberately cannot depend on). */
static __attribute__((no_stack_protector))
void put_hex_nibble(uint64_t v)
{
    unsigned n = (unsigned)(v & 0xfu);
    arch_early_putc((char)(n < 10 ? '0' + n : 'a' + (n - 10)));
}

static __attribute__((no_stack_protector))
void put_hex_u64(uint64_t v, unsigned digits)
{
    for (unsigned i = digits; i > 0; --i) {
        unsigned shift = (i - 1) * 4;
        put_hex_nibble(v >> shift);
    }
}

static __attribute__((no_stack_protector))
void put_hex_u8(unsigned v)
{
    put_hex_nibble(v >> 4);
    put_hex_nibble(v);
}

static __attribute__((no_stack_protector))
void put_mpidr(uint64_t v)
{
    put_hex_u64(v, 16);
}

static __attribute__((no_stack_protector))
void put_reg64(uint64_t v)
{
    put_hex_u64(v, 16);
}

__attribute__((noreturn, no_stack_protector, cold))
void aarch64_el1_sync_fatal(struct pt_regs *regs, uint64_t esr, uint64_t far)
{
    /* 1. Mask IRQ, then the wider DAIF (D/A/F/I) — spec §4. */
    arch_local_irq_disable();
    __asm__ __volatile__("msr daifset, #0xf" ::: "memory");

    /* 2. Read MPIDR_EL1 BEFORE any other ESR-dependent work.  Reading
     *    system registers is fine here (DAIF is masked) and gives a
     *    stable hardware CPU identity without depending on TPIDR_EL1. */
    uint64_t mpidr;
    __asm__ __volatile__("mrs %0, mpidr_el1" : "=r"(mpidr));

    /* 3. Compute EC and FAR-validity once, then atomic-claim.  Losers
     *    bypass the UART to avoid tearing the line. */
    unsigned ec = (unsigned)((esr >> 26) & 0x3fu);
    bool far_valid = aarch64_sync_far_valid(esr);
    int winner = arch_atomic_cas(&first_fault, 0, 1);
    if (!winner) {
        for (;;) arch_cpu_halt();
    }

    /* 4. First reporter: print one line.  Fixed-width lowercase hex,
     *    fields in spec order, CR/LF once at end.  The format is
     *    consumed by qemutests/aarch64_sync_fault.py::sync_fault_evidence
     *    (Task 3), so width/order/separators are part of the ABI. */
    arch_early_puts("[aarch64-sync] FATAL mpidr=0x");
    put_mpidr(mpidr);
    arch_early_puts(" ec=0x");
    put_hex_u8(ec);
    arch_early_puts(" esr=0x");
    put_reg64(esr);
    arch_early_puts(" elr=0x");
    put_reg64(regs->elr_el1);
    arch_early_puts(" spsr=0x");
    put_reg64(regs->spsr_el1);
    arch_early_puts(" far=");
    if (far_valid) {
        arch_early_puts("0x");
        put_reg64(far);
    } else {
        arch_early_puts("n/a");
    }
    arch_early_putc('\n');

    /* 5. Park.  Diagnostic is delivered; never eret from this path. */
    for (;;) arch_cpu_halt();
    __builtin_unreachable();
}
