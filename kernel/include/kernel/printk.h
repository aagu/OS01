#ifndef _KERNEL_PRINTK_H
#define _KERNEL_PRINTK_H

#include <stdint.h>

#define WHITE 	0x00ffffff		//白
#define BLACK 	0x00000000		//黑
#define RED	    0x00ff0000		//红
#define ORANGE	0x00ff8000		//橙
#define YELLOW	0x00ffff00		//黄
#define GREEN	0x0000ff00		//绿
#define BLUE	0x000000ff		//蓝
#define INDIGO	0x0000ffff		//靛
#define PURPLE	0x008000ff		//紫

#define VIRT_FRAMEBUFFER_OFFSET 0xffff8000e0000000

typedef struct position
{
	int32_t XResolution;
	int32_t YResolution;

	int32_t XPosition;
	int32_t YPosition;

	uint32_t * Phy_addr;
	uint32_t * FB_addr; // memory should be write in 32bits block
	uint64_t FB_length;
} position;

extern position Pos;

void putchark(unsigned int FRcolor,unsigned int BKcolor,unsigned char font);
int color_printk(unsigned int FRcolor,unsigned int BKcolor,const char * fmt,...);
void frame_buffer_init();
void frame_buffer_early_init();

void serial_printk(const char * fmt,...);

#endif