#include "timer.h"
#include "kernel.h"

struct TIMERCTL timerctl;

void init_pit(void)
{
	io_out8(PIT_CTRL, 0x34);
	io_out8(PIT_CNT0, 0x9c);
	io_out8(PIT_CNT0, 0x2e);
    timerctl.count = 0;
	timerctl.timeout = 0;
    register_interrupt_handler(IRQ0, timer_handler);
}

void timer_handler(pt_regs *regs)
{
	io_out8(0x0020, 0x60); /* 把IRQ-00信号接收完了的信息通知给PIC */
    timerctl.count++;
	if (timerctl.timeout > 0)
	{
		timerctl.timeout--;
		if (timerctl.timeout == 0)
		{
			fifo8_put(timerctl.fifo, timerctl.data);
		}
	}
	return;
}

void settimer(unsigned int timeout, struct FIFO8 *fifo, unsigned char data)
{
	int eflags;
	eflags = io_load_eflags();
	io_cli();
	timerctl.timeout = timeout;
	timerctl.fifo = fifo;
	timerctl.data = data;
	io_store_eflags(eflags);
	return;
}