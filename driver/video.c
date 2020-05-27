#include "video.h"
#include "kernel.h"
#include "font.h"

const char cursor[16][16] = {
        "**************..",
        "*OOOOOOOOOOO*...",
        "*OOOOOOOOOO*....",
        "*OOOOOOOOO*.....",
        "*OOOOOOOO*......",
        "*OOOOOOO*.......",
        "*OOOOOOO*.......",
        "*OOOOOOOO*......",
        "*OOOO**OOO*.....",
        "*OOO*..*OOO*....",
        "*OO*....*OOO*...",
        "*O*......*OOO*..",
        "**........*OOO*.",
        "*..........*OOO*",
        "............*OO*",
        ".............***"
};

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

/**
 * *vram 显存地址
 * xsize 屏幕宽度
 * c 背景色
 * x0, y0 方块左上角坐标
 * x1, y1 方块右下角坐标
*/
void boxfill8(unsigned char *vram, int xsize, unsigned char c, int x0, int y0, int x1, int y1)
{
	int x, y;
	for (y = y0; y <= y1; y++) {
		for (x = x0; x <= x1; x++)
			vram[y * xsize + x] = c;
	}
	return;
}

void init_mouse_cursor(unsigned char* mouse, char bc) {
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

void init_screen8(unsigned char *vram, int x, int y)
{
    boxfill8(vram, x, COL8_848484,  0,         0,         x - 1, y - 21);
	boxfill8(vram, x, COL8_C6C6C6,  0,         y - 20,    x - 1, y - 19);
	boxfill8(vram, x, COL8_FFFFFF,  0,         y - 19,    x - 1, y - 18);
	boxfill8(vram, x, COL8_C6C6C6,  0,         y - 18,    x - 1, y -  1);

	return;
}

void putfonts8_asc_sht(struct SHEET *sht, int x, int y, int c, int b, char *s, int l)
{
	boxfill8(sht->buf, sht->bxsize, b, x, y, x + l * 8 - 1, y + 15);
	showString(sht->buf, sht->bxsize, x, y, c, s);
	sheet_refresh(sht, x, y, x + l * 8, y + 16);
	return;
}
