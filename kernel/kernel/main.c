#include <kernel/bootinfo.h>
#include <string.h>
#include <kernel/printk.h>

int kernel_main(struct KERNEL_BOOT_PARAMETER_INFORMATION *bootinfo)
{
    Pos.FB_addr = (unsigned int *)bootinfo->Graphics_Info.FrameBufferBase;
    Pos.FB_length = bootinfo->Graphics_Info.FrameBufferSize;
    Pos.XResolution = bootinfo->Graphics_Info.HorizontalResolution;
    Pos.YResolution = bootinfo->Graphics_Info.VerticalResolution;
    
    color_printk(RED, BLACK, "Hello, World!\n");
    int i = 1/ 0;
    while(1)
    {
        __asm__ __volatile__("hlt");
    }
    return 0;
}