#include "descriptor.h"
#include "interrupt.h"
#include "video.h"
#include "mouse.h"
#include "printk.h"
#include "sheet.h"
#include "keymap.h"

int kx, ky;

unsigned int memtest(unsigned int start, unsigned int end);
unsigned int memtest_sub(unsigned int start, unsigned int end);

void main(void)
{
    struct BOOTINFO *binfo = (struct BOOTINFO*) 0x0ff0;
    int mx, my, i;
	char s[40];
    SHTCTL *shtctl;
	SHEET *sht_back, *sht_mouse;
	unsigned char *buf_back, mcursor[256];

	init_gdt();
	init_idt();

	io_sti();

	init_keyboard();
	init_mouse();
	
	init_palette();/* 设定调色板 */

	shtctl = shtctl_init(binfo->vram, binfo->scrnx, binfo->scrny);
	sht_back  = sheet_alloc(shtctl);
	sht_mouse = sheet_alloc(shtctl);
	buf_back = (unsigned char *) 0x810000;
	sheet_setbuf(sht_back, buf_back, binfo->scrnx, binfo->scrny, -1); /* 没有透明色 */
	sheet_setbuf(sht_mouse, mcursor, 16, 16, 99); /* 透明色号99 */
	init_screen8(buf_back, binfo->scrnx, binfo->scrny);
	init_mouse_cursor(mcursor, 99);
	mx = (binfo->scrnx - 16) / 2;
    my = (binfo->scrny - 28 - 16) / 2;  
    sheet_slide(shtctl, sht_mouse, mx, my);
	sheet_updown(shtctl, sht_back,  0);
	sheet_updown(shtctl, sht_mouse, 1);
	
	setscrnbuf(shtctl, sht_mouse);

	i = memtest(0x00400000, 0xbfffffff) / (1024 * 1024);
	vsprintf(s, "%d", i);
	showString(buf_back, binfo->scrnx, binfo->scrny,
		8, 8, s);
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
			sheet_refresh(shtctl, sht_back, kx, ky, kx+16, ky+16); /* 刷新文字 */
			kx += 16;
		}
	}
}

#define EFLAGS_AC_BIT			0x00040000
#define CR0_CACHE_DISABLE	0x60000000

unsigned int memtest(unsigned int start, unsigned int end) 
{
	char flg486 = 0;
	unsigned int eflg, cr0, i;

	/* 确认CPU是386还是486以上的 */
	eflg = io_load_eflags();
	eflg |= EFLAGS_AC_BIT; /* AC-bit = 1 */
	io_store_eflags(eflg);
	eflg = io_load_eflags();
	if ((eflg & EFLAGS_AC_BIT) != 0) {
		/* 如果是386，即使设定AC=1，AC的值还会自动回到0 */
		flg486 = 1;
	}

	eflg &= ~EFLAGS_AC_BIT; /* AC-bit = 0 */
	io_store_eflags(eflg);

	if (flg486 != 0) {
		cr0 = load_cr0();
		cr0 |= CR0_CACHE_DISABLE; /* 禁止缓存 */ 
		store_cr0(cr0);
	}

	i = memtest_sub(start, end);

	if (flg486 != 0) {
		cr0 = load_cr0();
		cr0 &= ~CR0_CACHE_DISABLE; /* 允许缓存 */
		store_cr0(cr0);
	}

	return i;
}