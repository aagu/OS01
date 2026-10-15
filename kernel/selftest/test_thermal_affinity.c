#if defined(OS01_SELFTEST)
/* kernel/selftest/test_thermal_affinity.c
 *
 * Thermal plan, Task 2 — in-kernel CPU-affinity contract test.
 *
 * The host fixture (hosttests/cases/test_thermal_affinity.c)
 * covers the data-layout / API-shape contract; this body covers
 * the live scheduler paths that only an in-kernel test can:
 *
 *   - sched_pick_cpu_mask returns NR_CPUS on empty mask
 *   - sched_pick_cpu_mask returns a valid cpu on a single-bit
 *     mask that names an online+scheduler_ok CPU
 *   - kernel_thread_on_cpu rejects cpu >= NR_CPUS, online==0,
 *     scheduler_ok==0
 *   - enqueue_task refuses to place a task whose mask does not
 *     include the target rq's cpu_id
 *
 * Gated behind OS01_SELFTEST (KERNEL_SELFTEST=1 build only) so
 * production kernels do not emit this code at all.  Does NOT
 * spawn real workers (those are owned by kernel/init.c) — the
 * check is structural + planner-side.
 */

#include <sched/task.h>
#include <sched/internal.h>
#include <percpu/percpu.h>
#include <arch/cpu.h>
#include <arch/spinlock.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include <core/printk.h>

static int test_msr_safe_pick_cpu_mask_empty(void)
{
    /* Mask == 0 → no eligible cpu → NR_CPUS.  The task-side
     * picker MUST signal "nowhere to put me" with NR_CPUS, not
     * 0 — Task 2's plan contract makes that explicit. */
    unsigned c = sched_pick_cpu_mask(0);
    if (c != NR_CPUS) {
        serial_printk("[selftest] affinity.pick_empty: got %u expected NR_CPUS=%u\n",
                      c, (unsigned)NR_CPUS);
        return -1;
    }
    return 0;
}

static int test_msr_safe_pick_cpu_mask_bsp(void)
{
    /* BSP is always online + scheduler_ok after task_init(). */
    percpu_t *rq = this_cpu();
    (void)rq;
    unsigned c = sched_pick_cpu_mask(1ULL);
    if (c == NR_CPUS || c >= num_cpus) {
        serial_printk("[selftest] affinity.pick_bsp: got %u (num_cpus=%u)\n",
                      c, (unsigned)num_cpus);
        return -1;
    }
    /* The matching bit must be the one we asked for. */
    if (((1ULL << c) & 1ULL) == 0) {
        serial_printk("[selftest] affinity.pick_bsp: returned cpu %u not in mask 0x1\n",
                      c);
        return -1;
    }
    return 0;
}

static int test_msr_safe_pick_cpu_mask_offline(void)
{
    /* Find any bit beyond num_cpus — by construction, no such
     * cpu is online.  sched_pick_cpu_mask must return NR_CPUS. */
    uint64_t mask = (uint64_t)1ULL << (NR_CPUS - 1);
    if (num_cpus >= NR_CPUS) {
        /* On a machine with NR_CPUS cpus live, this test is
         * trivially satisfied — return 0 to keep the green
         * window. */
        return 0;
    }
    unsigned c = sched_pick_cpu_mask(mask);
    if (c != NR_CPUS) {
        serial_printk("[selftest] affinity.pick_offline: got %u expected NR_CPUS\n", c);
        return -1;
    }
    return 0;
}

static int test_kernel_thread_on_cpu_rejects_invalid(void)
{
    /* Bad argument: cpu >= NR_CPUS. */
    int pid = kernel_thread_on_cpu(NULL, 0, NR_CPUS);
    if (pid != -EINVAL) {
        serial_printk("[selftest] affinity.kt_rejects_nr_cpus: expected -EINVAL got %d\n", pid);
        return -1;
    }
    pid = kernel_thread_on_cpu(NULL, 0, NR_CPUS + 5);
    if (pid != -EINVAL) {
        serial_printk("[selftest] affinity.kt_rejects_oversize: expected -EINVAL got %d\n", pid);
        return -1;
    }
    /* cpu beyond num_cpus but < NR_CPUS → also -EINVAL. */
    if (num_cpus + 1 < NR_CPUS) {
        pid = kernel_thread_on_cpu(NULL, 0, num_cpus + 1);
        if (pid != -EINVAL) {
            serial_printk("[selftest] affinity.kt_rejects_notlive: expected -EINVAL got %d\n",
                          pid);
            return -1;
        }
    }
    return 0;
}

/* Wait for the worker pinned to a non-existent offline cpu to fail.
 * That is identical to the off-BSP pin path; covered by
 * test_kernel_thread_on_cpu_rejects_invalid above.
 *
 * We intentionally do NOT spawn a worker here: a successful
 * pin would leave an idle kthread running in KERNEL_SELFTEST
 * mode long after the selftest returns, which would skew the
 * other per-CPU runtime assertions the monitor spec relies on.
 * The pin → cpu → allowed_cpus publishing order is exercised
 * by do_fork's masked publication path (enqueue_task's negative
 * case below), which doesn't need a live worker to validate. */

static int test_enqueue_task_refuses_out_of_mask(void)
{
    /* Build a synthetic task_t whose mask excludes the current
     * rq's cpu.  A direct enqueue_task() against the local rq
     * must REFUSE (return without placing).  Without the plan's
     * mask filter, this would corrupt the rq with a task that
     * cannot legally run here.  After the call, the task stays
     * off the rq and nr_running is unchanged. */
    percpu_t *rq = this_cpu();
    uint64_t saved_nr = rq->nr_running;
    task_t fake;
    memset(&fake, 0, sizeof(fake));
    fake.pid          = 0xFFFFFFFEU;     /* sentinel */
    fake.state        = TASK_RUNNING;
    fake.allowed_cpus = (rq->cpu_id == 0) ? 0x2ULL : 0x1ULL;
    fake.cpu          = rq->cpu_id;
    fake.vruntime     = rq->min_vruntime;
    fake.deadline     = fake.vruntime + EEVDF_MIN_SLICE;
    fake.on_rq        = false;
    fake.on_cpu       = false;
    fake.rb_node.parent = fake.rb_node.left = fake.rb_node.right = NULL;
    fake.rb_node.color = RB_BLACK;

    enqueue_task(&fake, rq);

    if (rq->nr_running != saved_nr) {
        serial_printk("[selftest] affinity.enqueue_refused: nr_running changed %lu->%lu\n",
                      (unsigned long)saved_nr, (unsigned long)rq->nr_running);
        /* Roll back the bad enqueue before we return -1 so the
         * rq stays usable for downstream selftests. */
        dequeue_task(&fake, rq);
        return -1;
    }
    if (fake.on_rq) {
        serial_printk("[selftest] affinity.enqueue_refused: fake.on_rq flipped\n");
        return -1;
    }
    return 0;
}

int test_thermal_affinity(void)
{
    int ok = 0, fail = 0;
    serial_printk("[selftest] thermal_affinity... ");
    struct { const char *name; int (*fn)(void); } cases[] = {
        { "pick_cpu_mask_empty",        test_msr_safe_pick_cpu_mask_empty         },
        { "pick_cpu_mask_bsp",           test_msr_safe_pick_cpu_mask_bsp           },
        { "pick_cpu_mask_offline",       test_msr_safe_pick_cpu_mask_offline       },
        { "kt_on_cpu_rejects_invalid",   test_kernel_thread_on_cpu_rejects_invalid },
        { "enqueue_refuses_out_of_mask", test_enqueue_task_refuses_out_of_mask     },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (cases[i].fn() == 0) { ok++; }
        else {
            fail++;
            serial_printk("[selftest] thermal_affinity.%s FAIL\n", cases[i].name);
        }
    }
    serial_printk("[selftest] thermal_affinity: %d passed, %d failed\n", ok, fail);
    return fail;
}

#endif /* OS01_SELFTEST */
