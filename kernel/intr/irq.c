#include <intr/interrupt.h>
#include <arch/irq.h>
#include <core/debug.h>
#include <stddef.h>
#include <intr/softirq.h>
#include <string.h>

// kernel/intr/irq.c — arch-neutral IRQ registration API.
//
// All public functions (register_irq, unregister_irq, irq_mask,
// irq_unmask) take a GSI number. The GSI→vector/hwirq translation
// and the controller-selection logic live in arch-specific hooks
// declared in kernel/include/arch/irq.h. This file knows
// nothing about APIC / PIC / GIC.

int32_t register_irq(uint32_t gsi, void * arg,
        void (*handler)(uint64_t nr, uint64_t parameter, pt_regs_t * regs),
        uint64_t parameter, uint32_t flags, const char * irq_name)
{
    if (gsi >= MAX_GSI) {
        debug_irq("IRQ: GSI %u out of range (max %u)\n", gsi, MAX_GSI);
        return 0;
    }

    hw_int_controller_t *controller = arch_irq_select_controller(gsi);
    if (controller == NULL) {
        debug_irq("IRQ: no controller available for GSI %u\n", gsi);
        return 0;
    }

    uint64_t vector = arch_irq_gsi_to_vector(gsi);

    irq_desc_t *p = &irq_table[gsi];

    p->controller = controller;
    strcpy(p->irq_name, irq_name);
    p->parameter = parameter;
    p->flags = flags;
    p->handler = handler;

    if (p->controller != NULL) {
        p->controller->install(vector, arg);
        p->controller->enable(vector);
    }

    return 1;
}

// Signature now takes a GSI (consistent with register_irq). The
// previous code took a "vector" — which on x86_64 was 0x20 + gsi —
// and the off-by-swap footgun is documented in
// docs/superpowers/plans/2026-08-17-timer-clocksource-clockevent.md.
uint32_t unregister_irq(uint32_t gsi)
{
    if (gsi >= MAX_GSI) {
        debug_irq("unregister_irq: GSI %u out of range (max %u)\n", gsi, MAX_GSI);
        return 0;
    }
    irq_desc_t *p = &irq_table[gsi];

    uint64_t vector = arch_irq_gsi_to_vector(gsi);
    if (p->controller != NULL) {
        p->controller->disable(vector);
        p->controller->uninstall(vector);
    }
    p->controller = NULL;
    p->irq_name[0] = '\0';
    p->parameter = 0;
    p->flags = 0;
    p->handler = NULL;

    return 1;
}

void irq_install()
{
    arch_irq_install();
    softirq_init();
}

void irq_mask(uint32_t gsi)
{
    if (gsi >= MAX_GSI) return;
    irq_desc_t *p = &irq_table[gsi];
    if (p->controller)
        p->controller->disable(arch_irq_gsi_to_vector(gsi));
}

void irq_unmask(uint32_t gsi)
{
    if (gsi >= MAX_GSI) return;
    irq_desc_t *p = &irq_table[gsi];
    if (p->controller)
        p->controller->enable(arch_irq_gsi_to_vector(gsi));
}
