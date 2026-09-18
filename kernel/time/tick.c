#include <time/clockevent.h>
#include <time/timer.h>
#include <time/clocksource.h>   // clocksource_read_ns()
#include <intr/softirq.h>       // set_softirq_status, TIMER_SIRQ
#include <percpu/percpu.h>        // this_cpu()
#include <arch/cpu.h>      // arch_tick_start()
#include <intr/interrupt.h>     // irq_mask / irq_unmask
#include <fs/poll.h>          // poll_timeout_node_t, poll_timeout_head/lock
#include <sync/wait.h>          // wait_queue_wake_all
#include <sched/task.h>          // current
#include <kernel.h>               // container_of

// 从 pit_handler 迁来的 poll 超时注册表（定义在 kernel/fs/poll.c）。
extern poll_timeout_node_t *poll_timeout_head;
extern spinlock_T poll_timeout_lock;

void tick_handler(void)
{
    jiffies++;

#if defined(__x86_64__)
    // x86_64-only poll-timeout scan. aarch64 phase 1 has no userland
    // processes (no init_thread, no scheduler, no /dev/poll); the
    // poll-timeout scan is dead code on aarch64.
    if (poll_timeout_head) {
        uint64_t flags = spin_lock_irqsave(&poll_timeout_lock);
        for (poll_timeout_node_t *n = poll_timeout_head; n; n = n->next)
            if (clocksource_read_ns() >= n->deadline)
                wait_queue_wake_all(n->wq);
        spin_unlock_irqrestore(&poll_timeout_lock, flags);
    }
#endif

    this_cpu()->need_resched = 1;
    this_cpu()->watchdog_counter++;

    if ((container_of(list_next(&timer_list_head.list), timer_t, list)->expire_jiffies <= jiffies))
        set_softirq_status(TIMER_SIRQ);
}

void tick_start(void)
{
#if defined(__x86_64__)
    // x86_64-only PIT/LAPIC handoff ceremony. aarch64 phase 1 has
    // no PIT, no LAPIC; kernel/arch/aarch64/main.c:334 calls
    // arch_tick_start() directly without going through tick_start().
    // tick_start() is dead code on aarch64.
    irq_mask(0);
    if (arch_tick_start()) {
        // LAPIC 接管成功，PIT 保持掩蔽。
    } else {
        // LAPIC 未校准/失败：回退 PIT。
        irq_unmask(0);
    }
#endif
}
