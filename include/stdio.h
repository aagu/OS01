#ifndef STDIO
#define STDIO

/*
 * Video Text color values
 */
#define WHITE_TXT 0x07
#define BLUE_TXT 0x09
#define RED_TXT 0x04

void clear_screen(); //clears the screen, duh
void print(char *msg, unsigned int line);//prints a message to the screen at line specified
char *itoa(int number);
#endif // stdio.h
