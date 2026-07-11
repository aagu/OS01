#include <kernel/percpu.h>
#include <kernel/arch/cpu.h>
#include <string.h>

percpu_t percpu_data[NR_CPUS];
uint32_t num_cpus;

void percpu_install_gs(uint32_t cpu)
{
    arch_set_percpu_base(&percpu_data[cpu]);
}

void percpu_init(uint32_t cpu, uint32_t apic_id)
{
    memset(&percpu_data[cpu], 0, sizeof(percpu_t));
    percpu_data[cpu].cpu_id  = cpu;
    percpu_data[cpu].arch_processor_id = apic_id;
    // Store self-pointer as the first qword so GS:0 yields &percpu_data[cpu]
    percpu_data[cpu].self = (uint64_t)&percpu_data[cpu];
    list_init(&percpu_data[cpu].run_queue);
}
