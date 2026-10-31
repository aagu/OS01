#include <sched/task.h>
#include <percpu/percpu.h>
#include <arch/irq.h>
#include <arch/cpu.h>

void arch_aarch64_idle_step(void)
{
    percpu_t *cpu = this_cpu();
    arch_irq_state_t flags = arch_local_irq_save();
    if (cpu->need_resched || !rbtree_empty(&cpu->run_queue)) {
        schedule();
        arch_local_irq_restore(flags);
        return;
    }

    /* The periodic BSP CNTP tick bounds the check-to-WFI race.  IRQ
     * is enabled before sleeping, then the caller rechecks after wake. */
    arch_local_irq_enable();
    __asm__ __volatile__("wfi" ::: "memory");
}

void idle_resume(void)
{
    for (;;) {
        arch_aarch64_idle_step();
    }
}
