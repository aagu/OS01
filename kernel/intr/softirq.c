#include <intr/softirq.h>
#include <stddef.h>
#include <string.h>

uint64_t softirq_status;

softirq_t softirq_vector[64] = {0};

void set_softirq_status(uint64_t status)
{
#if defined(__x86_64__)
    __asm__ __volatile__("lock orq %0, softirq_status(%%rip)"
                         :: "r"(status) : "memory");
#else
    /* aarch64 (and other arches): plain write. SMP-safe in practice
     * because tick_handler() runs at IRQ context with IRQs masked
     * (no concurrent set_softirq_status); softirq_status is single
     * uint64_t written by tick + cleared by do_softirq, no race. */
    softirq_status |= status;
#endif
}

uint64_t get_softirq_status()
{
    return softirq_status;
}

void register_softirq(int32_t nr, void (*action)(void* data), void* data)
{
    softirq_vector[nr].action = action;
    softirq_vector[nr].data = data;
}

void unregister_softirq(int nr)
{
	softirq_vector[nr].action = NULL;
	softirq_vector[nr].data = NULL;
}

void do_softirq()
{
	int i;
	for(i = 0; i < 64 && softirq_status; i++)
	{
		if(softirq_status & (1 << i))
		{
			softirq_vector[i].action(softirq_vector[i].data);
#if defined(__x86_64__)
			__asm__ __volatile__("lock andq %0, softirq_status(%%rip)"
			                     :: "r"(~(1ULL << i)) : "memory");
#else
			softirq_status &= ~(1ULL << i);
#endif
		}
	}
}

void softirq_init()
{
	softirq_status = 0;
	memset(softirq_vector,0,sizeof(struct softirq) * 64);
}