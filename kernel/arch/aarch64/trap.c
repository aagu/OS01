/* aarch64 phase 1: trap helpers (Task 3).
 *
 * Phase 1 doesn't have a meaningful C-side trap handler — the
 * EL1h IRQ vector in entry.S dispatches directly to el1_irq_dispatch
 * (defined in time.c), which handles the GIC IAR read + CNTP re-arm
 * + EOI.  We keep this file as the future home for ESR_EL1/FAR_EL1
 * decoding (sync exceptions / data aborts) once phase 2 needs them.
 *
 * head.S installs VBAR_EL1 directly using the LMA/VMA address of
 * entry.S's table; no helper needed for that here.
 */

#include <stdint.h>
#include <kernel/arch/cpu.h>

/* No-op for now.  Phase 2 may install a vector-table helper. */
void arch_install_exception_vectors(void)
{
    /* no-op; VBAR set in head.S via entry.S's exception_vectors */
}
