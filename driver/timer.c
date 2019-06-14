#include "timer.h"

struct TIMERCTL timerctl;

void init_pit(void)
{
	io_out8(PIT_CTRL, 0x43);
	io_out8(PIT_CNT0, 0x9c);
	io_out8(PIT_CNT0, 0x2e);
    timerctl.count = 0;
    register_interrupt_handler(IRQ0, timer_handler);
}

void timer_handler(pt_regs *regs)
{
	io_out8(0x0020, 0x60); /* 把IRQ-00信号接收完了的信息通知给PIC */
    timerctl.count++;
	return;
}