/* test_sched_boot_probe.c — x86 UP boot-window probe, late selftest half
 * (aarch64 EL1 scheduler plan, Phase 5 Task 4 / spec §10.1 item 4).
 *
 * The boot half (sched_boot_probe_create) runs from kernel_main inside
 * the legal pre-publication window; this file owns the marker function,
 * the shared state and the late case that proves the thread ACTUALLY ran
 * under the scheduler after scheduler_ok=1 (never a host mock or a
 * source scan), plus the runtime negative API calls.
 *
 * x86-only (picked up by the x86 selftest wildcard; NOT in the aarch64
 * source list).  Compiles in production builds too, but both entry
 * points are only invoked under OS01_SELFTEST.
 */

#include <sched/task.h>
#include <percpu/percpu.h>
#include <time/timer.h>
#include <errno.h>
#include <selftest/sched_boot_probe.h>

volatile unsigned sched_boot_probe_ran;
volatile int sched_boot_probe_pid = -1;

static uint64_t sched_boot_probe_fn(uint64_t arg)
{
    (void)arg;
    sched_boot_probe_ran = 1;
    return 0;
}

void sched_boot_probe_create(void)
{
    int pid = kernel_thread(sched_boot_probe_fn, 0, PF_KTHREAD);
    sched_boot_probe_pid = pid;
}

int test_sched_boot_probe(void)
{
    /* 1. The boot-phase creation must have succeeded while CPU0 was
     *    online but scheduler_ok == 0 (no eligible CPU existed). */
    if (sched_boot_probe_pid < 1)
        return 1;

    /* 2. The marker must have ACTUALLY executed under the scheduler —
     *    bounded wait through the production schedule(), no test
     *    scheduler.  This case runs from task_init() on the idle task,
     *    so schedule() is legal here (same precondition as every other
     *    late selftest case). */
    if (!sched_boot_probe_ran) {
        uint64_t deadline = jiffies + 200;   /* 2 s at 100 Hz — bounded */
        while (!sched_boot_probe_ran && jiffies < deadline)
            schedule();
    }
    if (!sched_boot_probe_ran)
        return 2;

    /* 3. Runtime negatives: the boot window is closed permanently, so
     *    the pinned API rejects every illegal target, and garbage flags
     *    are rejected.  (A live "no eligible CPU at all" state cannot be
     *    arranged after boot without breaking the system; the host
     *    kthread-ctor suite pins that case against the real
     *    window state machine.) */
    if (kernel_thread_on_cpu(sched_boot_probe_fn, 0, num_cpus) != -EINVAL)
        return 3;
    if (kernel_thread_on_cpu(sched_boot_probe_fn, 0, NR_CPUS + 3) != -EINVAL)
        return 4;
    if (kernel_thread(sched_boot_probe_fn, 0, ~0ULL) != -EINVAL)
        return 5;
    return 0;
}
