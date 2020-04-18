#include "interrupt.h"
#include "video.h"
#include "printk.h"
#include "keymap.h"
#include "timer.h"
#include "task.h"
#include "stdio.h"

#define MEMMAN_ADDR 0x003c0000

void main(void)
{
    struct BOOTINFO *binfo = (struct BOOTINFO*) 0x0ff0;
	char s[40];

	struct TIMER *timer;
	struct FIFO8 timerinfo;
	unsigned char timerbuf[8], taskbuf[8];

	unsigned int memtotal;
	struct MEMMAN *memman = (struct MEMMAN *) MEMMAN_ADDR;
	struct TASK *task_a;

	init_gdt();
	init_idt();

	io_sti();

	init_pit();
	
	init_keyboard(task_a);

	task_a = task_init(memman);

	timer = timer_alloc();
	timer_init(timer, &timerinfo, 1);
	timer_settime(timer, 10000);
	fifo8_init(&timerinfo, 8, timerbuf, task_a);

	memtotal = memtest(0x00400000, 0xbfffffff);
	memman_init(memman);
	memman_free(memman, 0x00400000, memtotal - 0x00400000);

	int i = 0;
	while (1) {
		if (fifo8_status(&timerinfo) != 0)
		{
			printk("%d times up, the CPU has passed 10000 ticks\n", ++i);
			timer_settime(timer, 10000);
			fifo8_get(&timerinfo);
		}
		io_hlt();

		if (i == 50)
		{
			clear_screen();
			printk("timer stoped\n");
			// timer_free(timer);
			// break;
		}
	}

	io_hlt();
}