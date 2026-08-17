#ifndef _KERNEL_INTERRUPT_H
#define _KERNEL_INTERRUPT_H

#include <stdint.h>
#include <kernel/arch/irq.h>

// ── IRQ trigger mode flags (passed to register_irq) ──────────
#define IRQF_TRIGGER_NONE    0x00   // Let system decide
#define IRQF_TRIGGER_EDGE    0x01   // Edge-triggered
#define IRQF_TRIGGER_LEVEL   0x02   // Level-triggered
#define IRQF_SHARED          0x80   // Shared IRQ line (future)

// ── Max GSI we handle ────────────────────────────────────────
#define MAX_GSI              24

typedef struct hw_int_type
{
    void (*enable)(uint64_t irq);
    void (*disable)(uint64_t irq);

    uint64_t (*install)(uint64_t irq, void* arg);
    void (*uninstall)(uint64_t irq);

    void (*ack)(uint64_t irq);
} hw_int_controller_t;

typedef struct irq_desc
{
    hw_int_controller_t* controller;

    char irq_name[32];
    uint64_t parameter;
    void (*handler)(uint64_t nr, uint64_t parameter, pt_regs_t * regs);
    uint64_t flags;          // IRQF_* trigger mode hints
} irq_desc_t;

#define NR_IQRS MAX_GSI

irq_desc_t irq_table[NR_IQRS] = {0};

// ── Register an IRQ handler for a given GSI ──────────────────
// gsi:       Global System Interrupt number (0..MAX_GSI-1)
//            ISA IRQs 0-15 → GSI 0-15 (unless ISO overrides)
//            PCI INTx     → GSI 16+ (PIRQ routing)
// handler:   Called from do_IRQ with the interrupt vector as nr
// flags:     IRQF_TRIGGER_* hint (system may override via MADT)
//
// The controller is auto-selected: IOAPIC if available, else
// PIC (ISA-only, GSIs 0-15).  PCI GSIs (16+) require IOAPIC.
int32_t register_irq(uint32_t gsi, void * arg,
        void (*handler)(uint64_t nr, uint64_t parameter, pt_regs_t * regs),
        uint64_t parameter, uint32_t flags, const char * irq_name);

uint32_t unregister_irq(uint64_t nr);

// 只掩蔽/解掩蔽，不触碰 irq_desc 的 handler（作 fallback 保留用）。
// 参数是 gsi；内部转 vector(0x20+gsi) 再调 controller->disable/enable。
void irq_mask(uint32_t gsi);
void irq_unmask(uint32_t gsi);

void irq_install();

#endif
