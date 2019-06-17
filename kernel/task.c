#include "task.h"
#include "timer.h"
#include "kernel.h"

struct TIMER *mt_timer;
int mt_tr;

void mt_init(void)
{
    mt_timer = timer_alloc();
    timer_settime(mt_timer, 200);
    mt_tr = 6 * 8;
    return;
}

void mt_taskswitch(void)
{
    if (mt_tr == 6 * 8)
    {
        mt_tr = 7 * 8;
    } else
    {
        mt_tr = 6 * 8;
    }
    timer_settime(mt_timer, 200);
    taskswitch(0, mt_tr);
    return;
}