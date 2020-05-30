#include "stdio.h"
#include "stdlib.h"
#include "kernel.h" 

static unsigned short *vidmem = (unsigned short *) 0xB8000;
int print_char(char c, int col, int row, char attr);
void set_cursor_offset(int offset);
int get_offset(int col, int row);
int get_offset_row(int offset);
int get_offset_col(int offset);

/*
 * Simply clears the screen to ' '
 */
void clear_screen()
{
	int i;
	for (i = 0; i < VGA_WIDTH*VGA_HEIGHT;)
   	{
	   vidmem[i++] = WHITE_TXT << 8 | ' ';
   	}
	set_cursor(0, 0);
}

/*
 * 显示一串字符
 * string 表示要显示的字符串
 * color_b 表示显示字符串的背景颜色
 * color_c 表示显示的字符的颜色
 */
void print(unsigned char *string, unsigned char color_b, unsigned char color_c)
{
	int cursor_x=0, cursor_y=0;
	unsigned char attribute = (color_b << 4)|(15 & color_c);
	unsigned short cursor_xy=0;

	cursor_xy = get_cursor();
	cursor_x = cursor_xy % VGA_WIDTH;
	cursor_y = cursor_xy / VGA_WIDTH;

	while(*string != '\0'){
		cursor_xy = print_char(*string, cursor_x, cursor_y, attribute);
		cursor_x = get_offset_col(cursor_xy);
		cursor_y = get_offset_row(cursor_xy);
		*string++;
	}
}

int get_cursor()
{
	int pos = 0;
    io_out8(0x3D4, 0x0F);
    pos |= io_in8(0x3D5);
    io_out8(0x3D4, 0x0E);
    pos |= ((int)io_in8(0x3D5)) << 8;
    return pos;
}

void set_cursor(int x, int y)
{
	int pos = y * VGA_WIDTH + x;
 
	set_cursor_offset(pos);
}

void set_cursor_offset(int offset) {
	io_out8(0x3D4, 0x0F);
	io_out8(0x3D5, (short) (offset & 0xFF));
	io_out8(0x3D4, 0x0E);
	io_out8(0x3D5, (short) ((offset >> 8) & 0xFF));
}

int print_char(char c, int col /* x */, int row /* y */, char attr) {
	if (!attr) attr = WHITE_ON_BLACK;
	if (col >= VGA_WIDTH || row >= VGA_HEIGHT) {
		vidmem[(VGA_WIDTH)*(VGA_HEIGHT)-2] = 'E';
		vidmem[(VGA_WIDTH)*(VGA_HEIGHT)-1] = RED_ON_WHITE;
		return get_offset(col, row);
	}

	int offset;
	if (col >= 0 && row >= 0) offset = get_offset(col, row);
	else offset = get_cursor();
	
	if (c == '\n') {
		// ‘\n’ 不显示
		row = get_offset_row(offset);
		offset = get_offset(0, row + 1);
	} else {
		vidmem[offset] = c | (attr << 8);
		offset++;
	}

	/* 偏移超出屏幕尺寸，滚动画面 */
	if (offset >= VGA_WIDTH * VGA_HEIGHT) {
		int i;
		memcpy(vidmem + VGA_WIDTH, vidmem, VGA_WIDTH * (VGA_HEIGHT - 1) * 2/* 2 byte each cell */);
		unsigned short* last_line = get_offset(0, VGA_HEIGHT - 1) + vidmem;
		for (i = 0; i < VGA_WIDTH; i++) last_line[i] = 0;

		offset -= VGA_WIDTH;
	}

	set_cursor_offset(offset);
	return offset;
}

int get_offset(int col, int row) { return (row * VGA_WIDTH + col); }
int get_offset_row(int offset) { return offset / (VGA_WIDTH); }
int get_offset_col(int offset) { return (offset - (get_offset_row(offset)*VGA_WIDTH)); }