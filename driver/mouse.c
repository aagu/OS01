#include "mouse.h"
#include "video.h"

static MS_INPUT ms_in;
static MOUSE_DEC mdec;
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
    //mouse_read();
    //开始发送数据包
    mouse_write(0xF4);
    //mouse_read();
    ms_in.count = 0;
    ms_in.p_head = ms_in.p_tail = ms_in.buf;
    register_interrupt_handler(IRQ12, mouse_handler);
}

 /*
 *鼠标中断处理
 */
void mouse_handler(pt_regs *regs)
{
    unsigned char data;
    data = io_in8(0x60);
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
<<<<<<< HEAD
    //从鼠标获取响应
    mouse_wait(0);
}
=======
    unsigned char data;
    io_cli();
	if(ms_in.count > 0){
		data = *(ms_in.p_tail);
		ms_in.p_tail++;
		ms_in.p_tail++;
		// 如果读到了最后
		if(ms_in.p_tail == ms_in.buf + 128){
			ms_in.p_tail = ms_in.buf;
		}
		ms_in.count = ms_in.count - 2;
	}
    io_sti();
    return data;
}
>>>>>>> 提高分辨率
