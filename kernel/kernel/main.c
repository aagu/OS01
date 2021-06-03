#include <kernel/bootinfo.h>
#include <string.h>
#include <kernel/printk.h>
#include <kernel/memory.h>
#include <kernel/arch/x86_64/trap.h>
#include <kernel/arch/x86_64/gate.h>
#include <kernel/arch/x86_64/asm.h>

int kernel_main(struct BOOT_INFO *bootinfo)
{
    int i;
    Pos.FB_addr = (unsigned int *)bootinfo->Graphics_Info.FrameBufferBase;
    Pos.FB_length = bootinfo->Graphics_Info.FrameBufferSize;
    Pos.XResolution = bootinfo->Graphics_Info.HorizontalResolution;
    Pos.YResolution = bootinfo->Graphics_Info.VerticalResolution;

    load_TR(8);
    set_tss64(0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00, 0x7c00);
    sys_vector_install();
    
    color_printk(RED, BLACK, "Hello, World!\n");
    pmm_init(bootinfo->E820_Info);
    while(1)
    {
        hlt();
    }
    return 0;
}