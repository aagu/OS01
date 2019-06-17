#include "interrupt.h"
#include "font.h"
#include "video.h"
#include "mouse.h"
#include "printk.h"
#include "sheet.h"
#include "keymap.h"
#include "memory.h"
#include "kernel.h"
#include "timer.h"
#include "task.h"

#define MEMMAN_ADDR 0x003c0000

int kx, ky;

void make_window(unsigned char *buf, int xsize, int ysize, char *title);
void putfonts8_asc_sht(struct SHEET *sht, int x, int y, int c, int b, char *s, int l);
void task_console(struct SHEET *sht_cons);

void main(void)
{
    struct BOOTINFO *binfo = (struct BOOTINFO*) 0x0ff0;
    int mx, my, i, cursor_x, cursor_c;
	char s[40];
    SHTCTL *shtctl;
	SHEET *sht_back, *sht_mouse, *sht_win, *sht_cons;
	struct TIMER *timer;
	struct FIFO8 timerinfo;
	unsigned char timerbuf[8], taskbuf[8];
	unsigned char *buf_back, mcursor[256], *buf_win, *buf_cons;

	unsigned int memtotal;
	struct MEMMAN *memman = (struct MEMMAN *) MEMMAN_ADDR;
	struct TASK *task_a, *task_cons;

	init_gdt();
	init_idt();

	io_sti();

	init_pit();
	
	init_keyboard(task_a);
	init_mouse();

	task_a = task_init(memman);

	timer = timer_alloc();
	timer_init(timer, &timerinfo, 1);
	timer_settime(timer, 400);
	fifo8_init(&timerinfo, 8, timerbuf, task_a);

	init_palette();/* 设定调色板 */
	memtotal = memtest(0x00400000, 0xbfffffff);
	memman_init(memman);
	memman_free(memman, 0x00400000, memtotal - 0x00400000);

	shtctl = shtctl_init(memman, binfo->vram, binfo->scrnx, binfo->scrny);
	sht_back  = sheet_alloc(shtctl);
	sht_mouse = sheet_alloc(shtctl);
	sht_win = sheet_alloc(shtctl);
	sht_cons = sheet_alloc(shtctl);
	buf_back = (unsigned char *) memman_alloc_4k(memman, binfo->scrnx * binfo->scrny);
	buf_win = (unsigned char *) memman_alloc_4k(memman, 160*52);
	buf_cons = (unsigned char *) memman_alloc_4k(memman, 256*165);
	sheet_setbuf(sht_back, buf_back, binfo->scrnx, binfo->scrny, -1); /* 没有透明色 */
	sheet_setbuf(sht_mouse, mcursor, 16, 16, 99); /* 透明色号99 */
	sheet_setbuf(sht_win, buf_win, 160, 52, -1);
	sheet_setbuf(sht_cons, buf_cons, 256, 165, -1);
	init_screen8(buf_back, binfo->scrnx, binfo->scrny);
	init_mouse_cursor(mcursor, 99);
	make_window(buf_win, 160, 52, "window");
	make_textbox(sht_win, 8, 28, 144, 16, COL8_FFFFFF);
	cursor_x = 8;
	cursor_c = COL8_FFFFFF;
	sheet_slide(sht_back, 0, 0);
	mx = (binfo->scrnx - 16) / 2;
    my = (binfo->scrny - 28 - 16) / 2;  
    sheet_slide(sht_mouse, mx, my);
	sheet_slide(sht_win, 80, 72);
	sheet_updown(sht_back,  0);
	sheet_updown(sht_mouse, 3);
	sheet_updown(sht_win, 1);
	
	setscrnbuf(sht_mouse);

	task_cons = task_alloc();
	task_cons->tss.esp = memman_alloc_4k(memman, 64 * 1024) + 64 * 1024 - 8;
	task_cons->tss.eip = (int) &task_console;
	task_cons->tss.es = 2 * 8;
	task_cons->tss.cs = 1 * 8;
	task_cons->tss.ss = 2 * 8;
	task_cons->tss.ds = 2 * 8;
	task_cons->tss.fs = 2 * 8;
	task_cons->tss.gs = 2 * 8;
	*((int *) (task_cons->tss.esp + 4)) = (int) sht_cons;
	task_run(task_cons, 0);

	while (1) {
		int scode = keyboard_read();
		if (scode != -1) {
			if (keymap[scode*3] == BACKSPACE && cursor_x > 8) { /* 退格键 */
				/* 用空格键把光标消去后，后移1次光标 */
				cursor_x -= 8;
				putfonts8_asc_sht(sht_win, cursor_x, 28, COL8_000000, COL8_FFFFFF, " ", 1);
			} else
			{
				sprintf(s, "%c", keymap[scode*3]);
				putfonts8_asc_sht(sht_win, cursor_x, 28, COL8_000000, COL8_FFFFFF, s, 1);
				cursor_x += 8;
			}
		}
		if (fifo8_status(&timerinfo) != 0)
		{
			if (cursor_c == COL8_000000)
			{
				cursor_c = COL8_FFFFFF;
			} else
			{
				cursor_c = COL8_000000;
			}
			boxfill8(sht_win->buf, sht_win->bxsize, cursor_c, cursor_x, 28, cursor_x + 3, 43);
			sheet_refresh(sht_win, cursor_x, 28, cursor_x + 8, 44);
			timer_settime(timer, 400);
			fifo8_get(&timerinfo);
		}
	}
}

void make_window(unsigned char *buf, int xsize, int ysize, char *title)
{
	static char closebtn[14][16] = {
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

	int x, y;
	char c;
	boxfill8(buf, xsize, COL8_C6C6C6, 0, 0, xsize - 1, 0 );
	boxfill8(buf, xsize, COL8_FFFFFF, 1, 1, xsize - 2, 1 );
	boxfill8(buf, xsize, COL8_C6C6C6, 0, 0, 0, ysize - 1);
	boxfill8(buf, xsize, COL8_FFFFFF, 1, 1, 1, ysize - 2);
	boxfill8(buf, xsize, COL8_848484, xsize - 2, 1, xsize - 2, ysize - 2);
	boxfill8(buf, xsize, COL8_000000, xsize - 1, 0, xsize - 1, ysize - 1);
	boxfill8(buf, xsize, COL8_C6C6C6, 2, 2, xsize - 3, ysize - 3);
	boxfill8(buf, xsize, COL8_000084, 3, 3, xsize - 4, 20 );
	boxfill8(buf, xsize, COL8_848484, 1, ysize - 2, xsize - 2, ysize - 2);
	boxfill8(buf, xsize, COL8_000000, 0, ysize - 1, xsize - 1, ysize - 1);
	showString(buf, xsize, 24, 4, COL8_FFFFFF, title);

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
			buf[(5 + y) * xsize + (xsize - 21 + x)] = c;
		}
	}
	return;
}

void putfonts8_asc_sht(struct SHEET *sht, int x, int y, int c, int b, char *s, int l)
{
	boxfill8(sht->buf, sht->bxsize, b, x, y, x + l * 8 - 1, y + 15);
	showString(sht->buf, sht->bxsize, x, y, c, s);
	sheet_refresh(sht, x, y, x + l * 8, y + 16);
	return;
}

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
	boxfill8(sht->buf, sht->bxsize, c, x0 - 1, y0 - 1, x1 + 0, y1 + 0);
	return;
}

void task_console(struct SHEET *sht_cons)
{
	struct FIFO8 fifo;
	struct TIMER *timer;
	int i, fifobuf[8], cursor_x = 8, cursor_c = COL8_000000;
	char s[40];
	fifo8_init(&fifo, 8, fifobuf, task_now());
	timer = timer_alloc();
	timer_init(timer, &fifo, 4);
	timer_settime(timer, 400);
	int count = 0;

	make_window(sht_cons->buf, 256, 165, "console");
	make_textbox(sht_cons, 8, 28, 240, 128, COL8_000000);

	sheet_slide(sht_cons, 160, 130);
	sheet_updown(sht_cons, 2);

	while (1) {
		count++;
		if (fifo8_status(&fifo) != 0) {
			i = fifo8_get(&fifo);
			if (i == 4)
			{
				if (cursor_c == COL8_000000)
				{
					cursor_c = COL8_FFFFFF;
				} else
				{
					cursor_c = COL8_000000;
				}
				timer_settime(timer, 400);
				boxfill8(sht_cons->buf, sht_cons->bxsize, cursor_c, cursor_x, 28, cursor_x + 7, 43);
				sheet_refresh(sht_cons, cursor_x, 28, cursor_x + 8, 44);
			}
		}
	}
}