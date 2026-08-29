/* aarch64 phase 1: exception-vector support stubs.
 *
 * entry.S holds the actual VBAR table; this file holds the C-side
 * companions that other arch code might reference later.  Phase 1
 * has no GIC dispatch yet (Task 3), so `el1_irq_handler` is a panic
 * spinloop if it ever fires.  main.c installs the vector base via
 * head.S directly.
 */

#include <stdint.h>
#include <kernel/arch/cpu.h>

/* Default EL1 IRQ handler.  Phase 1 keeps IRQs masked the whole time
 * the kernel is doing real work (tick ISR comes in Task 3 after we
 * explicitly enable interrupts), so this should not fire.  If it
 * does, hang. */
void el1_irq_handler(uint64_t esr, uint64_t far, uint64_t elr,
                     uint64_t spsr, uint64_t sp)
{
    (void)esr; (void)far; (void)elr; (void)spsr; (void)sp;
    for (;;) {
        arch_cpu_halt();
    }
}

/* phase 1: no real installation needed — head.S writes VBAR_EL1
 * directly using the LMA/VMA address of entry.S's table. */
void arch_install_exception_vectors(void)
{
    /* no-op; VBAR set in head.S / entry.S */
}
