/* kernel/intr/arch_irq_hooks.c — weak defaults for the arch-neutral
 * IRQ dispatch hooks declared in kernel/include/arch/irq.h.
 *
 * Each arch (x86_64 today, aarch64 in a future slice) provides a
 * strong override in kernel/arch/<arch>/irq_hooks.c. The weak
 * defaults below let the link succeed even if an arch forgets to
 * override them — they either return NULL (caller aborts with a
 * FATAL log) or identity-map the GSI↔vector translation.
 *
 * NOTE: the strong overrides in kernel/arch/<arch>/irq_hooks.c are
 * compiled into the per-arch .o set, but the WEAK defaults here are
 * compiled into the arch-neutral intr/ build. The linker resolves
 * each symbol to the strong definition when one exists; otherwise the
 * weak one is kept. If neither, the link fails — which is the
 * correct behaviour for a future arch that hasn't wired its IRQ
 * controller yet.
 */

#include <arch/irq.h>
#include <log/log.h>
#include <arch/cpu.h>   /* arch_cpu_halt for the FATAL path */

__attribute__((weak))
hw_int_controller_t *arch_irq_select_controller(uint32_t gsi)
{
    (void)gsi;
    log_err("[intr] FATAL: no arch_irq_select_controller override\n");
    arch_cpu_halt();
    return (hw_int_controller_t *)0;
}

__attribute__((weak))
uint64_t arch_irq_gsi_to_vector(uint32_t gsi)
{
    /* Default: identity. Works on archs where hwirq == gsi (e.g. GIC). */
    return (uint64_t)gsi;
}

__attribute__((weak))
uint32_t arch_irq_vector_to_gsi(uint64_t vector)
{
    return (uint32_t)vector;
}

__attribute__((weak))
void arch_irq_dispatch(pt_regs_t *regs, uint64_t hwirq)
{
    /* No override: just drop the interrupt. The kernel will continue
     * running; if the interrupt was essential the missing device
     * driver will eventually time out and report. */
    (void)regs;
    (void)hwirq;
}
