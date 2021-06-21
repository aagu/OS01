#ifndef _KERNEL_INTERRUPT_H
#define _KERNEL_INTERRUPT_H

#include <stdint.h>
#include <kernel/arch/x86_64/regs.h>
#include <kernel/arch/x86_64/linkage.h>

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

    char* irq_name;
    uint64_t parameter;
    void (*handler)(uint64_t nr, uint64_t parameter, pt_regs_t * regs);
    uint64_t flags;
} irq_desc_t;

#define NR_IQRS 24

irq_desc_t irq_table[NR_IQRS] = {0};

int32_t register_irq(uint64_t nr, void * arg,
        void (*handler)(uint64_t nr, uint64_t parameter, pt_regs_t * regs),
        uint64_t parameter,
        hw_int_controller_t * controller,
        const char * irq_name);

uint32_t unregister_irq(uint64_t nr);

extern void (* interrupt[24])(void);

extern void do_IQR(pt_regs_t * regs, uint64_t nr);

void irq_install();

#endif