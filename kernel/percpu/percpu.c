#include <kernel/percpu.h>
#include <kernel/arch/x86_64/msr.h>
#include <string.h>

// GS base MSR (defined in msr.h now, but repeated for clarity)
#ifndef IA32_GS_BASE
#define IA32_GS_BASE  0xC0000101
#endif

percpu_t percpu_data[NR_CPUS];
uint32_t num_cpus;

void percpu_init(uint32_t cpu, uint32_t apic_id)
{
    memset(&percpu_data[cpu], 0, sizeof(percpu_t));
    percpu_data[cpu].cpu_id  = cpu;
    percpu_data[cpu].apic_id = apic_id;
    // Store self-pointer as the first qword so GS:0 yields &percpu_data[cpu]
    percpu_data[cpu].self = (uint64_t)&percpu_data[cpu];
    list_init(&percpu_data[cpu].run_queue);
}

void percpu_install_gs(uint32_t cpu)
{
    wrmsr(IA32_GS_BASE, (uint64_t)&percpu_data[cpu]);
}
