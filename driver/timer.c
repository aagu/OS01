#include "timer.h"
#include "kernel.h"

#define TIMER_FLAG_ALLOC 1
#define TIMER_FLAG_USING 2

struct TIMERCTL timerctl;

void init_pit(void)
{
	int i = 0;
	io_out8(PIT_CTRL, 0x34);
	io_out8(PIT_CNT0, 0x9c);
	io_out8(PIT_CNT0, 0x2e);
    timerctl.count = 0;
	for (i = 0; i < MAX_TIMER; i++)
	{
		timerctl.timer[i].flags = 0; /* 标记未使用 */
	}
	
    register_interrupt_handler(IRQ0, timer_handler);
}

void timer_handler(pt_regs *regs)
{
	io_out8(0x0020, 0x60); /* 把IRQ-00信号接收完了的信息通知给PIC */
    timerctl.count++;
	int i = 0;
	for (i = 0; i < MAX_TIMER; i++)
	{
		if (timerctl.timer[i].flags == TIMER_FLAG_USING)
		{
			timerctl.timer[i].timeout--;
			if (timerctl.timer[i].timeout == 0)
			{
				timerctl.timer[i].flags = TIMER_FLAG_ALLOC;
				fifo8_put(timerctl.timer[i].fifo, timerctl.timer[i].data);
			}
		}
	}
	return;
}

struct TIMER *timer_alloc(void)
{
	int i = 0;
	for (i = 0; i < MAX_TIMER; i++)
	{
		/* 初始化并返回第一个未用的计时器 */
		if (timerctl.timer[i].flags == 0)
		{
			timerctl.timer[i].flags = TIMER_FLAG_ALLOC;
			return &timerctl.timer[i];
		}
	}
}

void timer_init(struct TIMER *timer, struct FIFO8 *fifo, unsigned char data)
{
	timer->fifo = fifo;
	timer->data = data;
	return;
}

void timer_settime(struct TIMER* timer, unsigned int timeout)
{
	timer->timeout = timeout;
	timer->flags = TIMER_FLAG_USING;
	return;
}

void timer_free(struct TIMER *timer)
{
	timer->flags = 0; /* 标记未使用 */
	return;
}