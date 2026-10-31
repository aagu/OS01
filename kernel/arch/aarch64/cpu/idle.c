#include <sched/task.h>
#include <percpu/percpu.h>
#include <arch/irq.h>
#include <arch/cpu.h>

void idle_resume(void)
{
    percpu_t *cpu = this_cpu();
    for (;;) {
        arch_irq_state_t flags = arch_local_irq_save();
        if (cpu->need_resched || !rbtree_empty(&cpu->run_queue)) {
            schedule();
            arch_local_irq_restore(flags);
            continue;
        }

        /* The periodic BSP CNTP tick bounds the check-to-WFI race.  IRQ
         * is enabled before sleeping, then the loop rechecks the queue
         * and need_resched immediately after every wake. */
        arch_local_irq_enable();
        __asm__ __volatile__("wfi" ::: "memory");
    }
}
