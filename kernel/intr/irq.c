#include <kernel/interrupt.h>
#include <kernel/arch/irq.h>
#include <kernel/debug.h>
#include <stddef.h>
#include <kernel/softirq.h>
#include <string.h>
#include <kernel/apic.h>

int32_t register_irq(uint32_t gsi, void * arg,
        void (*handler)(uint64_t nr, uint64_t parameter, pt_regs_t * regs),
        uint64_t parameter, uint32_t flags, const char * irq_name)
{
    if (gsi >= MAX_GSI) {
        debug_irq("IRQ: GSI %u out of range (max %u)\n", gsi, MAX_GSI);
        return 0;
    }

    // ── Auto-select controller ───────────────────────────────
    hw_int_controller_t *controller = NULL;
    if (apic_available()) {
        controller = get_ioapic_controller();
    } else if (gsi < 16) {
        // PIC only handles ISA IRQs 0-15; PCI GSIs (16+) need IOAPIC
        extern hw_int_controller_t *get_pic_controller(void);
        controller = get_pic_controller();
    } else {
        debug_irq("IRQ: GSI %u needs IOAPIC but APIC not available\n", gsi);
        return 0;
    }

    // ── Vector = 0x20 + gsi ──────────────────────────────────
    uint64_t vector = 0x20 + gsi;

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

uint32_t unregister_irq(uint64_t nr)
{
    irq_desc_t *p = &irq_table[nr - 32];

    if (p->controller != NULL) {
        p->controller->disable(nr);
        p->controller->uninstall(nr);
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
