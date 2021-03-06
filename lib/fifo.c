# include "kernel.h"
#include "task.h"

#define FLAGS_OVERRUN 0x0001

/* 初始化FIFO缓冲区 */
void fifo8_init(struct FIFO8 *fifo, int size, unsigned char *buf, struct TASK *task)
{
    fifo->size = size;
    fifo->buf = buf;
    fifo->free = size; /* 缓冲区大小 */
    fifo->flags = 0;
    fifo->p = 0; /* 下一个数据写入位置 */
    fifo->q = 0; /* 下一个数据读取位置 */
	fifo->task = task;
    return;
}

/* 向FIFO传送数据并保存 */
int fifo8_put(struct FIFO8 *fifo, unsigned char data)
{
    if (fifo->free == 0) {
		/* 没有空间，溢出 */
		fifo->flags |= FLAGS_OVERRUN;
		return -1;
	}
	fifo->buf[fifo->p] = data;
	fifo->p++;
	if (fifo->p == fifo->size) {
		fifo->p = 0;
	}
	fifo->free--;
	if (fifo->task != 0) {
		if (fifo->task->flags != TASK_RUNNING) { /*如果任务处于休眠状态*/
			task_run(fifo->task, 0); /*将任务唤醒*/
		}
	}
	return 0;
}

/* 从FIFO取得一个数据 */
int fifo8_get(struct FIFO8 *fifo)
{
    int data;
	if (fifo->free == fifo->size) {
		/* 如果缓冲区为空则返回-1 */
		return -1;
	}
	data = fifo->buf[fifo->q];
	fifo->q++;
	if (fifo->q == fifo->size) {
		fifo->q = 0;
	}
	fifo->free++;
	return data;
}

/* 报告一下积攒是数据量 */
int fifo8_status(struct FIFO8 *fifo)
{
    return fifo->size - fifo->free;
}