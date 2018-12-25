#ifndef MOUSE_H
#define MOUSE_H

#include "interrupt.h"

void mouse_handler(pt_regs *regs);
void init_mouse();
void mouse_read();
void mouse_wait(int a_type);
void mouse_write(unsigned char a_write);
#endif