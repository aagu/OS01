#include <descriptor.h>
#include <interrupt.h>
#include <video.h>
#include <mouse.h>
#include "printk.h"

unsigned char *vram;/* 声明变量vram、用于BYTE [...]地址 */
int mouse_decode(struct MOUSE_DEC *mdec, unsigned char dat);

void main(void)
{
    int mx = 0, my = 0, i;
    char mcursor[256];
    char s[40];
	clear_screen();
	int xsize, ysize;
	vram = (unsigned char *) 0xa0000;/* 地址变量赋值 */
	xsize = 320;
	ysize = 200;

    MOUSE_DEC mdec;

	init_gdt();
	init_idt();

	init_palette();/* 设定调色板 */

	init_keyboard();
	/* 根据 0xa0000 + x + y * 320 计算坐标 8*/
	boxfill8(vram, xsize, COL8_848484,  0,         0,          xsize -  1, ysize - 21);
	boxfill8(vram, xsize, COL8_C6C6C6,  0,         ysize - 20, xsize -  1, ysize - 19);
	boxfill8(vram, xsize, COL8_FFFFFF,  0,         ysize - 19, xsize -  1, ysize - 18);
	boxfill8(vram, xsize, COL8_C6C6C6,  0,         ysize - 18, xsize -  1, ysize -  1);

	mx = (xsize - 16) / 2;
    my = (ysize - 28 - 16) / 2;  
    init_mouse_cursor(mcursor, COL8_848484);
	putblock(vram, xsize, 16, 16, mx, my, mcursor, 16);
	
	init_mouse();
    mdec.phase = 0;
	//asm volatile("int $44");
	for (;;) {
		//io_hlt();
		keyboard_read();
        io_cli();
        i = mouse_read();
		io_sti();
		if (mouse_decode(&mdec, i) != 0) {
			/* 3字节都凑齐了，所以把它们显示出来*/
			if ((mdec.btn & 0x01) != 0) {
				s[1] = 'L';
			}
			if ((mdec.btn & 0x02) != 0) {
				s[3] = 'R';
			}
			if ((mdec.btn & 0x04) != 0) {
				s[2] = 'C';
			}
			boxfill8(vram, 320, COL8_848484, 32, 16, 32 + 15 * 8 - 1, 31);
			showString(vram, 320, 32, 16, COL8_FFFFFF, s);
			/* 鼠标指针的移动 */
			boxfill8(vram, 320, COL8_848484, mx, my, mx + 15, my + 15); /* 隐藏鼠标 */
			mx += mdec.x;
			my += mdec.y;
			if (mx < 0) {
						mx = 0;
			}
			if (my < 0) {
					my = 0;
			}
			if (mx > 320 - 16) {
						mx = 320 - 16;
			}
			if (my > 200 - 16) {
						my = 200 - 16;
			}
			putblock(vram, 320, 16, 16, mx, my, mcursor, 16); /* 描画鼠标 */
		}
	}
}

int mouse_decode(struct MOUSE_DEC *mdec, unsigned char dat){
	if (mdec->phase == 0) {
		/* 等待鼠标的0xfa的阶段 */
		if (dat == 0xfa) {
			mdec->phase = 1;
		}        
		return 0;
	}
	if (mdec->phase == 1) {
		/* 等待鼠标第一字节的阶段 */
		mdec->buf[0] = dat;
		mdec->phase = 2;
		return 0;
	}
	if (mdec->phase == 2) {
		/* 等待鼠标第二字节的阶段 */
		mdec->buf[1] = dat;
		mdec->phase = 3;
		return 0;
	}
	if (mdec->phase == 3) {
		/* 等待鼠标第二字节的阶段 */
		mdec->buf[2] = dat;
		mdec->phase = 1;
		mdec->btn = mdec->buf[0] & 0x07;
		mdec->x = mdec->buf[1];
		mdec->y = mdec->buf[2];
		if ((mdec->buf[0] & 0x10) != 0) {
			mdec->x |= 0xffffff00;
		}
		if ((mdec->buf[0] & 0x20) != 0) {
			mdec->y |= 0xffffff00;
		}     
		/* 鼠标的y方向与画面符号相反 */   
		mdec->y = - mdec->y; 
		return 1;
	}
	/* 应该不可能到这里来 */
	return -1; 
}