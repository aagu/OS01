#include <sched/task.h>
#include <percpu/percpu.h>
#include <arch/irq.h>
#include <arch/cpu.h>
#include <arch/spinlock.h>
#include <arch/aarch64/ap_work.h>

/* P1 polling idle (spec §4): every aarch64 scheduler CPU — BSP and,
 * from the P1 handoff on, accepted APs — runs THIS loop.  Each
 * iteration consumes at most one ap_work item with IRQs enabled, then
 * checks need_resched / the runqueue under the rq_lock; with work it
 * schedules (IRQ state restored around it), without work it pauses.
 * There is deliberately NO WFI: no AP has a periodic timer in P1, so a
 * missed notification could never be recovered on an AP — the busy
 * poll is the notification mechanism until P2 adds the resched SGI. */
void arch_aarch64_idle_step(void)
{
    percpu_t *cpu = this_cpu();

    ap_work_run_one(cpu_id());

    arch_irq_state_t flags = arch_local_irq_save();
    uint64_t rq_flags = spin_lock_irqsave(&cpu->rq_lock);
    bool work = __atomic_load_n(&cpu->need_resched, __ATOMIC_ACQUIRE) ||
                !rbtree_empty(&cpu->run_queue);
    spin_unlock_irqrestore(&cpu->rq_lock, rq_flags);
    if (work) {
        schedule();
        arch_local_irq_restore(flags);
        return;
    }
    arch_local_irq_restore(flags);
    arch_cpu_pause();
}

void idle_resume(void)
{
    for (;;) {
        arch_aarch64_idle_step();
    }
}
