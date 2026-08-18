#include <kernel/clockevent.h>
#include <device/timer.h>
#include <kernel/clocksource.h>   // clocksource_read_ns()
#include <kernel/softirq.h>       // set_softirq_status, TIMER_SIRQ
#include <kernel/percpu.h>        // this_cpu()
#include <kernel/arch/cpu.h>      // arch_tick_start()
#include <kernel/interrupt.h>     // irq_mask / irq_unmask
#include <kernel/poll.h>          // poll_timeout_node_t, poll_timeout_head/lock
#include <kernel/wait.h>          // wait_queue_wake_all
#include <kernel/task.h>          // current
#include <kernel.h>               // container_of

// 从 pit_handler 迁来的 poll 超时注册表（定义在 kernel/fs/poll.c）。
extern poll_timeout_node_t *poll_timeout_head;
extern spinlock_T poll_timeout_lock;

void tick_init(void)
{
    // 静态注册阶段无 tick 源切换；PIT 由 pit_init 照常启动。
}

void tick_handler(void)
{
    jiffies++;

    // poll 超时扫描（纳秒比较）—— 从 pit_handler 迁来。
    // 与 poll.c 单位一致：poll.c 用 `clocksource_read_ns() + timeout*1e6` 注册
    // ns deadline（poll.c:do_poll_core），此扫描用同一时间轴比较。
    // ⚠️ 时序假设：boot 期 poll_timeout_head 恒 NULL（poll 只在用户态进程里调，
    // 用户态进程 task_init() 之后才有），此短路保证 GS base 装之前（phase 4 到
    // main.c:276）不调 clocksource_read_ns()（它读 this_cpu()->tsc_offset）。
    if (poll_timeout_head) {
        spin_lock(&poll_timeout_lock);
        for (poll_timeout_node_t *n = poll_timeout_head; n; n = n->next)
            if (clocksource_read_ns() >= n->deadline)
                wait_queue_wake_all(n->wq);
        spin_unlock(&poll_timeout_lock);
    }

    this_cpu()->need_resched = 1;
    this_cpu()->watchdog_counter++;

    if ((container_of(list_next(&timer_list_head.list), timer_t, list)->expire_jiffies <= jiffies))
        set_softirq_status(TIMER_SIRQ);
}

void tick_start(void)
{
    // 先掩 PIT IRQ0，防止交接窗口 LAPIC+PIT 双计 jiffies。
    irq_mask(0);
    if (arch_tick_start()) {
        // LAPIC 接管成功，PIT 保持掩蔽。
    } else {
        // LAPIC 未校准/失败：回退 PIT。
        irq_unmask(0);
    }
}