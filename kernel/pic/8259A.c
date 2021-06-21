#include <device/pic.h>
#include <kernel/printk.h>
#include <hw.h>
#include <kernel/arch/x86_64/asm.h>
#include <kernel/arch/x86_64/regs.h>
#include <kernel/interrupt.h>
#include <stddef.h>

void pic_init()
{
    color_printk(YELLOW, BLACK, "8259A init\n");

    //8259A Master
    outb(0x20, 0x11);
    outb(0x21, 0x20);
    outb(0x21, 0x04);
    outb(0x21, 0x01);

    //8259A Slave
    outb(0xa0, 0x11);
    outb(0xa1, 0x28);
    outb(0xa1, 0x02);
    outb(0xa1, 0x01);

    outb(0x21, 0x00);
    outb(0xa1, 0x00);

    sti();
}

void do_IRQ(pt_regs_t * regs, uint64_t nr)	//regs,nr
{
    cli();
	outb(0x20,0x20);
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
    sti();
}