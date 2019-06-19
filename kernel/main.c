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
#include "compositor.h"

#define MEMMAN_ADDR 0x003c0000

int kx, ky;

void putfonts8_asc_sht(struct SHEET *sht, int x, int y, int c, int b, char *s, int l);
void task_console();

void main(void)
{
    struct BOOTINFO *binfo = (struct BOOTINFO*) 0x0ff0;
    int mx, my, i, cursor_x, cursor_c;
	char s[40];
    SHTCTL *shtctl;
	SHEET *sht_back, *sht_mouse;
	window *win;
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

	shtctl = compositor_init(memman, binfo);
	sht_back  = sheet_alloc(shtctl);
	sht_mouse = sheet_alloc(shtctl);
	win = create_window(80, 72, 160, 52, "window");
	buf_back = (unsigned char *) memman_alloc_4k(memman, binfo->scrnx * binfo->scrny);
	sheet_setbuf(sht_back, buf_back, binfo->scrnx, binfo->scrny, -1); /* 没有透明色 */
	sheet_setbuf(sht_mouse, mcursor, 16, 16, 99); /* 透明色号99 */
	init_screen8(buf_back, binfo->scrnx, binfo->scrny);
	init_mouse_cursor(mcursor, 99);
	make_textbox(win->sht, 8, 28, 144, 16, COL8_FFFFFF);
	cursor_x = 8;
	cursor_c = COL8_FFFFFF;
	sheet_slide(sht_back, 0, 0);
	mx = (binfo->scrnx - 16) / 2;
    my = (binfo->scrny - 28 - 16) / 2;  
    sheet_slide(sht_mouse, mx, my);
	sheet_updown(sht_back,  0);
	sheet_updown(sht_mouse, 10);
	
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
	task_run(task_cons, 0);

	while (1) {
		int scode = keyboard_read();
		if (scode != -1) {
			if (keymap[scode*3] == BACKSPACE && cursor_x > 8) { /* 退格键 */
				/* 用空格键把光标消去后，后移1次光标 */
				cursor_x -= 8;
				putfonts8_asc_sht(win->sht, cursor_x, 28, COL8_000000, COL8_FFFFFF, " ", 1);
			} else
			{
				sprintf(s, "%c", keymap[scode*3]);
				putfonts8_asc_sht(win->sht, cursor_x, 28, COL8_000000, COL8_FFFFFF, s, 1);
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
			boxfill8(win->win_buf, win->sht->bxsize, cursor_c, cursor_x, 28, cursor_x + 3, 43);
			sheet_refresh(win->sht, cursor_x, 28, cursor_x + 8, 44);
			timer_settime(timer, 400);
			fifo8_get(&timerinfo);
		}
	}
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

void task_console()
{
	struct FIFO8 fifo;
	struct TIMER *timer;
	window *win;
	int i, fifobuf[8], cursor_x = 8, cursor_c = COL8_000000;
	char s[40];
	fifo8_init(&fifo, 8, fifobuf, task_now());
	timer = timer_alloc();
	timer_init(timer, &fifo, 4);
	timer_settime(timer, 400);
	int count = 0;

	win = create_window(160, 130, 256, 165, "terminal");
	make_textbox(win->sht, 8, 28, 240, 128, COL8_000000);

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
				boxfill8(win->win_buf, win->sht->bxsize, cursor_c, cursor_x, 28, cursor_x + 7, 43);
				sheet_refresh(win->sht, cursor_x, 28, cursor_x + 8, 44);
			}
		}
	}
}