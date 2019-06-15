#include "stdio.h"
#include "kernel.h"

static unsigned short *vidmem = (unsigned short *)0xB8000;

/*
 * Simply clears the screen to ' '
 */
void clear_screen()
{
	unsigned int i = 0;

	while(i < (80*25*2))
	{
    vidmem[i] = ' ';
    i++;
	vidmem[i] = 0x07;
	i++;
	}
}

/*
 * Print only supports a-z and 0-9 and other keyboard characters
 */
//void print(char *msg, unsigned int color)
//{
//    //unsigned int i = cur_line*80*2, color = WHITE_TXT;
//	int pos, pos_x, pos_y;
//	pos = get_cursor();
//	pos_x = pos % VGA_WIDTH;
//	pos_y = pos / VGA_WIDTH;
//
//	while(*msg != 0)
//	{ 		  	
//		while (*msg == '\n')
//		{
//			pos_y++;
//			/* 跳过换行符 */
//			pos_x = 0;
//			*msg++;
//		}
//		if (*msg != '\0')
//		{
//			vidmem[pos_y * VGA_WIDTH + pos_x] = *msg;
//			pos_x++;
//			vidmem[pos_y * VGA_WIDTH + pos_x] = color;
//			pos_x++;
//			if (pos_x >= VGA_WIDTH)
//			{
//				pos_y++;
//				pos_x = 0;
//			}
//			*msg++;
//		}
//	}
//	set_cursor(pos_x, pos_y);
//}

/*
 * 显示一串字符
 * string 表示要显示的字符串
 * color_b 表示显示字符串的背景颜色
 * color_c 表示显示的字符的颜色
 */
void print(unsigned char *string, unsigned char color_b, unsigned char color_c)
{
	int cursor_x=0,cursor_y=0;
	unsigned char attribute = (color_b << 4)|(15 & color_c);
	unsigned short cursor_xy=0;

	cursor_xy = get_cursor();
	cursor_x = cursor_xy % VGA_WIDTH;
	cursor_y = cursor_xy / VGA_WIDTH;
	while(*string != '\0'){
		//检查是否有换行符
		while(*string == '\n')
		{
			cursor_y++;   // 纵坐标+1
			cursor_x = 0; // 光标移到最开始
			*string++;	      // 不显示换行符
		}

		// 字符串最后的\0不显示
		if(*string != '\0')
		{
			vidmem[cursor_y*80+cursor_x] = *string | (attribute << 8);
			cursor_x++;
			*string++;

			// 到一行显示完，开始从下一行开始显示
			if(cursor_x + 1 >= VGA_WIDTH)
				cursor_y++;
		}
		set_cursor(cursor_x,cursor_y);
	}
}

char *itoa(int number)
{
	char str[10];
	int i = 0;
	while(number > 10)
	{
		str[i] = number%10+'0';
		number = number/10;
		i++;
	}
	str[i] = number+'0';
	str[i+1] = '\0';
	return str;
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
 
	io_out8(0x3D4, 0x0F);
	io_out8(0x3D5, (short) (pos & 0xFF));
	io_out8(0x3D4, 0x0E);
	io_out8(0x3D5, (short) ((pos >> 8) & 0xFF));
}