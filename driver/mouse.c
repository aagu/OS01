#include "mouse.h"
#include "video.h"
#include "sheet.h"

static MS_INPUT ms_in;
static MOUSE_DEC mdec;

int mouse_bytes [3];
int mouse_cycle = 0;
int mouse_x = 0;
int mouse_y = 0;
SHEET *sht_mouse;

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
    static int mouse_cycle = 0;
    static char mouse_byte[3];
    struct BOOTINFO *binfo = (struct BOOTINFO*) 0x0ff0;
    switch(mouse_cycle) {
        case 0:
            mouse_byte[0] = io_in8(0x60);
            mouse_cycle++;
            break;
        case 1:
            mouse_byte[1] = io_in8(0x60);
            mouse_cycle++;
            break;
        case 2:
            mouse_byte[2]= io_in8(0x60);
            mouse_x = mouse_x + (mouse_byte[1]);
            mouse_y = mouse_y - (mouse_byte[2]);

            // Adjust mouse position
            if(mouse_x < 0)
                mouse_x = 0;
            if(mouse_y < 0)
                mouse_y = 0;
            if(mouse_x > binfo->scrnx - 1)
                mouse_x = binfo->scrnx - 16;
            if(mouse_y > binfo->scrny - 1)
                mouse_y = binfo->scrny - 16;

            sheet_slide(sht_mouse, mouse_x, mouse_y); /* 包含sheet_refresh含sheet_refresh */

            mouse_cycle = 0;
            break;
    }
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

void setscrnbuf(SHEET *mouse_buf)
{
    sht_mouse = mouse_buf;
}