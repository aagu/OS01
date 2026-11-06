#include <time/clockevent.h>
#include <time/timer.h>
#include <time/clocksource.h>   // clocksource_read_ns()
#include <intr/softirq.h>       // set_softirq_status, TIMER_SIRQ
#include <percpu/percpu.h>        // this_cpu()
#include <arch/cpu.h>      // arch_tick_start()
#include <intr/interrupt.h>     // irq_mask / irq_unmask
#include <sync/wait.h>          // wait_queue_wake_all
#include <sched/task.h>          // current
#include <sched/accounting.h>    // sched_account_tick()
#include <kernel.h>               // container_of

// Per-tick poll-timeout scan. Strong definition lives in kernel/fs/poll.c
// (x86_64 path); the weak default below is what aarch64 phase 1 builds
// (no fs/poll.c compiled).
__attribute__((weak)) void poll_timeout_tick(void);

void tick_handler(void)
{
    jiffies++;

#ifdef __x86_64__
    // Monitor plan Task 2: BSP tick accounting — exactly ONE sample per
    // tick here.  This covers both the PIT fallback and the BSP LAPIC
    // path; the BSP branch of lapic_timer_handler() routes through this
    // function and must not call sched_account_tick() a second time.
    // (APs account separately in lapic_timer_handler()'s AP branch.)
    sched_account_tick();

    poll_timeout_tick();

    __atomic_store_n(&this_cpu()->need_resched, 1, __ATOMIC_RELEASE);
    this_cpu()->watchdog_counter++;
#else
    /* aarch64: this is the BSP's GLOBAL tick — it advances the global
     * time base only (jiffies, poll timeout, timer list / TIMER_SIRQ).
     * The per-CPU accounting, the need_resched request and the
     * watchdog counter live in the ready-gated local scheduler tick
     * (kernel/arch/aarch64/cpu/sched_tick.c): aarch64 charges the
     * vruntime per DELIVERED local tick, not per schedule() call, and
     * an AP must never advance the global time base. */
    poll_timeout_tick();
#endif

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
