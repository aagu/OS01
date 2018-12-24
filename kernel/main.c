#include "io.h"
#include "video.h"
#include "mem.h"
#include "interupt.h"

extern char systemFont[16];

unsigned char *vram;/* 声明变量vram、用于BYTE [...]地址 */

void main(void)
{
	int xsize, ysize;
	char mcursor[256];
	vram = (unsigned char *) 0xa0000;/* 地址变量赋值 */
	xsize = 320;
	ysize = 200;

	init_gdtidt();
	init_pic();
	io_sti();
	init_palette();/* 设定调色板 */
	

	/* 根据 0xa0000 + x + y * 320 计算坐标 8*/
	boxfill8(vram, xsize, COL8_848484,  0,         0,          xsize -  1, ysize - 21);
	boxfill8(vram, xsize, COL8_C6C6C6,  0,         ysize - 20, xsize -  1, ysize - 19);
	boxfill8(vram, xsize, COL8_FFFFFF,  0,         ysize - 19, xsize -  1, ysize - 18);
	boxfill8(vram, xsize, COL8_C6C6C6,  0,         ysize - 18, xsize -  1, ysize -  1);

	init_mouse_cursor(mcursor, COL8_848484);
	putblock(vram, xsize, 16, 16, 80, 80, mcursor, 16);

	io_out8(PIC0_IMR, 0xf9); /* 开放PIC1和键盘中断(11111001) */
	io_out8(PIC1_IMR, 0xef); /* 开放鼠标中断(11101111) */

	for (;;) {
		io_hlt();
	}
}
