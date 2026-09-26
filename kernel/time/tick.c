#include <time/clockevent.h>
#include <time/timer.h>
#include <time/clocksource.h>   // clocksource_read_ns()
#include <intr/softirq.h>       // set_softirq_status, TIMER_SIRQ
#include <percpu/percpu.h>        // this_cpu()
#include <arch/cpu.h>      // arch_tick_start()
#include <intr/interrupt.h>     // irq_mask / irq_unmask
#include <sync/wait.h>          // wait_queue_wake_all
#include <sched/task.h>          // current
#include <kernel.h>               // container_of

// Per-tick poll-timeout scan. Strong definition lives in kernel/fs/poll.c
// (x86_64 path); the weak default below is what aarch64 phase 1 builds
// (no fs/poll.c compiled).
__attribute__((weak)) void poll_timeout_tick(void);

void tick_handler(void)
{
    jiffies++;

    poll_timeout_tick();

    this_cpu()->need_resched = 1;
    this_cpu()->watchdog_counter++;

    if ((container_of(list_next(&timer_list_head.list), timer_t, list)->expire_jiffies <= jiffies))
        set_softirq_status(TIMER_SIRQ);
}

void tick_start(void)
{
    arch_tick_start();
}

// Weak default for poll_timeout_tick(). Strong override is in
// kernel/fs/poll.c (compiled only on x86_64). aarch64 phase 1 has no
// userland processes, no /dev/poll, no fs/poll.c — the weak default
// is the only definition and is a no-op.
__attribute__((weak)) void poll_timeout_tick(void)
{
    /* Default no-op. Strong override lives in kernel/fs/poll.c
     * (x86_64 path). aarch64 phase 1 does not compile fs/poll.c;
     * the weak default is the only definition. */
}
