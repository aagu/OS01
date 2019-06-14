#include "interrupt.h"
#include "kernel.h"

#define PIT_CTRL 0x0043
#define PIT_CNT0 0x0040

void init_pit(void);
void timer_handler(pt_regs *regs);