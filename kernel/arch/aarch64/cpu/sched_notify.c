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
#include <arch/aarch64/ipi.h>
#include <intr/ipi.h>
#include <percpu/percpu.h>
#include <log/log.h>

uint64_t arch_kthread_default_mask(void)
{
    /* Ordinary kernel threads stay on CPU0; explicit pinned targets
     * carry their own single-bit mask (kernel_thread_on_cpu). */
    return 1;
}

void aarch64_sched_kick(unsigned cpu)
{
    /* Frozen P2 contract (spec §5.1): kick depends ONLY on the
     * target's ipi_ready — a not-yet-scheduler AP still takes SGIs in
     * its old work loop, and reading scheduler_ok here would reopen
     * the store-buffering hole the contract closes. */
    if (cpu >= NR_CPUS || cpu >= num_cpus ||
        !__atomic_load_n(&percpu_data[cpu].ipi_ready, __ATOMIC_ACQUIRE)) {
        log_err("[sched] aarch64_sched_kick: target cpu=%u not IPI-ready\n",
                cpu);
        for (;;)
            arch_cpu_halt();   /* fail-stop: never send into the void */
    }
    if ((int)cpu == (int)cpu_id())
        return;   /* local: need_resched is already published */
    /* Make the need_resched publication globally visible before the
     * MMIO write (spec §5.1: dsb ishst before GICD_SGIR).  The host
     * build of this TU (backend contract tests) has no barrier: the
     * fixture drives the admission/order logic, the real barrier is
     * proven by the ARM gates. */
#ifdef __aarch64__
    __asm__ __volatile__("dsb ishst" ::: "memory");
#endif
    ipi_broadcast(IPI_VECTOR_RESCHED, 1ULL << cpu);
}

void arch_sched_notify_remote(unsigned cpu)
{
    /* P2 backend: same delivery path as ap_work's post-READY kick. */
    aarch64_sched_kick(cpu);
}

bool arch_sched_scan_blockers(void)
{
    /* Only the BSP scans the global blocker list: an AP-installed
     * blocker could never be discovered by the single scanner, so APs
     * are refused blocker_wait() (-EOPNOTSUPP) and the scanner skips
     * tasks pinned to other CPUs before touching their state. */
    return cpu_id() == 0;
}
