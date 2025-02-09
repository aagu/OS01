#include <stddef.h>
#include <driver/keyboard.h>
#include <device/pic.h>
#include <kernel/interrupt.h>
#include <kernel/printk.h>
#include <kernel/arch/x86_64/hw.h>

hw_int_controller_t keyboard_controller = 
{
    .enable = pic_enable,
    .disable = pic_disable,
    .install = pic_install,
    .uninstall = pic_uninstall,
    .ack = pic_ack,
};

void keyboard_handler(uint64_t nr, uint64_t parameter __attribute__((unused)), pt_regs_t * regs __attribute__((unused)))
{
    unsigned char x = inb(0x60);
    outb(0x20, 0x20);
    color_printk(GREEN, BLACK, "keyboard pressed, code: %#08x\n", x);
}

void keyboard_init()
{
    register_irq(0x21, NULL, &keyboard_handler, 0, &keyboard_controller, "keyboard");
}