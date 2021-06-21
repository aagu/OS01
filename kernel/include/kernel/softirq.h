#ifndef _KERNEL_SOFTIRQ_H
#define _KERNEL_SOFTIRQ_H

#include <stdint.h>

#define TIMER_IRQ (1<<0)

extern uint64_t softirq_status;

typedef struct softirq
{
    void (*action)(void* data);
    void* data;
}softirq_t;

extern softirq_t softirq_vector[64];

void register_softirq(int32_t nr, void (*action)(void* data), void* data);
void unregister_softirq(int32_t nr);

void set_softirq_status(uint64_t status);
uint64_t get_softirq_status();

void softirq_init();

#endif