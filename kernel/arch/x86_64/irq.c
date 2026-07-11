// kernel/arch/x86_64/irq.c -- x86-specific IRQ gate installation
//
// Build_IRQ expansions and arch_irq_install() are still in
// intr/irq.c at this stage.  They will be moved here in Task 6
// when intr/irq.c is cleaned up.

#include <kernel/arch/x86_64/gate.h>

void arch_install_intr_gate(uint8_t vector, void *stub, uint8_t ist) {
    set_intr_gate_raw(vector, ist, stub);
}
