#include <intr/softirq.h>
#include <arch/atomic.h>
#include <stddef.h>
#include <string.h>

uint64_t softirq_status;

softirq_t softirq_vector[64] = {0};

void set_softirq_status(uint64_t status)
{
    /* arch-neutral atomic bit-op facade (AAGU-4.5). x86_64: lock orq;
     * aarch64: ldaxr + stlxr LR/SC retry. Per-arch strong overrides live
     * in kernel/arch/<arch>/atomic.c. */
    arch_atomic_or_u64(&softirq_status, status);
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
			/* arch-neutral atomic bit clear (AAGU-4.5). */
			arch_atomic_and_u64(&softirq_status, ~(1ULL << i));
		}
	}
}

void softirq_init()
{
	softirq_status = 0;
	memset(softirq_vector,0,sizeof(struct softirq) * 64);
}