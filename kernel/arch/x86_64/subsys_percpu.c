// kernel/arch/x86_64/subsys_percpu.c

#include <kernel/subsys.h>
#include <kernel/apic.h>

static int _lapic_timer_start_percpu(int cpu_id)
{
    (void)cpu_id;
    lapic_timer_start(100);
    return 0;
}

void arch_register_subsys_percpu(void)
{
    register_subsys_percpu("lapic-timer-start", _lapic_timer_start_percpu, 0);
}
