#include "timer.h"
#include "kernel.h"

#define TIMER_FLAG_ALLOC 1
#define TIMER_FLAG_USING 2

struct TIMERCTL timerctl;

void init_pit(void)
{
	int i;
	io_out8(PIT_CTRL, 0x34);
	io_out8(PIT_CNT0, 0x9c);
	io_out8(PIT_CNT0, 0x2e); /* 0x2e9c =100Hz */
    timerctl.count = 0;
	timerctl.next = 0xffffffff;
	timerctl.using = 0;
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
	if (timerctl.next > timerctl.count)
	{
		return; /* 还不到下一个时刻，所以结束*/
	}
	timerctl.next = 0xffffffff;
	int i, j;
	for (i = 0; i < timerctl.using; i++)
	{
		if (timerctl.timerU[i]->timeout > timerctl.count)
		{
			break;
		}
		timerctl.timerU[i]->flags = TIMER_FLAG_ALLOC;
		fifo8_put(timerctl.timerU[i]->fifo, timerctl.timerU[i]->data);
	}
	timerctl.using -= i;
	for (j = 0; j < timerctl.using; j++)
	{
		/* 	前移 */
		timerctl.timerU[j] = timerctl.timerU[i + j];
	}
	if (timerctl.using > 0)
	{
		timerctl.next = timerctl.timerU[0]->timeout;
	} else
	{
		timerctl.next = 0xffffffff;
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
	return 0;
}

void timer_init(struct TIMER *timer, struct FIFO8 *fifo, unsigned char data)
{
	timer->fifo = fifo;
	timer->data = data;
	return;
}

void timer_settime(struct TIMER* timer, unsigned int timeout)
{
	int eflags, i, j;
	timer->timeout = timeout + timerctl.count;
	timer->flags = TIMER_FLAG_USING;
	eflags = io_load_eflags();
	io_cli();
	for (i = 0; i < MAX_TIMER; i++)
	{
		if (timerctl.timerU[i]->timeout >= timer->timeout)
		{
			break;
		}
	}
	/* 后移 */
	for (j = timerctl.using; j > i; j--)
	{
		timerctl.timerU[j] = timerctl.timerU[j - 1];
	}
	timerctl.using++;
	timerctl.timerU[i] = timer;
	timerctl.next = timerctl.timerU[0]->timeout;
	io_store_eflags(eflags);
	return;
}

void timer_free(struct TIMER *timer)
{
	timer->flags = 0; /* 标记未使用 */
	return;
}