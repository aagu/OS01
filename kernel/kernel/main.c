#include <kernel/bootinfo.h>
#include <string.h>
#include <kernel/printk.h>
#include <kernel/memory.h>
#include <kernel/pmm.h>
#include <kernel/arch/x86_64/trap.h>
#include <kernel/arch/x86_64/gate.h>
#include <kernel/arch/x86_64/asm.h>
#include <kernel/arch/x86_64/spinlock.h>
#include <kernel/interrupt.h>
#include <device/pic.h>
#include <driver/pit.h>
#include <driver/serial.h>
#include <driver/keyboard.h>
#include <device/timer.h>
#include <stdlib.h>

extern char _text;
extern char _etext;
extern char _edata;
extern char _erodata;
extern char _end;

timer_t * timer;

void test_timer(void * data __attribute__((unused)))
{
    color_printk(GREEN, BLACK, "test_timer\n");
    free(timer);
}

int kernel_main(struct BOOT_INFO *bootinfo)
{
    Pos.Phy_addr = (uint32_t *)bootinfo->Graphics_Info.FrameBufferBase;
    Pos.FB_length = bootinfo->Graphics_Info.FrameBufferSize;
    Pos.XResolution = bootinfo->Graphics_Info.HorizontalResolution;
    Pos.YResolution = bootinfo->Graphics_Info.VerticalResolution;

    spin_init(&Pos.lock);

    load_TR(8);
    set_tss64(0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00);
    sys_vector_install();
    irq_install();
    init_serial();
    serial_printk("serial port init succedd\n");
    serial_printk("PMMgr: 0x%p\n", &PMMngr);
    unsigned long cr3 = (unsigned long)get_cr3() & (~ 0xffffUL);
    serial_printk("cr3 address: %#08x\n", cr3);

    frame_buffer_early_init();
    
    color_printk(RED, BLACK, "Hello, World!\n");

    PMMngr.start_code = (uint64_t)&_text;
    serial_printk("_text: 0x%p\n", &_text);
    PMMngr.end_code = (uint64_t)&_etext;
    serial_printk("_etext: 0x%p\n", &_etext);
    PMMngr.end_data = (uint64_t)&_edata;
    serial_printk("_edata: 0x%p\n", &_edata);
    PMMngr.end_rodata = (uint64_t)&_erodata;
    serial_printk("_erodata: 0x%p\n", &_erodata);
    PMMngr.start_brk = (uint64_t)&_end;
    serial_printk("_end: 0x%p\n", &_end);

    pmm_init(bootinfo->E820_Info);
    serial_printk("PMMgr.end_of_struct: 0x%p\n", PMMngr.end_of_struct);

    vmm_init();

    frame_buffer_init();
    color_printk(GREEN, BLACK, "frame buffer remap succeed\n");

    pic_init();
    timer_init();
    pit_init();
    keyboard_init();

    timer = create_timer(test_timer, NULL, 10);;
    add_timer(timer);

    while(1)
    {
        hlt();
    }
    return 0;
}