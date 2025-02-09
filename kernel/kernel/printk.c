#include <kernel/printk.h>
#include <kernel/memory.h>
#include <kernel/vmm.h>
#include <kernel/pmm.h>
#include <kernel/slab.h>
#include <driver/serial.h>
#include <stdio.h>
#include <font.h>
#include <stddef.h>
#include <string.h>

static char buf[4096]={0};

position Pos;

psf2_t *font = (psf2_t*)&_binary_kernel_font_psf_start;

void putchark(unsigned int FRcolor,unsigned int BKcolor,unsigned char c)
{
    uint32_t i = 0,j = 0;
	uint32_t * addr = NULL;
	int testval = 0;
	unsigned char *glyph = (unsigned char*)&_binary_kernel_font_psf_start + font->headersize +
            (c>0&&c<font->numglyph?c:0)*font->bytesperglyph;

	for(i = 0; i < font->height;i++)
	{
		addr = Pos.FB_addr + Pos.XResolution * ( Pos.YPosition * font->height + i ) + Pos.XPosition * font->width;
		testval = 0x100;
		for(j = 0;j < font->width;j++)		
		{
			testval = testval >> 1;
			if(*glyph & testval)
				*addr = FRcolor;
			else
				*addr = BKcolor;
			addr++;
		}
		glyph++;		
	}
}

int color_printk(unsigned int FRcolor,unsigned int BKcolor,const char * fmt,...)
{
	int i = 0;
	int count = 0;
	int line = 0;
	va_list args;

	va_start(args, fmt);
	i = vsprintf(buf, fmt, args);
	va_end(args);

	spin_lock(&Pos.lock);
	for(count = 0;count < i || line;count++)
	{
		if(line > 0)
		{
			count--;
			goto Label_tab;
		}
		if((unsigned char)*(buf + count) == '\n')
		{
			Pos.YPosition++;
			Pos.XPosition = 0;
		}
		else if((unsigned char)*(buf + count) == '\b')
		{
			Pos.XPosition--;
			if(Pos.XPosition < 0)
			{
				Pos.XPosition = (Pos.XResolution / font->width - 1) * font->width;
				Pos.YPosition--;
				if(Pos.YPosition < 0)
					Pos.YPosition = (Pos.YResolution / font->height - 1) * font->height;
			}
			putchark(FRcolor , BKcolor , ' ');	
		}
		else if((unsigned char)*(buf + count) == '\t')
		{
			line = ((Pos.XPosition + 8) & ~(8 - 1)) - Pos.XPosition;

Label_tab:
			line--;
			putchark(FRcolor , BKcolor , ' ');	
			Pos.XPosition++;
		}
		else
		{
			putchark(FRcolor , BKcolor , (unsigned char)*(buf + count));
			Pos.XPosition++;
		}


		if(Pos.XPosition >= (int32_t)(Pos.XResolution / font->width))
		{
			Pos.YPosition++;
			Pos.XPosition = 0;
		}
		if(Pos.YPosition >= (int32_t)(Pos.YResolution / font->height))
		{
			Pos.YPosition = 0;
		}

	}
	spin_unlock(&Pos.lock);

	return i;
}

// we only have 1 pde for now, must ensure vbe frame buffer be mapped inside this pde
void frame_buffer_early_init()
{
	uint64_t *pml2 = (uint64_t *)0xffff800000103000;
	for (uintptr_t i = 0; i < Pos.FB_length; i += PAGE_2M_SIZE)
	{
		size_t level2 = (size_t) ((0xffff800000e00000 + i) >> PAGE_2M_SHIFT) & 0x1ff;
		pml2[level2] = (((uint64_t)Pos.Phy_addr + i) & PAGE_2M_MASK) | (PAGE_KERNEL_Page | PAGE_PWT | PAGE_PCD);
	}
	Pos.FB_addr = (uint32_t *)0xffff800000e00000;

	flush_tlb();
}

// after pmm and vmm properly set up, we can map vbe frame buffer to memory hole now
void frame_buffer_init()
{
	for (uintptr_t i = 0; i < Pos.FB_length; i += PAGE_2M_SIZE)
	{
		vmm_map_page(kernel_map, i + (uint64_t)Pos.Phy_addr, VIRT_FRAMEBUFFER_OFFSET + i, PAGE_KERNEL_Page | PAGE_PWT | PAGE_PCD);
	}
	Pos.FB_addr = (uint32_t *)VIRT_FRAMEBUFFER_OFFSET;

	flush_tlb();
}

void serial_printk(const char * fmt,...)
{
	int i = 0;
	int count = 0;
	va_list args;

	va_start(args, fmt);
	i = vsprintf(buf, fmt, args);
	va_end(args);

	for(count = 0;count < i ;count++)
	{
		write_serial((unsigned char)*(buf + count));
	}
}