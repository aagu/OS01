#include <intr/pic.h>
#include <core/printk.h>
#include <arch/io.h>
#include <kernel.h>
#include <arch/irq.h>
#include <arch/thread.h>
#include <intr/interrupt.h>
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

// NOTE: The hardware-IRQ dispatch path (formerly `do_IRQ`) lives in
// kernel/arch/x86_64/irq_hooks.c as `arch_irq_dispatch`. The IDT
// assembly stubs in kernel/arch/x86_64/irq.c `jmp arch_irq_dispatch`
// directly. The PIC controller implementation stays here because it
// is a x86_64 platform driver, not arch-neutral plumbing.

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

#ifdef __x86_64__
#include <subsys/subsys.h>
// Register this driver into the platform's subsystem table. The
// _register function is collected by arch_register_subsys() at boot via
// the .subsys_init linker section; it calls register_subsys() to queue
// the init wrapper for subsys_init_phase() to run later. The split
// (register vs init) ensures each driver's init runs ONCE — at the
// right phase — and never during the .subsys_init pass (which would
// race init order with sibling drivers).
static int _pic_init_wrapper(void)
{
    pic_init();
    return 0;
}
static int _pic_register(void)
{
    register_subsys("pic", _pic_init_wrapper,
                    SUBSYS_PHASE_3, SUBSYS_FLAG_OPTIONAL);
    return 0;
}
SUBSYS_INITCALL(_pic_register);
#endif
