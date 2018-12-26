#include <descriptor.h>
#include <interrupt.h>
#include <video.h>
#include <mouse.h>

unsigned char *vram;/* 声明变量vram、用于BYTE [...]地址 */
static int mx = 0, my = 0;
static char mcursor[256];


static MOUSE_DEC mdec;

void main(void)
{
	clear_screen();
	int xsize, ysize;
	vram = (unsigned char *) 0xa0000;/* 地址变量赋值 */
	xsize = 1024;
	ysize = 768;

	init_gdt();
	init_idt();

	init_palette();/* 设定调色板 */

	init_keyboard();
	/* 根据 0xa0000 + x + y * 320 计算坐标 8*/
	boxfill8(vram, xsize, COL8_848484,  0,         0,          xsize -  1, ysize - 21);
	boxfill8(vram, xsize, COL8_C6C6C6,  0,         ysize - 20, xsize -  1, ysize - 19);
	boxfill8(vram, xsize, COL8_FFFFFF,  0,         ysize - 19, xsize -  1, ysize - 18);
	boxfill8(vram, xsize, COL8_C6C6C6,  0,         ysize - 18, xsize -  1, ysize -  1);

	//mx = (xsize - 16) / 2;
    //my = (ysize - 28 - 16) / 2;  
	putblock(vram, xsize, 16, 16, 80, 80, mcursor, 16);
	
	init_mouse();
	//asm volatile("int $44");
	for (;;) {
		//io_hlt();
		keyboard_read();
		//show_mouse_info();
	}
}

void computeMousePosition(MOUSE_DEC* mdec) {
    mx += mdec->x;
    my += mdec->y;
    if (mx < 0) {
       mx = 0;
    }

    if (my < 0) {
       my = 0;
    }

    if (mx > 320- 16) {
       mx = 320 - 16;
    }
    if (my > 320 - 16) {
       my = 320 - 16;
    }

}

void eraseMouse(char* vram) {
    boxfill8(vram, 320, COL8_008484, mx, my, mx+15, my+15);
}

void drawMouse(char* vram) {
    putblock(vram, 320, 16, 16, mx, my, mcursor, 16);
}

int mouse_decode(struct MOUSE_DEC *mdec, unsigned char dat) {
    if (mdec->phase == 0) {
        if (dat == 0xfa) {
           mdec->phase = 1;
        }

       return 0;
    }

    if (mdec->phase == 1) {
        if ((dat & 0xc8) == 0x08) {
            mdec->buf[0] = dat;
            mdec->phase = 2;
        }

        return 0;
    }

    if (mdec->phase == 2) {
        mdec->buf[1] = dat;
        mdec->phase = 3;
        return 0;
    }

    if (mdec->phase == 3) {
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

        mdec->y = -mdec->y;
        return 1;
    }

    return -1;
}

void show_mouse_info(void) {
    char*vram = vram;
    unsigned char data = 0;

    data = mouse_read();
    if (mouse_decode(&mdec, data) != 0) {
         eraseMouse(vram);
         computeMousePosition(&mdec);
         drawMouse(vram);
    }
}