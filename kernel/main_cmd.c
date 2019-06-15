#include "descriptor.h"
#include "interrupt.h"
#include "video.h"
#include "printk.h"
#include "keymap.h"
#include "timer.h"

#define MEMMAN_ADDR 0x003c0000

void main(void)
{
    struct BOOTINFO *binfo = (struct BOOTINFO*) 0x0ff0;
	char s[40];

	struct TIMER *timer;
	struct FIFO8 timerinfo;
	unsigned char timerbuf[8];

	unsigned int memtotal;
	struct MEMMAN *memman = (struct MEMMAN *) MEMMAN_ADDR;

	init_gdt();
	init_idt();

	io_sti();

	init_pit();
	init_keyboard();

	timer = timer_alloc();
	timer_init(timer, &timerinfo, 1);
	timer_settime(timer, 10000);
	fifo8_init(&timerinfo, 8, timerbuf);

	memtotal = memtest(0x00400000, 0xbfffffff);
	memman_init(memman);
	memman_free(memman, 0x00001000, 0x0009e000); /* 0x00001000 - 0x0009efff */
	memman_free(memman, 0x00400000, memtotal - 0x00400000);
	printk("memory %dMB free : %dKB\n", memtotal / (1024 * 1024), memman_total(memman) / 1024);
	extern struct TIMERCTL timerctl;
	while (1)
	{
		if (fifo8_status(&timerinfo) != 0)
		{
			timer_free(timer);
			printk("count done!\n");
			break;
		}
		//printk("couter %d", timerctl.count);
	}
	
	io_hlt();
}