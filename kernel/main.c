#include "descriptor.h"
#include "interrupt.h"
#include "video.h"
#include "mouse.h"
#include "printk.h"
#include "sheet.h"
#include "keymap.h"
int cx,cy;
void main(void)
{
    struct BOOTINFO *binfo = (struct BOOTINFO*) 0x0ff0;
    int mx = 0, my = 0, i;
	char s[40];
	MOUSE_DEC mdec;
    SHTCTL *shtctl;
	SHEET *sht_back, *sht_mouse;
	unsigned char *buf_back, mcursor[256];

	init_gdt();
	init_idt();

	io_sti();

	init_keyboard();
	init_mouse();
    mdec.phase = 0;
	cx = 8;
	cy = 32;
	
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
	
	showString((unsigned char *) binfo->vram, binfo->scrnx, 0, 16, COL8_000084, "Welcome to my OS");
	for (;;) {
		io_hlt();
		int scode = keyboard_read();
		if (scode != -1) {
			if(cx >= 320){
				cx = 8;
				cy += 16;
				if(cy>100) cy =32;
			}

			// boxfill8(buf_back, binfo->scrnx, COL8_848484, 0, 0, 79, 15); /* 文字 */
			// showFont8(buf_back, binfo->scrnx, 4, 0, COL8_FFFFFF, systemFont + (unsigned char)keymap[scode*3]*16); /* 写文字 */
			showFont8((unsigned char *) binfo->vram, binfo->scrnx, cx, cy, COL8_FFFFFF, systemFont+  keymap[scode*3] * 16);
			cx+=8;


			sheet_refresh(shtctl, sht_back, 0, 0, 80, 16); /* 刷新文字 */
		}
		
        i = mouse_read();
		io_sti();
		if (i != -1) {
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
				/* 鼠标指针的移动 */
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
				sheet_slide(shtctl, sht_mouse, mx, my); /* 包含sheet_refresh含sheet_refresh */
			}
		}
	}
}
