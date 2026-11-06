#include <sched/task.h>
#include <percpu/percpu.h>
#include <arch/irq.h>
#include <arch/cpu.h>
#include <arch/spinlock.h>
#include <arch/aarch64/ap_work.h>
#include <arch/aarch64/task_arch.h>
#ifdef AARCH64_AP_SCHED_WAKE_TEST
#include <selftest/aarch64_ap_scheduler.h>
#endif

/* P2 polling+sleeping idle (spec §5.3).  Call contract: idle task
 * stack, IRQs ENABLED at entry, no lock held; returns with IRQs
 * ENABLED.  One iteration:
 *
 *   1. consume at most one ap_work item with IRQs on (the handler may
 *      touch the VMM and must not run masked or under the rq lock);
 *   2. mask IRQs; under the rq_lock check need_resched / the runqueue
 *      and acquire-load the ap_work slot state;
 *   3. with runqueue work or a pending resched: leave the lock, then
 *      schedule (IRQ state restored around it);
 *   4. with a READY slot: leave the lock, restore IRQs, back to the
 *      consumption path next iteration;
 *   5. with NO work: release the lock and KEEP DAIF.I=1, then
 *      dsb sy; wfi — a SGI arriving in that window stays pending and
 *      wakes the WFI (never enable IRQs before the WFI: the SGI could
 *      be consumed and the CPU would sleep with work pending).  After
 *      the return: enable IRQs + ISB and re-check from the top.
 *
 * The wait helper is aarch64_idle_wait_irq (idle_wait.S): exactly
 * dsb sy; wfi; ret, no DAIF change, audited by the harness.  The BSP
 * runs the same protocol; its CNTP tick only advances time / drives
 * the scan fallback, it is never a WFI safety net.  An ADMITTED AP
 * owns a PRIVATE CNTP interrupt for its local scheduler tick (per-CPU
 * accounting); it never advances the global clock.  An AP that was
 * never admitted to the scheduler
 * stays in its OLD work loop (secondary_idle) — this idle step only
 * runs on the idle task stack. */
void arch_aarch64_idle_step(void)
{
    percpu_t *cpu = this_cpu();

    /* 1. ap_work consumption with IRQs on. */
    ap_work_run_one(cpu_id());
#ifdef AARCH64_AP_SCHED_WAKE_TEST
    aarch64_ap_sched_idle_hook(cpu_id(), AP_SCHED_IDLE_AFTER_WORK);
#endif

    /* 2. masked check under the rq lock. */
    arch_irq_state_t flags = arch_local_irq_save();
    uint64_t rq_flags = spin_lock_irqsave(&cpu->rq_lock);
    bool need = __atomic_load_n(&cpu->need_resched, __ATOMIC_ACQUIRE) ||
                !rbtree_empty(&cpu->run_queue);
    uint32_t slot_state =
        __atomic_load_n(&ap_work[cpu_id()].state, __ATOMIC_ACQUIRE);
    spin_unlock_irqrestore(&cpu->rq_lock, rq_flags);

    /* 3. runnable work → schedule (IRQ state restored around it). */
    if (need) {
        schedule();
        arch_local_irq_restore(flags);
        return;
    }

    /* 4. a READY slot → back to the consumption path next round. */
    if (slot_state == AP_WORK_READY) {
        arch_local_irq_restore(flags);
        return;
    }

    /* 5. nothing to do: keep IRQs MASKED across the wait. */
#ifdef AARCH64_AP_SCHED_WAKE_TEST
    /* stage 2 may wait (<=2s) for BSP permission; stage 3 publishes
     * WAIT_ARMED and returns at once — both IRQ-masked, no lock. */
    aarch64_ap_sched_idle_hook(cpu_id(), AP_SCHED_IDLE_AFTER_CHECK);
    aarch64_ap_sched_idle_hook(cpu_id(), AP_SCHED_IDLE_BEFORE_WAIT);
#endif
    aarch64_idle_wait_irq();     /* dsb sy; wfi; ret */
    arch_local_irq_enable();
#ifdef __aarch64__
    __asm__ __volatile__("isb" ::: "memory");
#endif
#ifdef AARCH64_AP_SCHED_WAKE_TEST
    aarch64_ap_sched_idle_hook(cpu_id(), AP_SCHED_IDLE_AFTER_WAIT);
#endif
}

void idle_resume(void)
{
    for (;;) {
        arch_aarch64_idle_step();
    }
}
