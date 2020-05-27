#include "compositor.h"
#include "video.h"
#include "printk.h"

const char closebtn[14][16] = {
	"OOOOOOOOOOOOOOO@",
	"OQQQQQQQQQQQQQ$@",
	"OQQQQQQQQQQQQQ$@",
	"OQQQ@@QQQQ@@QQ$@",
	"OQQQQ@@QQ@@QQQ$@",
	"OQQQQQ@@@@QQQQ$@",
	"OQQQQQQ@@QQQQQ$@",
	"OQQQQQ@@@@QQQQ$@",
	"OQQQQ@@QQ@@QQQ$@",
	"OQQQ@@QQQQ@@QQ$@",
	"OQQQQQQQQQQQQQ$@",
	"OQQQQQQQQQQQQQ$@",
	"O$$$$$$$$$$$$$$@",
	"@@@@@@@@@@@@@@@@"
};

void compositor_init(struct BOOTINFO *bootinfo)
{
	binfo = bootinfo;
    shtctl = shtctl_init(binfo->vram, binfo->scrnx, binfo->scrny);
    return;
}

window *create_window(int x0, int y0, int xsize, int ysize, char *title)
{
    window *win;
    win->name = title;
    win->sht = sheet_alloc(shtctl);
    win->win_buf = (unsigned char *) memman_alloc_4k(xsize * ysize);
    sheet_setbuf(win->sht, win->win_buf, xsize, ysize, -1);
    win->depth = shtctl->top + 1; //top为鼠标层
	win->x0 = x0;
	win->y0 = y0;
	win->width = xsize;
	win->height = ysize;
	
    int x, y;
	char c;
	boxfill8(win->win_buf, xsize, COL8_C6C6C6, 0, 0, xsize - 1, 0 );
	boxfill8(win->win_buf, xsize, COL8_FFFFFF, 1, 1, xsize - 2, 1 );
	boxfill8(win->win_buf, xsize, COL8_C6C6C6, 0, 0, 0, ysize - 1);
	boxfill8(win->win_buf, xsize, COL8_FFFFFF, 1, 1, 1, ysize - 2);
	boxfill8(win->win_buf, xsize, COL8_848484, xsize - 2, 1, xsize - 2, ysize - 2);
	boxfill8(win->win_buf, xsize, COL8_000000, xsize - 1, 0, xsize - 1, ysize - 1);
	boxfill8(win->win_buf, xsize, COL8_C6C6C6, 2, 2, xsize - 3, ysize - 3);
	boxfill8(win->win_buf, xsize, COL8_000084, 3, 3, xsize - 4, 20 );
	boxfill8(win->win_buf, xsize, COL8_848484, 1, ysize - 2, xsize - 2, ysize - 2);
	boxfill8(win->win_buf, xsize, COL8_000000, 0, ysize - 1, xsize - 1, ysize - 1);
	showString(win->win_buf, xsize, 24, 4, COL8_FFFFFF, title);

	for (y = 0; y < 14; y++) {
		for (x = 0; x < 16; x++) {
			c = closebtn[y][x];
			if (c == '@') {
				c = COL8_000000;
			} else if (c == '$') {
				c = COL8_848484;
			} else if (c == 'Q') {
				c = COL8_C6C6C6;
			} else {
				c = COL8_FFFFFF;
			}
			win->win_buf[(5 + y) * xsize + (xsize - 21 + x)] = c;
		}
	}
    sheet_slide(win->sht, x0, y0);
    sheet_updown(win->sht, win->depth);
	sheet_updown(sht_mouse, sht_mouse->height+1); //鼠标层高度自增
	return win;
}

void create_background()
{
	sht_back = sheet_alloc(shtctl);
	unsigned char *buf_back = (unsigned char *) memman_alloc_4k(binfo->scrnx * binfo->scrny);
	sheet_setbuf(sht_back, buf_back, binfo->scrnx, binfo->scrny, -1); /* 没有透明色 */
	init_screen8(buf_back, binfo->scrnx, binfo->scrny);
	sheet_slide(sht_back, 0, 0);
	sheet_updown(sht_back,  0);
}

void create_mouse()
{
	sht_mouse = sheet_alloc(shtctl);
	sheet_setbuf(sht_mouse, cursor, 16, 16, 99); /* 透明色号99 */
	init_mouse_cursor(cursor, 99);
	int mx = (binfo->scrnx - 16) / 2;
    int my = (binfo->scrny - 28 - 16) / 2;  
	sheet_slide(sht_mouse, mx, my);
	setscrnbuf(sht_mouse);
	sheet_updown(sht_mouse, 1);
}

/**
 * 绘制文本框
 * *sht 图层
 * x0, y0 原点
 * sx, sy 宽和高
 * c 背景色
*/
void make_textbox(struct SHEET *sht, int x0, int y0, int sx, int sy, int c)
{
	int x1 = x0 + sx, y1 = y0 + sy;
	boxfill8(sht->buf, sht->bxsize, COL8_848484, x0 - 2, y0 - 3, x1 + 1, y0 - 3);
	boxfill8(sht->buf, sht->bxsize, COL8_848484, x0 - 3, y0 - 3, x0 - 3, y1 + 1);
	boxfill8(sht->buf, sht->bxsize, COL8_FFFFFF, x0 - 3, y1 + 2, x1 + 1, y1 + 2);
	boxfill8(sht->buf, sht->bxsize, COL8_FFFFFF, x1 + 2, y0 - 3, x1 + 2, y1 + 2);
	boxfill8(sht->buf, sht->bxsize, COL8_000000, x0 - 1, y0 - 2, x1 + 0, y0 - 2);
	boxfill8(sht->buf, sht->bxsize, COL8_000000, x0 - 2, y0 - 2, x0 - 2, y1 + 0);
	boxfill8(sht->buf, sht->bxsize, COL8_C6C6C6, x0 - 2, y1 + 1, x1 + 0, y1 + 1);
	boxfill8(sht->buf, sht->bxsize, COL8_C6C6C6, x1 + 1, y0 - 2, x1 + 1, y1 + 1);
	boxfill8(sht->buf, sht->bxsize, c,           x0 - 1, y0 - 1, x1 + 0, y1 + 0);
	sheet_refresh(sht, x0, y0, x1, y1);
	return;
}