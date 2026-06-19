#ifndef _KERNEL_PRINTK_H
#define _KERNEL_PRINTK_H

#include <stdint.h>
#include <kernel/arch/x86_64/spinlock.h>

#define WHITE 	0x00ffffff		//白
#define BLACK 	0x00000000		//黑
#define RED	    0x00ff0000		//红
#define ORANGE	0x00ff8000		//橙
#define YELLOW	0x00ffff00		//黄
#define GREEN	0x0000ff00		//绿
#define BLUE	0x000000ff		//蓝
#define INDIGO	0x0000ffff		//靛
#define PURPLE	0x008000ff		//紫

// Temporary framebuffer address used before pmm/vmm are available.
// Within PDPT[0] for simple direct-PDE-write setup in frame_buffer_early_init().
// Placed at 1008MB (PDE index 504) — above all physical RAM for QEMU -m up to ~1GB.
#define VIRT_FRAMEBUFFER_EARLY 0xFFFF80003F000000

// Permanent framebuffer address, remapped by frame_buffer_init() after VMM.
// Uses a separate PML4 entry (PML4[288], 16TB above higher-half base) that
// will NEVER overlap with the physical RAM direct mapping (PML4[256..287]),
// regardless of QEMU -m size.
#define VIRT_FRAMEBUFFER_OFFSET 0xFFFF900000000000

typedef struct position
{
	int32_t XResolution;
	int32_t YResolution;

	int32_t XPosition;
	int32_t YPosition;

	uint32_t * Phy_addr;
	uint32_t * FB_addr; // memory should be write in 32bits block
	uint64_t FB_length;

	spinlock_T lock;
} position;

extern position Pos;

void putchark(unsigned int FRcolor,unsigned int BKcolor,unsigned char font);
int color_printk(unsigned int FRcolor,unsigned int BKcolor,const char * fmt,...);
void frame_buffer_init();
void frame_buffer_early_init();

void serial_printk(const char * fmt,...);

#endif