#include <intr/pic.h>
#include <log/log.h>
#include <arch/io.h>
#include <kernel.h>
#include <arch/irq.h>
#include <arch/thread.h>
#include <intr/interrupt.h>
#include <stddef.h>

void pic_init()
{
    log_info("8259A init\n");

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

    /* BOOT-2: controller init only configures hardware and preserves
     * the entry IF — the first sti belongs to x86_boot_irq_runtime_open
     * after the PIT is the confirmed periodic source. */
}

// NOTE: The hardware-IRQ dispatch path (formerly `do_IRQ`) lives in
// kernel/arch/x86_64/irq_hooks.c as `arch_irq_dispatch`. The IDT
// assembly stubs in kernel/arch/x86_64/irq.c `jmp arch_irq_dispatch`
// directly. The PIC controller implementation stays here because it
// is a x86_64 platform driver, not arch-neutral plumbing.

/* BOOT-2: the slave PIC sits behind master IRQ2, so a slave input is
 * only deliverable when BOTH the slave bit and the master cascade bit
 * are unmasked.  Enable opens the cascade alongside the slave bit;
 * disable masks the slave bit and closes IRQ2 only when the LAST slave
 * input goes masked (other slave users must keep the cascade).  The
 * IMR read-modify-write runs with IRQs off and restores the caller's
 * IF — this is boot-time BSP usage, not an SMP PIC redesign. */

void pic_enable(uint64_t nr)
{
    uint16_t port;
    uint8_t bit;
    arch_irq_state_t flags = arch_local_irq_save();
    if (nr >= 0x28)
    {
        nr -= 8;
        port = SLAVE_OCW1;
        bit = (uint8_t)(1 << (nr - 0x20));
        arch_outb(port, arch_inb(port) & (uint8_t)~bit);
        /* Open the master cascade for this slave input. */
        arch_outb(MASTER_OCW1, arch_inb(MASTER_OCW1)
                               & (uint8_t)~(1 << 2));
    }
    else
    {
        port = MASTER_OCW1;
        bit = (uint8_t)(1 << (nr - 0x20));
        arch_outb(port, arch_inb(port) & (uint8_t)~bit);
    }
    arch_local_irq_restore(flags);
}

void pic_disable(uint64_t nr)
{
    uint16_t port;
    uint8_t bit;
    arch_irq_state_t flags = arch_local_irq_save();
    if (nr >= 0x28)
    {
        nr -= 8;
        port = SLAVE_OCW1;
        bit = (uint8_t)(1 << (nr - 0x20));
        arch_outb(port, arch_inb(port) | bit);
        /* Close the master cascade only when NO slave input is left
         * unmasked. */
        if (arch_inb(SLAVE_OCW1) == 0xff)
            arch_outb(MASTER_OCW1, arch_inb(MASTER_OCW1) | (1 << 2));
    }
    else
    {
        port = MASTER_OCW1;
        bit = (uint8_t)(1 << (nr - 0x20));
        arch_outb(port, arch_inb(port) | bit);
    }
    arch_local_irq_restore(flags);
}

uint64_t pic_install(uint64_t nr, void * data __attribute__((unused)))
{
    log_info("pic device %d installed\n", nr - 0x20);
    return 0;
}

void pic_uninstall(uint64_t nr)
{
    log_info("pic device %d uninstalled\n", nr - 0x20);
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

#include <intr/apic.h>
#include <subsys/subsys.h>
// Register this driver into the platform's subsystem table. The
// _register function is collected by arch_register_subsys() at boot via
// the .subsys_init linker section; it calls register_subsys() to queue
// the init wrapper for subsys_init_phase() to run later. The split
// (register vs init) ensures each driver's init runs ONCE — at the
// right phase — and never during the .subsys_init pass (which would
// race init order with sibling drivers).
// This file lives under kernel/arch/x86_64/intr/ so it is x86_64-only by
// directory placement; the previous #ifdef __x86_64__ guard is now dead
// and was removed when the file was relocated (AAGU-4.5).
static int _pic_init_wrapper(void)
{
    if (ioapic_available()) {
        log_info("8259A: skipped (IOAPIC line controller active)\n");
        arch_outb(MASTER_OCW1, 0xff);
        arch_outb(SLAVE_OCW1, 0xff);
        /* BOOT-2: preserve entry IF (see pic_init). */
        return 0;
    }
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
