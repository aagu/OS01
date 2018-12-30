#include "mouse.h"
#include "video.h"

static MS_INPUT ms_in;
static MOUSE_DEC mdec;

int mouse_bytes [3];
int mouse_cycle = 0;
int x = 0;
int y = 0;

/**
 * 鼠标中断注册
 */
 void init_mouse()
{
    mouse_wait(1);
    io_out8(0x64,0xA8);
    mouse_wait(1);
    io_out8(0x64,0x20);
    unsigned char status_byte;
    mouse_wait(0);
    status_byte =(io_in8(0x60) | 2);
    mouse_wait(1);
    io_out8(0x64,0x60);
    mouse_wait(1);
    io_out8(0x60,status_byte);
    //设置默认值
    mouse_write(0xF6);
    io_in8(0x60);
    //开始发送数据包
    mouse_write(0xF4);
    io_in8(0x60);
    ms_in.count = 0;
    ms_in.p_head = ms_in.p_tail = ms_in.buf;
    register_interrupt_handler(IRQ12, mouse_handler);
}

 /*
 *鼠标中断处理
 */
void mouse_handler(pt_regs *regs)
{
    int data = io_in8(0x60);
    if(ms_in.count < 128) {
        *(ms_in.p_head) = data;
        ms_in.p_head++;
        if(ms_in.p_head == ms_in.buf+128)
        {
            ms_in.p_head = ms_in.buf;
        }
    }
    ms_in.count++;
}

void mouse_wait(int a_type)
{
    unsigned int time_out = 100000;
    if(a_type == 0) {
        while(time_out--) {
            if((io_in8(0x60) & 1) == 1)
            {
                return;
            }
        }
        return;
    }
    else
    {
        while(time_out--) {
            if((io_in8(0x64) & 2) == 1)
            {
                return;
            }
        }
        return;
    }
}

void mouse_write(unsigned char a_write)
{
    //等待能够发送命令
    mouse_wait(1);
    //告诉鼠标我们正在发送命令
    io_out8(0x64,0xD4);
    //等待最后一部分
    mouse_wait(1);
    //最后写
    io_out8(0x60,a_write);
}

int mouse_read()
{
    char data;
    io_cli();
	if(ms_in.count > 0){
		data = *(ms_in.p_tail);
        ms_in.p_tail++;
		// 如果读到了最后
		if(ms_in.p_tail == ms_in.buf + 128){
			ms_in.p_tail = ms_in.buf;
		}
		ms_in.count = ms_in.count - 1;
        io_sti();
        return data;
	}
    io_sti();
    return -1;
}

int mouse_decode(struct MOUSE_DEC *mdec, unsigned char dat){
	if (mdec->phase == 0) {
		/* 等待鼠标的0xfa的阶段 */
		if (dat == 0xfa) {
			mdec->phase = 1;
		}        
		return 0;
	}
	if (mdec->phase == 1) {
		/* 等待鼠标第一字节的阶段 */
		mdec->buf[0] = dat;
		mdec->phase = 2;
		return 0;
	}
	if (mdec->phase == 2) {
		/* 等待鼠标第二字节的阶段 */
		mdec->buf[1] = dat;
		mdec->phase = 3;
		return 0;
	}
	if (mdec->phase == 3) {
		/* 等待鼠标第二字节的阶段 */
		mdec->buf[2] = dat;
		mdec->phase = 1;
		mdec->btn = mdec->buf[0] & 0x07;
		mdec->x = mdec->buf[1];
		mdec->y = mdec->buf[2];
		if ((mdec->buf[0] & 0x10) != 0) {
			mdec->x |= 0xffffff00;
		}
		if ((mdec->buf[0] & 0x20) != 0) {
			mdec->y |= 0xffffff00;
		}     
		/* 鼠标的y方向与画面符号相反 */   
		mdec->y = - mdec->y;
        if(mdec->x < 0) mdec->x = -2;
        else if(mdec->x == 0) mdec->x = 0;
        else mdec->x = 2;
        if(mdec->y < 0) mdec->y = -2;
        else if(mdec->y == 0) mdec->y = 0;
        else mdec->y = 2;
		return 1;
	}
	/* 应该不可能到这里来 */
	return -1; 
}