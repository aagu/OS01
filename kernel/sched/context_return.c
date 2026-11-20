#include <arch/context.h>

__attribute__((weak)) void arch_context_run_deferred_work(void)
{
}

void context_run_return_work(struct pt_regs *frame, enum context_return_mode mode)
{
    if (mode == CONTEXT_RETURN_FULL) {
        arch_context_run_deferred_work();
        if (!arch_context_reschedule_return(frame))
            return;
    }
    arch_context_deliver_return_signals(frame, mode);
}
