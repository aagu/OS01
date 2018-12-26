#include "stdio.h"

/*
 * Simply clears the screen to ' '
 */
void clear_screen()
{
	char *vidmem = (char *)0xB8000;
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
void print(char *msg, unsigned int line)
{
	char *vidmem = (char *)0xB8000;
    unsigned int i = line*80*2, color = WHITE_TXT;

		
	
	while(*msg != 0)
	{ 		  	
		  vidmem[i] = *msg;
		  i++;
		  vidmem[i] = color;
		  i++;
		  *msg++;
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