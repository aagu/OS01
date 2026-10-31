/* aarch64 scheduling-policy backends (<arch/sched.h>, P1 spec §4).
 *
 * P1 polling contract: there is no resched SGI yet.  Every aarch64
 * scheduler CPU (BSP and, from P1 Task 3 on, accepted APs) polls its
 * own runqueue and need_resched on every idle iteration, so a remote
 * enqueue is discovered on the next poll without any notification.
 * This file is deliberately free of arch intrinsics so the hosttests
 * can compile it against the fixture surface.
 */
#include <arch/sched.h>
#include <percpu/percpu.h>

uint64_t arch_kthread_default_mask(void)
{
    /* Ordinary kernel threads stay on CPU0; explicit pinned targets
     * carry their own single-bit mask (kernel_thread_on_cpu). */
    return 1;
}

void arch_sched_notify_remote(unsigned cpu)
{
    /* Polling backend: the target discovers the work on its next idle
     * poll of need_resched / its runqueue.  P2 replaces this with the
     * resched SGI (aarch64_sched_kick). */
    (void)cpu;
}

bool arch_sched_scan_blockers(void)
{
    /* Only the BSP scans the global blocker list: an AP-installed
     * blocker could never be discovered by the single scanner, so APs
     * are refused blocker_wait() (-EOPNOTSUPP) and the scanner skips
     * tasks pinned to other CPUs before touching their state. */
    return cpu_id() == 0;
}
