#include <arch/context.h>
#include <arch/x86_64/regs.h>
#include <arch/x86_64/trap.h>
#include <intr/softirq.h>
#include <percpu/percpu.h>
#include <sched/task.h>

void arch_context_run_deferred_work(void)
{
    if (softirq_status)
        do_softirq();
}

bool arch_context_reschedule_return(struct pt_regs *frame)
{
    (void)frame;
    uint64_t need_resched = 0;

    __asm__ volatile("movq %%gs:%c1, %0"
                     : "=r"(need_resched)
                     : "i"(PERCPU_NEED_RESCHED_OFFSET)
                     : "memory");
    if (need_resched) {
        __asm__ volatile("movq $0, %%gs:%c0"
                         :
                         : "i"(PERCPU_NEED_RESCHED_OFFSET)
                         : "memory");
        schedule();
    }
    return true;
}

void arch_context_deliver_return_signals(struct pt_regs *frame,
                                         enum context_return_mode mode)
{
    if (mode == CONTEXT_RETURN_SIGNAL_ONLY) {
        if (frame->cs & 3)
            arch_do_signal_delivery(frame);
        return;
    }

    while (__atomic_load_n(&current->signal, __ATOMIC_ACQUIRE) != 0) {
        if (!arch_do_signal_delivery(frame))
            break;
    }
}
