#include "descriptor.h"
#include "interrupt.h"
#include "video.h"
#include "printk.h"
#include "keymap.h"
#include "timer.h"
#include "task.h"

#define MEMMAN_ADDR 0x003c0000

void task_b_main(void);

void main(void)
{
    struct BOOTINFO *binfo = (struct BOOTINFO*) 0x0ff0;
	char s[40];

	struct TIMER *timer;
	struct FIFO8 timerinfo;
	unsigned char timerbuf[8];

	unsigned int memtotal;
	struct MEMMAN *memman = (struct MEMMAN *) MEMMAN_ADDR;

	int task_b_esp;
	struct TSS tss_a, tss_b;

	init_gdt();
	init_idt();

	io_sti();

	init_pit();
	init_keyboard();

	timer = timer_alloc();
	timer_init(timer, &timerinfo, 1);
	timer_settime(timer, 5000);
	fifo8_init(&timerinfo, 8, timerbuf);

	memtotal = memtest(0x00400000, 0xbfffffff);
	memman_init(memman);
	memman_free(memman, 0x00001000, 0x0009e000); /* 0x00001000 - 0x0009efff */
	memman_free(memman, 0x00400000, memtotal - 0x00400000);
	printk("memory %dMB free : %dKB\n", memtotal / (1024 * 1024), memman_total(memman) / 1024);
	struct TIMERCTL timerctl;
	tss_a.ldtr = 0;
	tss_a.iomap = 0x40000000;
	tss_b.ldtr = 0;
	tss_b.iomap = 0x40000000;
	set_tssldt2_gdt(5, &tss_a, TYPE_TSS);
	set_tssldt2_gdt(6, &tss_b, TYPE_TSS);
	printk("set gdt\n");
	load_tr(0 * 8);
	printk("load tr\n");
	task_b_esp = memman_alloc_4k(memman, 64 * 1024) + 64 * 1024;
	tss_b.eip = (int) &task_b_main;
	tss_b.eflags = 0x00000202; /* IF = 1; */
	tss_b.eax = 0;
	tss_b.ecx = 0;
	tss_b.edx = 0;
	tss_b.ebx = 0;
	tss_b.esp = task_b_esp;
	tss_b.ebp = 0;
	tss_b.esi = 0;
	tss_b.edi = 0;
	tss_b.es = 1 * 8;
	tss_b.cs = 2 * 8;
	tss_b.ss = 1 * 8;
	tss_b.ds = 1 * 8;
	tss_b.fs = 1 * 8;
	tss_b.gs = 1 * 8;
	while (1)
	{
		if (fifo8_status(&timerinfo) != 0)
		{
			timer_free(timer);
			//taskswitch6();
			printk("count done!\n");
			break;
		}
		//printk("couter %d", timerctl.count);
	}
	
	io_hlt();
}

void task_b_main(void)
{
	printk("task b\n");
	while (1)
	{
		io_hlt();
	}
}