#ifndef MOUSE_H
#define MOUSE_H

#include "interrupt.h"

typedef struct s_ms {
	char*	p_head;			/* 指向缓冲区中下一个空闲位置 */
	char*	p_tail;			/* 指向键盘任务应处理的字节 */
	int	count;			/* 缓冲区中共有多少字节 */
	char	buf[128];	/* 缓冲区 */
}MS_INPUT;

typedef struct MOUSE_DEC {
    unsigned char buf[3], phase;
    int x, y, btn;
}MOUSE_DEC;

void mouse_handler(pt_regs *regs);
void init_mouse();
int mouse_read();
void mouse_wait(int a_type);
void mouse_write(unsigned char a_write);
int mouse_decode(struct MOUSE_DEC *mdec, unsigned char dat);

#endif