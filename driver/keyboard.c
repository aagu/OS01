/*
 * 初始化键盘相关函数
 */

#include "keyboard.h"
#include "interrupt.h"
#include "video.h"
#include "kernel.h"

/*
 * 说明：注册键盘中断处理函数
 */
//static KB_INPUT kb_in;
static struct FIFO8 key_fifo;
static char buf[8];
static short key_down = 0; //按下键盘
static struct TASK *key_task;

void init_keyboard(struct TASK *task)
{
	wait_keyboard();
	io_out8(0x64, 0x60);
	wait_keyboard();
	io_out8(0x60, 0x47);
	fifo8_init(&key_fifo, 8, buf, 0);
	key_task = task;
	register_interrupt_handler(IRQ1,keyboard_handler);
}

/*
 * 说明：键盘中断处理函数
 */
void keyboard_handler(pt_regs *regs)
{
	unsigned char scancode;
	scancode = io_in8(0x60);
	if (key_down == 0) //按下键盘
	{
		fifo8_put(&key_fifo, scancode);
		key_down = 1;
	} else //抬起键盘
	{
		key_down = 0;
	}
}

int keyboard_read(void)
{
	int scancode = -1;
    io_cli();
	if (fifo8_status(&key_fifo) != 0)
	{
		scancode = fifo8_get(&key_fifo);
	} else
	{
		task_sleep(key_task);
	}
	io_sti();
	return scancode;
}

void wait_keyboard(void) {
	/* 等待键盘控制电路准备完毕 */
	for (;;) {
		if ((io_in8(0x64) & 0x02) == 0) {
			break;
		}
	}
	return;
}