#include <intr/softirq.h>
#include <stddef.h>
#include <string.h>

uint64_t softirq_status;

softirq_t softirq_vector[64] = {0};

void set_softirq_status(uint64_t status)
{
#if defined(__x86_64__)
    /* spec §2.3 facade: arch_atomic_or_u64. x86_64 strong override
     * lives in kernel/arch/x86_64/atomic.c (lock orq); kept as inline
     * here because the function-call overhead in the tick-handler
     * hot path showed up as a CI-side kernel-selftest flake. */
    __asm__ __volatile__("lock orq %0, softirq_status(%%rip)"
                         :: "r"(status) : "memory");
#elif defined(__aarch64__)
    /* spec §2.3 facade: arch_atomic_or_u64. aarch64 strong override
     * lives in kernel/arch/aarch64/atomic.c (ldaxr+stlxr LR/SC retry).
     * Plain write here matches the pre-AAGU-4.5 aarch64 path (single
     * writer in tick_handler; reader fully serialised from IRQ context);
     * CI-side x86_64 flake drove the inline-asm restoration. */
    softirq_status |= status;
#else
#error "Unsupported architecture"
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
#elif defined(__aarch64__)
			softirq_status &= ~(1ULL << i);
#else
#error "Unsupported architecture"
#endif
		}
	}
}

void softirq_init()
{
	softirq_status = 0;
	memset(softirq_vector,0,sizeof(struct softirq) * 64);
}