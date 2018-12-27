#include "video.h"
#include "kernel.h"

void init_palette(void)
{
	set_palette(0, 15, table_rgb);
	return;

	/* C语言中的static char语句只能用于数据，相当于汇编中的DB指令 */
}

void set_palette(int start, int end, unsigned char *rgb)
{
	int i, eflags;
	eflags = io_load_eflags();	/* 记录中断许可标志的值 */
	io_cli(); 					/* 将中断许可标志置为0,禁止中断 */
	io_out8(0x03c8, start);
	for (i = start; i <= end; i++) {
		io_out8(0x03c9, rgb[0] / 4);
		io_out8(0x03c9, rgb[1] / 4);
		io_out8(0x03c9, rgb[2] / 4);
		rgb += 3;
	}
	io_store_eflags(eflags);	/* 复原中断许可标志 */
	return;
}

void boxfill8(unsigned char *vram, int xsize, unsigned char c, int x0, int y0, int x1, int y1)
{
	int x, y;
	for (y = y0; y <= y1; y++) {
		for (x = x0; x <= x1; x++)
			vram[y * xsize + x] = c;
	}
	return;
}

void init_mouse_cursor(char* mouse, char bc) {
    int x, y;
    for (y = 0; y < 16; y++) {
        for (x = 0; x < 16; x++) {
           if (cursor[y][x] == '*') {
               mouse[y*16 + x] = COL8_000000;
           }
           if (cursor[y][x] == 'O') {
              mouse[y*16 + x] = COL8_FFFFFF;
           }
           if (cursor[y][x] == '.') {
               mouse[y*16 + x] = bc;
           }
        }
    }
}

void putblock(unsigned char* vram, int vxsize, int pxsize, int pysize, int px0, int py0, char* buf, int bxsize) {
    int x, y;
    for (y = 0; y < pysize; y++)
      for (x = 0; x < pxsize; x++) {
          vram[(py0+y) * vxsize + (px0+x)] = buf[y * bxsize + x];
      }
}

void showFont8(unsigned char *vram, int xsize, int x, int y, char c, char* font) {
    int i;
    char d;

    for (i = 0; i < 16; i++) {
        d = font[i]; 
        if ((d & 0x80) != 0) {vram[(y+i)*xsize + x + 0] = c;}
        if ((d & 0x40) != 0) {vram[(y+i)*xsize + x + 1] = c;}
        if ((d & 0x20) != 0) {vram[(y+i)*xsize + x + 2] = c;} 
        if ((d & 0x10) != 0) {vram[(y+i)*xsize + x + 3] = c;}
        if ((d & 0x08) != 0) {vram[(y+i)*xsize + x + 4] = c;}
        if ((d & 0x04) != 0) {vram[(y+i)*xsize + x + 5] = c;}
        if ((d & 0x02) != 0) {vram[(y+i)*xsize + x + 6] = c;}
        if ((d & 0x01) != 0) {vram[(y+i)*xsize + x + 7] = c;}
    }

}

void showString(unsigned char* vram, int xsize, int x, int y, char color, unsigned char *s ) {
    for (; *s != 0x00; s++) {
       showFont8(vram, xsize, x, y,color, systemFont+ *s * 16);
       x += 8;
    }
}

void init_screen8(char *vram, int x, int y)
{
    boxfill8(vram, x, COL8_848484,  0,         0,         x - 1, y - 21);
	boxfill8(vram, x, COL8_C6C6C6,  0,         y - 20,    x - 1, y - 19);
	boxfill8(vram, x, COL8_FFFFFF,  0,         y - 19,    x - 1, y - 18);
	boxfill8(vram, x, COL8_C6C6C6,  0,         y - 18,    x - 1, y -  1);

	return;
}
