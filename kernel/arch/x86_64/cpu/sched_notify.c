/* x86_64 scheduling-policy backends (<arch/sched.h>, P1 spec §4).
 *
 * The x86 contract is unchanged by the AP cooperative scheduler work:
 * the default mask keeps its num_cpus reach, remote notification stays
 * the LAPIC resched IPI addressed by arch_processor_id, and every CPU
 * may scan the blocker list.
 */
#include <arch/sched.h>
#include <percpu/percpu.h>
#include <intr/ipi.h>

uint64_t arch_kthread_default_mask(void)
{
    return (num_cpus >= NR_CPUS) ? ~0ULL : ((1ULL << num_cpus) - 1);
}

void arch_sched_notify_remote(unsigned cpu)
{
    /* The logical CPU was validated by sched_notify_remote_cpu();
     * the hardware wants the APIC ID, not the logical index. */
    ipi_send(percpu_data[cpu].arch_processor_id, IPI_VECTOR_RESCHED);
}

bool arch_sched_scan_blockers(void)
{
    return true;
}
