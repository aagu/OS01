#ifndef _KERNEL_TIMER_H
#define _KERNEL_TIMER_H

#include <stdint.h>
#include <list.h>

extern uint64_t volatile jiffies = 0;

typedef struct timer
{
    list_t list;
    uint64_t expire_jiffies;
    void (* func)(void * data);
    void * data;
} timer_t;

extern timer_t timer_list_head;

timer_t * create_timer(void (* func)(void * data), void * data, uint64_t expire_jiffies);
void timer_init();
void add_timer(timer_t * timer);
void del_timer(timer_t * timer);

#endif