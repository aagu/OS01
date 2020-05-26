#include "interrupt.h"
#include "video.h"
#include "printk.h"
#include "keymap.h"
#include "timer.h"
#include "task.h"
#include "stdio.h"

#define MEMMAN_ADDR 0x003c0000

void task_console()
{
	struct FIFO8 fifo;
	struct TIMER *timer;
	unsigned char fifobuf[8];
	char s[40];
	fifo8_init(&fifo, 8, fifobuf, task_now());
	timer = timer_alloc();
	timer_init(timer, &fifo, 4);
	timer_settime(timer, 1000);
	int count = 0;

	while (1) {
		count++;
		if (fifo8_status(&fifo) != 0) {
			printk("task console: timer up!\n");
			break;
		}
	}
	task_free(task_now());
}

void idle_task()
{
	for (;;)
	{
		io_hlt();
	}
	
}

void main(void)
{
    struct BOOTINFO *binfo = (struct BOOTINFO*) 0x0ff0;
	char s[40];

	struct TIMER *timer;
	struct FIFO8 timerinfo;
	unsigned char timerbuf[8], taskbuf[8];

	unsigned int memtotal;
	struct MEMMAN *memman = (struct MEMMAN *) MEMMAN_ADDR;
	struct TASK *task_a, *task_cons;

	init_gdt();
	init_idt();

	io_sti();

	init_pit();
	
	init_keyboard(task_a);

	task_a = task_init(memman);

	timer = timer_alloc();
	timer_init(timer, &timerinfo, 1);
	timer_settime(timer, 1000);
	fifo8_init(&timerinfo, 8, timerbuf, task_a);

	memtotal = memtest(0x00400000, 0xbfffffff);
	memman_init(memman);
	memman_free(memman, 0x00400000, memtotal - 0x00400000);

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

	int i = 0;
	while (i < 50) {
		if (fifo8_status(&timerinfo) != 0)
		{
			printk("%d times up, the CPU has passed 10000 ticks\n", ++i);
			timer_settime(timer, 10000);
			fifo8_get(&timerinfo);
		}
		io_hlt();
	}
	printk("main loop break out\n");
	io_cli();
	io_hlt();
}