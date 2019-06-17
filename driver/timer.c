#include "timer.h"
#include "kernel.h"
#include "task.h"

#define TIMER_FLAG_ALLOC 1
#define TIMER_FLAG_USING 2

struct TIMERCTL timerctl;
extern struct TIMER *task_timer;

void init_pit(void)
{
	int i;
	struct TIMER *t;
	io_out8(PIT_CTRL, 0x34);
	io_out8(PIT_CNT0, 0x9c);
	io_out8(PIT_CNT0, 0x2e); /* 0x2e9c =100Hz */
    timerctl.count = 0;
	for (i = 0; i < MAX_TIMER; i++)
	{
		timerctl.timer[i].flags = 0; /* 标记未使用 */
	}
    register_interrupt_handler(IRQ0, timer_handler);
	t = timer_alloc();
	t->timeout = 0xffffffff;
	t->flags = TIMER_FLAG_USING;
	t->next = 0;
	timerctl.t0 = t;
	timerctl.next = 0xffffffff;
	return;
}

void timer_handler(pt_regs *regs)
{
	io_out8(0x0020, 0x60); /* 把IRQ-00信号接收完了的信息通知给PIC */
    timerctl.count++;
	char ts = 0;
	if (timerctl.next > timerctl.count)
	{
		return; /* 还不到下一个时刻，所以结束*/
	}
	timerctl.next = 0xffffffff;
	struct TIMER *timer = timerctl.t0;
	while (timer->timeout <= timerctl.count)
	{
		timer->flags = TIMER_FLAG_ALLOC;
		if (timer == task_timer)
		{
			ts = 1;
		} else
		{
			fifo8_put(timer->fifo, timer->data);
		}
		timer = timer->next;
	}
	timerctl.t0 = timer;
	timerctl.next = timerctl.t0->timeout;
	if (ts != 0)
	{
		task_switch();
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
	int eflags;
	timer->timeout = timeout + timerctl.count;
	timer->flags = TIMER_FLAG_USING;
	eflags = io_load_eflags();
	io_cli();
	struct TIMER *head, *t;
	t = timerctl.t0;
	if (timer->timeout <= t->timeout)
	{
		/* 插到头部*/
		timerctl.t0 = timer;
		timer->next = t;
		timerctl.next = timer->timeout;
		io_store_eflags(eflags);
		return;
	}
	while (t->next != 0)
	{
		head = t;
		t = t->next;
		if (timer->timeout <= t->timeout)
		{
			head->next = timer;
			timer->next = t;
			io_store_eflags(eflags);
			return;
		}
	}
}

void timer_free(struct TIMER *timer)
{
	timer->flags = 0; /* 标记未使用 */
	return;
}