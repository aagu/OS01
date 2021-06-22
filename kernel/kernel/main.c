#include <kernel/bootinfo.h>
#include <string.h>
#include <kernel/printk.h>
#include <kernel/memory.h>
#include <kernel/pmm.h>
#include <kernel/arch/x86_64/trap.h>
#include <kernel/arch/x86_64/gate.h>
#include <kernel/arch/x86_64/asm.h>
#include <kernel/interrupt.h>
#include <device/pic.h>
#include <driver/rtc.h>
#include <stdlib.h>

extern char _text;
extern char _etext;
extern char _edata;
extern char _erodata;
extern char _end;

int kernel_main(struct BOOT_INFO *bootinfo)
{
    // int32_t i;
    Pos.FB_addr = (uint32_t *)bootinfo->Graphics_Info.FrameBufferBase;
    Pos.FB_length = bootinfo->Graphics_Info.FrameBufferSize;
    Pos.XResolution = bootinfo->Graphics_Info.HorizontalResolution;
    Pos.YResolution = bootinfo->Graphics_Info.VerticalResolution;

    load_TR(8);
    set_tss64(0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00);
    sys_vector_install();
    irq_install();
    
    color_printk(RED, BLACK, "Hello, World!\n");

    PMMngr.start_code = (uint64_t)&_text;
    PMMngr.end_code = (uint64_t)&_etext;
    PMMngr.end_data = (uint64_t)&_edata;
    PMMngr.end_rodata = (uint64_t)&_erodata;
    PMMngr.start_brk = (uint64_t)&_end;

    pmm_init(bootinfo->E820_Info);

    vmm_init();

    frame_buffer_init();
    color_printk(GREEN, BLACK, "frame buffer remap succeed\n");

    pic_init();

    datetime_t * dt = (datetime_t *)malloc(sizeof(datetime_t));
    rtc_read_datetime(dt);
    color_printk(GREEN, BLACK, "rtc time: %d-%d-%d %02d:%02d:%02d\n", dt->year, dt->month, dt->day, dt->hour, dt->minute, dt->second);
    free(dt);

    dt = (datetime_t *)malloc(sizeof(datetime_t));
    rtc_read_datetime(dt);
    color_printk(GREEN, BLACK, "rtc time: %d-%d-%d %02d:%02d:%02d\n", dt->year, dt->month, dt->day, dt->hour, dt->minute, dt->second);
    free(dt);

    while(1)
    {
        hlt();
    }
    return 0;
}