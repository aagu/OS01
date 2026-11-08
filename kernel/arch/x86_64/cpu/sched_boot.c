/* kernel/arch/x86_64/cpu/sched_boot.c — x86_64 scheduler boot mechanisms.
 *
 * Implements arch_sched_boot_prepare() and arch_sched_boot_idle().
 * Public orchestration lives in kernel/init/userspace.c.
 */

#include <arch/boot.h>
#include <arch/cpu.h>
#include <arch/irq.h>
#include <arch/thread.h>
#include <percpu/percpu.h>
#include <sched/task.h>

void arch_sched_boot_prepare(void)
{
    arch_task_init_platform();
}

void arch_sched_boot_idle(void)
{
    while (1) {
        arch_cpu_halt();
        if (__atomic_load_n(&this_cpu()->need_resched, __ATOMIC_ACQUIRE)) {
            schedule();
            arch_local_irq_enable();
        }
    }
}
