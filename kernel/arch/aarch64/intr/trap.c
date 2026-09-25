/* aarch64 phase 1: EL1 IRQ dispatch (Task 2.2).
 *
 * entry.S's `el1_irq_entry` saves the full 31 GPR + sp_el0 + elr + spsr
 * frame into a `struct pt_regs` and calls `el1_irq(regs)` (this function).
 * We delegate to the GIC driver's generic dispatch, which looks up the
 * INTID in the registered handler table and invokes the matching
 * handler with `regs` for inspection.
 *
 * Sync exceptions / data aborts land in the same vector table (slots 5/8)
 * and remain `b .` placeholders; phase 2 may extend this file with
 * ESR_EL1/FAR_EL1 decoding.
 *
 * `arch_install_exception_vectors()` stays no-op: VBAR_EL1 is installed
 * by main.c (msr vbar_el1, ...) immediately after pl011_init, which is
 * why this file does not touch the system register.
 */

#include <stdint.h>
#include <arch/regs.h>
#include <arch/aarch64/gic.h>

/* entry.S el1_irq_entry 的 C 落点：全量保存的 pt_regs + driver dispatch。 */
void el1_irq(struct pt_regs *regs)
{
    gic_dev_dispatch(gic_dev_current(), regs);
}

void arch_install_exception_vectors(void)
{
    /* no-op; VBAR set in main.c via entry.S's exception_vectors */
}
