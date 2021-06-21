#include <kernel/softirq.h>
#include <kernel/arch/x86_64/asm.h>
#include <stddef.h>
#include <string.h>

uint64_t softirq_status;
softirq_t softirq_vector[64] = {0};

void set_softirq_status(uint64_t status)
{
    softirq_status |= status;
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
	sti();
	for(i = 0;i < 64 && softirq_status;i++)
	{
		if(softirq_status & (1 << i))
		{
			softirq_vector[i].action(softirq_vector[i].data);
			softirq_status &= ~(1 << i);
		}
	}
	cli();
}

void softirq_init()
{
	softirq_status = 0;
	memset(softirq_vector,0,sizeof(struct softirq) * 64);
}