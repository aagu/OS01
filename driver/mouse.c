#include "mouse.h"
#include "video.h"

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
    register_interrupt_handler(IRQ12, mouse_handler);
}

 /*
 *鼠标中断处理
 */
void mouse_handler(pt_regs *regs)
{
    unsigned char data;
    data = io_in8(0x60);
    boxfill8((unsigned char *) 0xa0000, 320, COL8_848484, 8, 8, 321, 40); //clean last char
    showString((unsigned char*) 0xa0000, 320, 8, 8, COL8_FFFFFF, &data);
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

void mouse_read()
{
    //从鼠标获取响应
    mouse_wait(0);
}
