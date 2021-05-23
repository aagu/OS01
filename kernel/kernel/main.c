#include <kernel/bootinfo.h>
#include <string.h>
#include <kernel/printk.h>

int kernel_main(struct KERNEL_BOOT_PARAMETER_INFORMATION *bootinfo)
{
    Pos.FB_addr = (unsigned int *)bootinfo->Graphics_Info.FrameBufferBase;
    Pos.FB_length = bootinfo->Graphics_Info.FrameBufferSize;
    Pos.XResolution = bootinfo->Graphics_Info.HorizontalResolution;
    Pos.YResolution = bootinfo->Graphics_Info.VerticalResolution;
    
    color_printk(BLACK, RED, "Hello, World!\n");
    return 0;
}