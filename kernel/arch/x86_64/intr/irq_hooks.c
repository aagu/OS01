/* kernel/arch/x86_64/irq_hooks.c — strong overrides for the
 * arch-neutral IRQ dispatch hooks (kernel/include/arch/irq.h).
 *
 * Provides the x86_64-specific implementations of:
 *   • arch_irq_select_controller  → IOAPIC / PIC fallback
 *   • arch_irq_gsi_to_vector      → gsi + 0x20 (IDT base)
 *   • arch_irq_vector_to_gsi      → vector - 0x20
 *   • arch_irq_dispatch           → moved from kernel/intr/pic/8259A.c
 *
 * The linker's strong-vs-weak resolution makes these override the
 * weak defaults in kernel/intr/arch_irq_hooks.c.
 */

#include <arch/irq.h>
#include <arch/x86_64/intr_dispatch.h>
#include <arch/x86_64/regs.h>     // IA32_EFER, etc. (unused here but kept
                                          //   for consistency with the rest of the
                                          //   x86_64 arch layer)
#include <intr/apic.h>                  // apic_available, ioapic_available, get_ioapic_controller
#include <intr/pic.h>                   // get_pic_controller
#include <intr/interrupt.h>             // hw_int_controller_t
#include <arch/x86_64/irq_route.h>       // x86_irq_route_*
#include <core/debug.h>                 // debug_irq
#include <log/log.h>                    // log_err (for spurious-vector log)
#include <arch/cpu.h>              // arch_local_irq_disable
#include <stddef.h>                       // NULL

// IDs ISA IRQs 0-15. GSI 16+ come from PIRQ routing on Q35/ICH9.
// These are the historical ISA IRQ numbers; modern firmware maps
// everything through IOAPIC regardless.
#define GSI_ISA_MAX  16u

// Vectors 0x80+ are reserved / non-IOAPIC on this OS (the kernel
// uses 0x20-0x7F for IRQ vectors). Anything above is spurious.
#define IRQ_VECTOR_HIGH_BIT  0x80u

hw_int_controller_t *arch_irq_select_controller(uint32_t gsi)
{
    if (ioapic_available()) {
        return get_ioapic_controller();
    }
    /* No IOAPIC: legacy PIC handles ISA IRQs 0-15 only. PCI GSIs
     * (PIRQ routing → 16+) need IOAPIC. */
    if (gsi < GSI_ISA_MAX) {
        return get_pic_controller();
    }
    return (hw_int_controller_t *)0;
}

int arch_irq_route_prepare(uint32_t gsi, uint64_t *vector)
{
    return x86_irq_route_prepare(gsi, vector);
}

uint64_t arch_irq_gsi_to_vector(uint32_t gsi)
{
    const struct x86_irq_route *r = x86_irq_route_find_by_gsi(gsi);
    if (r && r->allocated)
        return r->vector;
    if (!ioapic_available() && gsi < 24) {
        return (uint64_t)(0x20 + gsi);
    }
    return UINT64_MAX;
}

uint32_t arch_irq_vector_to_gsi(uint64_t vector)
{
    uint32_t gsi = UINT32_MAX;
    if (x86_irq_route_lookup(vector, &gsi))
        return gsi;
    if (!ioapic_available() && vector >= 0x20 && vector <= 0x37) {
        return (uint32_t)(vector - 0x20);
    }
    return UINT32_MAX;
}

uint32_t arch_irq_isa_to_gsi(uint8_t source)
{
    if (ioapic_available()) {
        return isa_irq_to_gsi(source);
    }
    return (uint32_t)source;
}

// Body lifted from the old kernel/intr/pic/8259A.c::do_IRQ. The IDT
// entry stubs (kernel/arch/x86_64/irq.c: Build_IRQ) `jmp do_IRQ`,
// which used to live in 8259A.c; now we route through this hook so
// kernel/intr/irq.c and the platform drivers (8259A, IOAPIC) are
// free of dispatch logic. The `nr & 0x80` spurious check is x86_64
// legacy: master PIC = 0x20-0x27, slave PIC = 0x28-0x2f, IOAPIC
// vectors continue from 0x30 through 0x7F; 0x80+ never fires.
void x86_handle_device_irq(pt_regs_t *regs, uint64_t hwirq)
{
    arch_local_irq_disable();
    if ((hwirq & IRQ_VECTOR_HIGH_BIT) != 0) {
        log_err("do_IRQ: spurious vector %#018lx\n", hwirq);
        return;
    }
    uint32_t gsi = arch_irq_vector_to_gsi(hwirq);
    if (gsi == UINT32_MAX) {
        log_err("do_IRQ: unmapped vector %#lx\n", hwirq);
        return;
    }
    irq_desc_t *irq = irq_desc_for_gsi(gsi);
    if (irq == NULL) {
        debug_irq("do_IRQ: hwirq %#lx → gsi %u has no descriptor\n",
                  hwirq, (unsigned)gsi);
        if (ioapic_available()) {
            lapic_eoi();
        }
        return;
    }
    if (irq->handler != NULL) {
        irq->handler(hwirq, irq->parameter, regs);
    }
    if (irq->controller != NULL) {
        irq->controller->ack(hwirq);
    } else if (ioapic_available()) {
        lapic_eoi();
    }
}
