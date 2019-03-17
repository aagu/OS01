#include "descriptor.h"
#include "interrupt.h"
#include "video.h"
#include "mouse.h"
#include "printk.h"
#include "sheet.h"
#include "keymap.h"
#include "memory.h"

#define MEMMAN_ADDR 0x003c0000

int kx, ky;

void main(void)
{
    struct BOOTINFO *binfo = (struct BOOTINFO*) 0x0ff0;
    int mx, my, i;
	char s[40];
    SHTCTL *shtctl;
	SHEET *sht_back, *sht_mouse;
	unsigned char *buf_back, mcursor[256];

	unsigned int memtotal;
	struct MEMMAN *memman = (struct MEMMAN *) MEMMAN_ADDR;

	init_gdt();
	init_idt();

	io_sti();

	init_keyboard();
	init_mouse();
	
	init_palette();/* 设定调色板 */
	memtotal = memtest(0x00400000, 0xbfffffff);
	memman_init(memman);
	memman_free(memman, 0x00400000, memtotal - 0x00400000);

	shtctl = shtctl_init(memman, binfo->vram, binfo->scrnx, binfo->scrny);
	sht_back  = sheet_alloc(shtctl);
	sht_mouse = sheet_alloc(shtctl);
	buf_back = (unsigned char *) memman_alloc_4k(memman, binfo->scrnx * binfo->scrny);
	sheet_setbuf(sht_back, buf_back, binfo->scrnx, binfo->scrny, -1); /* 没有透明色 */
	sheet_setbuf(sht_mouse, mcursor, 16, 16, 99); /* 透明色号99 */
	init_screen8(buf_back, binfo->scrnx, binfo->scrny);
	init_mouse_cursor(mcursor, 99);
	sheet_slide(sht_back, 0, 0);
	mx = (binfo->scrnx - 16) / 2;
    my = (binfo->scrny - 28 - 16) / 2;  
    sheet_slide(sht_mouse, mx, my);
	sheet_updown(sht_back,  0);
	sheet_updown(sht_mouse, 1);
	
	setscrnbuf(sht_mouse);

	for (;;) {
		int scode = keyboard_read();
		if (scode != -1) {
			if (kx >= binfo->scrnx) {
				kx = 0;
				ky += 16;
				if (ky > binfo->scrny) ky = 0;
			}
			boxfill8(buf_back, binfo->scrnx, COL8_848484, kx, ky, kx+16, ky+16); /* 文字 */
			showFont8(buf_back, binfo->scrnx, kx, ky, COL8_FFFFFF, systemFont + (unsigned char)keymap[scode*3]*16); /* 写文字 */
			sheet_refresh(sht_back, kx, ky, kx+16, ky+16); /* 刷新文字 */
			kx += 16;
		}
	}
}