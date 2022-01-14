#include <device/timer.h>
#include <kernel/softirq.h>
#include <kernel.h>
#include <kernel/printk.h>
#include <stdlib.h>

uint64_t volatile jiffies;
timer_t timer_list_head;

void init_timer(timer_t * timer, void (* func)(void * data), void * data, uint64_t expire_jiffies)
{
    list_init(&timer->list);
    timer->func = func;
    timer->data = data;
    timer->expire_jiffies = jiffies + expire_jiffies;
}

timer_t * create_timer(void (* func)(void * data), void * data, uint64_t expire_jiffies)
{
    timer_t * timer = (timer_t *)calloc(sizeof(timer_t));
    init_timer(timer, func, data, expire_jiffies);
    return timer;
}

void do_timer(void * data __attribute__((unused)))
{
    timer_t * timer = container_of(list_next(&timer_list_head.list), timer_t, list);
    while ((!list_is_empty(&timer_list_head.list)) && (timer->expire_jiffies <= jiffies))
    {
        del_timer(timer);
        timer->func(timer->data);
        timer = container_of(list_next(&timer_list_head.list), timer_t, list);
    }
    
    color_printk(RED, BLACK, "(Timer:%d)", jiffies);
}

void timer_init()
{
    jiffies = 0;
    init_timer(&timer_list_head, NULL, NULL, -1UL);
    register_softirq(0, &do_timer, NULL);
}

void add_timer(timer_t * timer)
{
    timer_t * tmp = container_of(list_next(&timer_list_head.list), timer_t, list);

    if (!list_is_empty(&timer_list_head.list))
    {
        while (tmp->expire_jiffies < timer->expire_jiffies)
            tmp = container_of(list_next(&tmp->list), timer_t, list);
    }
    list_add_to_behind(&tmp->list, &timer->list);
}

void del_timer(timer_t * timer)
{
    list_del(&timer->list);
}