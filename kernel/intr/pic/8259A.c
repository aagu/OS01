#include <device/pic.h>
#include <kernel/printk.h>
#include <kernel/arch/io.h>
#include <kernel.h>
#include <kernel/arch/irq.h>
#include <kernel/arch/thread.h>
#include <kernel/interrupt.h>
#include <stddef.h>

void pic_init()
{
    color_printk(YELLOW, BLACK, "8259A init\n");

    //8259A Master
    arch_outb(MASTER_ICW1, 0x11);
    arch_outb(MASTER_ICW2, 0x20);
    arch_outb(MASTER_ICW3, 0x04);
    arch_outb(MASTER_ICW4, 0x01);

    //8259A Slave
    arch_outb(SLAVE_ICW1, 0x11);
    arch_outb(SLAVE_ICW2, 0x28);
    arch_outb(SLAVE_ICW3, 0x02);
    arch_outb(SLAVE_ICW4, 0x01);

    arch_outb(MASTER_OCW1, 0xff);
    arch_outb(SLAVE_OCW1, 0xff);

    arch_local_irq_enable();
}

void do_IRQ(pt_regs_t * regs, uint64_t nr)	//regs,nr
{
    arch_local_irq_disable();
    switch (nr & 0x80)
    {
    case 0x00:
        {
            irq_desc_t * irq = &irq_table[nr - 32];

            if (irq->handler != NULL)
                irq->handler(nr, irq->parameter, regs);

            if (irq->controller != NULL)
                irq->controller->ack(nr);
        }
        break;
    
    default:
        color_printk(RED,BLACK,"do_IRQ:%#018lx\t",nr);
        color_printk(RED,BLACK,"regs:%#018lx\t<RIP:%#018lx\tRSP:%#018lx>\n",regs,regs->rip,regs->rsp);
        break;
    }
    arch_local_irq_enable();
}

void pic_enable(uint64_t nr)
{
    uint16_t port;
    uint8_t value;
    if (nr >= 0x28)
    {
        nr -= 8;
        port = SLAVE_OCW1;
    }
    else
        port = MASTER_OCW1;

    value = arch_inb(port) & ~(1 << (nr - 0x20));
    arch_outb(port, value);
}

void pic_disable(uint64_t nr)
{
    uint16_t port;
    uint8_t value;
    if (nr >= 0x28)
    {
        nr -= 8;
        port = SLAVE_OCW1;
    }
    else
        port = MASTER_OCW1;

    value = arch_inb(port) | (1 << (nr - 0x20));
    arch_outb(port, value);
}

uint64_t pic_install(uint64_t nr, void * data __attribute__((unused)))
{
    color_printk(BLUE, BLACK, "pic device %d installed\n", nr - 0x20);
    return 0;
}

void pic_uninstall(uint64_t nr)
{
    color_printk(BLUE, BLACK, "pic device %d uninstalled\n", nr - 0x20);
}

void pic_ack(uint64_t nr)
{
    if (nr >= 0x28)
        arch_outb(SLAVE_OCW3, 0x20);
    arch_outb(MASTER_OCW3, 0x20);
}

// ── Global PIC controller instance ──────────────────────────
// Shared fallback for ISA IRQs when IOAPIC is unavailable.

static hw_int_controller_t pic_controller = {
    .enable    = pic_enable,
    .disable   = pic_disable,
    .install   = pic_install,
    .uninstall = pic_uninstall,
    .ack       = pic_ack,
};

hw_int_controller_t *get_pic_controller(void)
{
    return &pic_controller;
}
