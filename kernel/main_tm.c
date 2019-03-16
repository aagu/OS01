#include "descriptor.h"
#include "interrupt.h"
#include "video.h"
#include "printk.h"
#include "keymap.h"

#define MEMMAN_ADDR 0x003c0000

void main(void)
{
    struct BOOTINFO *binfo = (struct BOOTINFO*) 0x0ff0;
	char s[40];

	unsigned int memtotal;
	struct MEMMAN *memman = (struct MEMMAN *) MEMMAN_ADDR;

	init_gdt();
	init_idt();

	io_sti();

	init_keyboard();

	memtotal = memtest(0x00400000, 0xbfffffff);
	memman_init(memman);
	memman_free(memman, 0x00001000, 0x0009e000); /* 0x00001000 - 0x0009efff */
	memman_free(memman, 0x00400000, memtotal - 0x00400000);
	printk("memory %dMB free : %dKB", memtotal / (1024 * 1024), memman_total(memman) / 1024);
	io_hlt();
}