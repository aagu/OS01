#include <descriptor.h>
#include <interrupt.h>
#include <video.h>
#include <mouse.h>
#include "printk.h"

int mouse_decode(struct MOUSE_DEC *mdec, unsigned char dat);

void main(void)
{
    struct BOOTINFO *binfo = (struct BOOTINFO*) 0x0ff0;
    int mx = 0, my = 0, i;
    char mcursor[256];
    char s[40];
	clear_screen();

    MOUSE_DEC mdec;

	init_gdt();
	init_idt();

	init_palette();/* 设定调色板 */

	init_keyboard();
	
	boxfill8(binfo->vram, binfo->scrnx, COL8_848484,  0,         0,          binfo->scrnx-  1, binfo->scrny - 21);
	boxfill8(binfo->vram, binfo->scrnx, COL8_C6C6C6,  0,         binfo->scrny - 20, binfo->scrnx -  1, binfo->scrny - 19);
	boxfill8(binfo->vram, binfo->scrnx, COL8_FFFFFF,  0,         binfo->scrny - 19, binfo->scrnx -  1, binfo->scrny - 18);
	boxfill8(binfo->vram, binfo->scrnx, COL8_C6C6C6,  0,         binfo->scrny - 18, binfo->scrnx -  1, binfo->scrny -  1);

	mx = (binfo->scrnx - 16) / 2;
    my = (binfo->scrny - 28 - 16) / 2;  
    init_mouse_cursor(mcursor, COL8_848484);
	putblock(binfo->vram, binfo->scrnx, 16, 16, mx, my, mcursor, 16);
	
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
			boxfill8(binfo->vram, binfo->scrnx, COL8_848484, 32, 16, 32 + 15 * 8 - 1, 31);
			showString(binfo->vram, binfo->scrnx, 32, 16, COL8_FFFFFF, s);
			/* 鼠标指针的移动 */
			boxfill8(binfo->vram, binfo->scrnx, COL8_848484, mx, my, mx + 15, my + 15); /* 隐藏鼠标 */
			mx += mdec.x;
			my += mdec.y;
			if (mx < 0) {
						mx = 0;
			}
			if (my < 0) {
					my = 0;
			}
			if (mx > binfo->scrnx - 16) {
						mx = binfo->scrnx - 16;
			}
			if (my > binfo->scrny - 16) {
						my = binfo->scrny - 16;
			}
			putblock(binfo->vram, binfo->scrnx, 16, 16, mx, my, mcursor, 16); /* 描画鼠标 */
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