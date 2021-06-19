#include <kernel/bootinfo.h>
#include <string.h>
#include <kernel/printk.h>
#include <kernel/memory.h>
#include <kernel/pmm.h>
#include <kernel/arch/x86_64/trap.h>
#include <kernel/arch/x86_64/gate.h>
#include <kernel/arch/x86_64/asm.h>
#include <kernel/panic.h>

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

    kpanic("%s\n", "something went wrong");

/*
	struct Page * page = NULL;

	color_printk(RED,BLACK,"PMMngr.bits_map:%#018lx\n",*PMMngr.bits_map);
	color_printk(RED,BLACK,"PMMngr.bits_map:%#018lx\n",*(PMMngr.bits_map + 1));
	page = alloc_pages(ZONE_UNMAPPED,64,PG_PTable_Mapped | PG_Kernel);
	for(i = 0;i <= 64;i++)
	{
		color_printk(INDIGO,BLACK,"page%d\tattribute:%#018lx\taddress:%#018lx\t",i,(page + i)->attribute,(page + i)->phy_address);
		i++;
		color_printk(INDIGO,BLACK,"page%d\tattribute:%#018lx\taddress:%#018lx\n",i,(page + i)->attribute,(page + i)->phy_address);
	}
	color_printk(RED,BLACK,"PMMngr.bits_map:%#018lx\n",*PMMngr.bits_map);
	color_printk(RED,BLACK,"PMMngr.bits_map:%#018lx\n",*(PMMngr.bits_map + 1));
*/
    while(1)
    {
        hlt();
    }
    return 0;
}