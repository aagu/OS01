/* test_aarch64_sched_charge.c — the aarch64 schedule-charge
 * regression (per-CPU tick accounting plan, Task 2, step 2).
 *
 * The REAL kernel/sched/core.c, kernel/sched/fair.c and
 * kernel/sched/balance.c are host-compiled via
 * ../mock/blocker_runtime.h in its BLOCKER_RUNTIME_REAL_FAIR mode:
 * update_curr() is the REAL implementation — the assertion is about
 * the real vruntime value, never about a call counter.
 *
 * The same source is compiled TWICE:
 *
 *   - the aarch64 view (-D__aarch64__): kernel/sched/core.c takes the
 *     aarch64 branch, so schedule() does NOT charge the vruntime.  A
 *     burst of schedules without a single delivered local tick must
 *     therefore cost NOTHING.
 *   - the x86_64 view (no -D__aarch64__): schedule() still charges the
 *     vruntime — the x86 behaviour is unchanged.
 *
 * The host system ABI macros are preserved (-U__x86_64__ would break
 * the host libc), and the machine headers are masked by the mock's
 * preinclude, so the aarch64 view never executes aarch64 assembly.
 *
 * The scenario is the plan's: a single runnable worker on one CPU, so
 * schedule() takes the ordinary self-pick path.
 */

/* Selects the controllable arch-hook spy definitions in the mock
 * header; must be set BEFORE the mock include. */
#define BLOCKER_TEST_SPIES 1

#include "test_framework.h"
#include "../mock/blocker_runtime.h"

#ifndef EXPECT_SCHED_CHARGE
#error "EXPECT_SCHED_CHARGE must be defined (0 = aarch64 view, 1 = x86 view)"
#endif

static void schedule_charge_regression(void)
{
    sched_notify_reset();

    task_t worker;
    memset(&worker, 0, sizeof(worker));
    worker.state = TASK_RUNNING;
    worker.pid = 99;
    worker.cpu = 0;
    worker.allowed_cpus = 1ULL;
    worker.thread = &init_thread;
    worker.vruntime = 0;
    worker.deadline = 1000;

    /* The idle task of CPU0 and a runnable worker on the same CPU:
     * schedule() takes the ordinary self-pick path. */
    percpu_data[0].idle = &init_task_union.task;
    percpu_data[0].scheduler_ok = 1;
    mock_sched_this_cpu = &percpu_data[0];
    mock_sched_current = &worker;

    for (int i = 0; i < 16; i++)
        schedule();

    /* The runtime (task runtime_ticks) is charged by the per-CPU tick
     * accounting only — never by schedule(). */
    assert_eq(0, worker.runtime_ticks);

#if EXPECT_SCHED_CHARGE
    /* x86_64: the schedule() call still charges the vruntime. */
    assert_eq(16, worker.vruntime);
#else
    /* aarch64: the vruntime is charged per DELIVERED local tick, not
     * per schedule() call.  Sixteen schedules without a tick must not
     * cost anything. */
    assert_eq(0, worker.vruntime);
#endif
}

#if !EXPECT_SCHED_CHARGE
bool aarch64_vm_task_editing(const struct task_struct *task)
{
    (void)task;
    return false;
}

static void aarch64_balance_vetoes_pf_process(void)
{
    sched_notify_reset();
    num_cpus = 2;
    percpu_data[0].online = 1;
    percpu_data[0].scheduler_ok = 1;
    percpu_data[1].online = 1;
    percpu_data[1].scheduler_ok = 1;

    task_t user_proc;
    memset(&user_proc, 0, sizeof(user_proc));
    user_proc.state = TASK_RUNNING;
    user_proc.pid = 42;
    user_proc.flags = PF_PROCESS;
    user_proc.cpu = 0;
    user_proc.allowed_cpus = 3ULL;
    user_proc.vruntime = 10;
    user_proc.deadline = 20;
    user_proc.on_cpu = 0;
    enqueue_task(&user_proc, &percpu_data[0]);

    sched_balance(&percpu_data[1]);
    assert_eq(0, (int)user_proc.cpu);
    assert_eq(1, (int)percpu_data[0].nr_running);
    assert_eq(0, (int)percpu_data[1].nr_running);
}
#endif

TEST_LIST_BEGIN
    TEST_ENTRY(schedule_charge_regression),
#if !EXPECT_SCHED_CHARGE
    TEST_ENTRY(aarch64_balance_vetoes_pf_process),
#endif
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed != 0;
}
