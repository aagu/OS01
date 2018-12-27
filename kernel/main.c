#include "descriptor.h"
#include "interrupt.h"
#include "video.h"
#include "mouse.h"
#include "printk.h"
#include "sheet.h"
#include "mm.h"

char s[40];

void main(void)
{
    struct BOOTINFO *binfo = (struct BOOTINFO*) 0x0ff0;
    int mx = 0, my = 0, i;
	unsigned int memtotal;
	MOUSE_DEC mdec;
	MEMMAN *memman = (MEMMAN *) MEMMAN_ADDR;
    SHTCTL *shtctl;
	SHEET *sht_back, *sht_mouse;
	unsigned char *buf_back, mcursor[256];

	init_gdt();
	init_idt();

	io_sti();

	init_keyboard();
	init_mouse();
    mdec.phase = 0;
	
	init_palette();/* 设定调色板 */

	//memtotal = memtest(0x00400000, 0xbfffffff);
	//vsprintf(s, "%x", memtotal);
	//showString(binfo->vram, binfo->scrnx, 8, 32, COL8_FFFFFF, s);
	//memman_init(memman);
	//memman_free(memman, 0x00001000, 0x0009e000); /* 0x00001000 - 0x0009efff */
	//memman_free(memman, 0x00400000, memtotal - 0x00400000);

	shtctl = shtctl_init(binfo->vram, binfo->scrnx, binfo->scrny);
	sht_back  = sheet_alloc(shtctl);
	sht_mouse = sheet_alloc(shtctl);
	//buf_back  = (unsigned char *) memman_alloc_4k(memman, binfo->scrnx * binfo->scrny);
	buf_back = (unsigned char *) 0x400512;
	sheet_setbuf(sht_back, buf_back, binfo->scrnx, binfo->scrny, -1); /* 没有透明色 */
	sheet_setbuf(sht_mouse, mcursor, 16, 16, 99); /* 透明色号99 */
	init_screen8(buf_back, binfo->scrnx, binfo->scrny);
	init_mouse_cursor(mcursor, 99);
	mx = (binfo->scrnx - 16) / 2;
    my = (binfo->scrny - 28 - 16) / 2;  
    sheet_slide(shtctl, sht_mouse, mx, my);
	sheet_updown(shtctl, sht_back,  0);
	sheet_updown(shtctl, sht_mouse, 1);
	//putblock(binfo->vram, binfo->scrnx, 16, 16, mx, my, mcursor, 16);
	sheet_refresh(shtctl, sht_back, 0, 0, binfo->scrnx, 48); /* 刷新文字 */
	
	//asm volatile("int $44");
	for (;;) {
		io_cli();
		int scode = keyboard_read();
		io_sti();
		//vsprintf(s, "0x%02X", scode);
		boxfill8(buf_back, binfo->scrnx, COL8_848484, 0, 0, 79, 15); /* 文字 */
		showString(buf_back, binfo->scrnx, 0, 0, COL8_FFFFFF, s); /* 写文字 */
		sheet_refresh(shtctl, sht_back, 0, 0, 80, 16); /* 刷新文字 */
		s[0] = 'n';
		s[1] = '\0';
        //io_cli();
        //i = mouse_read();
		//io_sti();
		//if (mouse_decode(&mdec, i) != 0) {
			/* 3字节都凑齐了，所以把它们显示出来*/
			//vsprintf(s, "[lcr %4d %4d", mdec.x, mdec.y);
			//if ((mdec.btn & 0x01) != 0) {
			//	s[1] = 'L';
			//}
			//if ((mdec.btn & 0x02) != 0) {
			//	s[3] = 'R';
			//}
			//if ((mdec.btn & 0x04) != 0) {
			//	s[2] = 'C';
			//}
			//boxfill8(binfo->vram, binfo->scrnx, COL8_848484, 32, 16, 32 + 15 * 8 - 1, 31);
			//showString(binfo->vram, binfo->scrnx, 32, 16, COL8_FFFFFF, s);
			/* 鼠标指针的移动 */
			//sheet_refresh(shtctl, sht_back, 32, 16, 32 + 15 * 8, 32);  /* 刷新文字 */
			//mx += mdec.x;
			//my += mdec.y;
			//if (mx < 0) {
						mx = 0;
			//}
			//if (my < 0) {
			//		my = 0;
			//}
			//if (mx > binfo->scrnx - 16) {
			//			mx = binfo->scrnx - 16;
			//}
			//if (my > binfo->scrny - 16) {
			//			my = binfo->scrny - 16;
			//}
			//vsprintf(s, "(%3d, %3d)", mx, my);
			//boxfill8(buf_back, binfo->scrnx, COL8_848484, 0, 0, 79, 15); /* 消坐标 */
			//showString(buf_back, binfo->scrnx, 0, 0, COL8_FFFFFF, s); /* 写坐标 */
			//sheet_refresh(shtctl, sht_back, 0, 0, 80, 16); /* 刷新文字 */
			//sheet_slide(shtctl, sht_mouse, mx, my); /* 包含sheet_refresh含sheet_refresh */
		//}
	}
}

void interrupt_callback(void)
{
	s[0] = 'k';
	s[1] = 'e';
	s[2] = 'y';
	s[3] = 'b';
	s[4] = 'o';
	s[5] = 'a';
	s[6] = 'r';
	s[7] = 'd';
	s[8] = '\0';
}