/*
 * 初始化键盘相关函数
 */

#include <keyboard.h>
#include <interrupt.h>
#include <keymap.h>
#include "video.h"
#include "kernel.h"

/*
 * 说明：注册键盘中断处理函数
 */
static KB_INPUT kb_in;

void init_keyboard()
{
	kb_in.count = 0;
	kb_in.p_head = kb_in.p_tail = kb_in.buf;
	register_interrupt_handler(IRQ1,keyboard_handler);
}

/*
 * 说明：键盘中断处理函数
 */
void keyboard_handler(pt_regs *regs)
{
	unsigned char scancode;
	//printk("keyboard down\n");
	scancode = io_in8(0x60);
	if(kb_in.count < KB_IN_BYTES){
		*(kb_in.p_head) = scancode;
		kb_in.p_head++;
		// 如果满了，又指向开始
		if(kb_in.p_head == kb_in.buf+KB_IN_BYTES){
			kb_in.p_head = kb_in.buf;
		}

	}
	kb_in.count++;
	//printk("0x%02X,",scancode);
	interrupt_callback();
}

int keyboard_read()
{
	unsigned char scancode;
    io_cli();
	if(kb_in.count > 0){
		scancode = *(kb_in.p_tail);
		kb_in.p_tail++;
		kb_in.p_tail++;
		// 如果读到了最后
		if(kb_in.p_tail == kb_in.buf + KB_IN_BYTES){
			kb_in.p_tail = kb_in.buf;
		}
		kb_in.count = kb_in.count - 2;
	}
    io_sti();
	return scancode;
}