#include "compositor.h"
#include "video.h"

SHTCTL *compositor_init(struct MEMMAN  *memman, struct BOOTINFO *bootinfo)
{
    mem = memman;
    shtctl = shtctl_init(memman, bootinfo->vram, bootinfo->scrnx, bootinfo->scrny);
    return shtctl;
}

window *create_window(int x0, int y0, int xsize, int ysize, char *title)
{
    window *win;
    win->name = title;
    win->sht = sheet_alloc(shtctl);
    win->win_buf = (unsigned char *) memman_alloc_4k(mem, xsize * ysize);
    sheet_setbuf(win->sht, win->win_buf, xsize, ysize, -1);
    win->depth = shtctl->top + 1;

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
	return win;
}