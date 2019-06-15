#ifndef STDIO
#define STDIO

/*
 * Video Text color values
 */
#define WHITE_TXT 0x07
#define BLUE_TXT 0x09
#define RED_TXT 0x04

#define VGA_WIDTH 80

void clear_screen(); //clears the screen, duh
//void print(char *msg, unsigned int line);//prints a message to the screen at line specified
void print(unsigned char *string, unsigned char color_b, unsigned char color_c);
char *itoa(int number);
int get_cursor();
void set_cursor(int x, int y);
#endif // stdio.h
