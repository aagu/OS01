/* @os01-test
 * id: test_sched_aarch64_migration
 * layer: host
 * subsystem: sched
 * timing: strict
 * tier: nightly
 * link: sched-aarch64-migration
 */

#define SCHED_NOTIFY_TEST_COMMON_SPIES 1
#include "test_framework.h"
#include "../mock/sched_notify_test_runtime.h"

static const struct task_struct *g_editing_task = NULL;

bool aarch64_vm_task_editing(const struct task_struct *task)
{
    return task && task == g_editing_task;
}

static void (*g_lock_hook)(spinlock_T *l) = NULL;

void mock_on_spin_lock(spinlock_T *l)
{
    if (g_lock_hook)
        g_lock_hook(l);
}

static void init_ready_cpus(uint32_t count)
{
    sched_notify_reset();
    num_cpus = count;
    user_init_pid = 1;
    g_editing_task = NULL;
    g_lock_hook = NULL;
    for (uint32_t i = 0; i < count; i++) {
        percpu_data[i].online = 1;
        percpu_data[i].ipi_ready = 1;
        percpu_data[i].scheduler_ok = 1;
        percpu_data[i].run_queue.rb_node = NULL;
        spin_init(&percpu_data[i].rq_lock);
    }
}

static task_t make_user_task(int64_t pid, uint32_t cpu, uint64_t allowed,
                             uint64_t vr, uint64_t dl)
{
    task_t t;
    memset(&t, 0, sizeof(t));
    t.state = TASK_RUNNING;
    t.flags = PF_PROCESS;
    t.pid = pid;
    t.cpu = cpu;
    t.allowed_cpus = allowed;
    t.vruntime = vr;
    t.deadline = dl;
    t.irq_return_migratable = true;
    t.on_cpu = 0;
    return t;
}

TEST_FUNC(test_positive_pull_user_task)
{
    init_ready_cpus(2);
    percpu_data[0].min_vruntime = 77;
    percpu_data[1].min_vruntime = 12;

    task_t t = make_user_task(42, 0, 3ULL, 5, 25);
    enqueue_task(&t, &percpu_data[0]);

    sched_balance(&percpu_data[1]);

    assert_eq(1, (int)t.cpu);
    assert_eq(0, (int)percpu_data[0].nr_running);
    assert_eq(1, (int)percpu_data[1].nr_running);
    assert_eq(12ULL, t.vruntime);
    assert_eq(12ULL + EEVDF_MIN_SLICE, t.deadline);
    assert_true(pick_eevdf(&percpu_data[1]) == &t);
    assert_eq(77ULL, percpu_data[0].min_vruntime);
}

TEST_FUNC(test_negative_gates)
{
    /* 1. Single CPU */
    init_ready_cpus(1);
    task_t t = make_user_task(42, 0, 1ULL, 5, 25);
    enqueue_task(&t, &percpu_data[0]);
    sched_balance(&percpu_data[0]);
    assert_eq(0, (int)t.cpu);
    assert_eq(1, (int)percpu_data[0].nr_running);

    /* 2. idle / init (pid == 1) */
    init_ready_cpus(2);
    task_t init_t = make_user_task(1, 0, 3ULL, 5, 25);
    enqueue_task(&init_t, &percpu_data[0]);
    enqueue_task(&init_task_union.task, &percpu_data[0]);
    sched_balance(&percpu_data[1]);
    assert_eq(0, (int)init_t.cpu);
    assert_eq(0, (int)init_task_union.task.cpu);
    assert_eq(0, (int)percpu_data[1].nr_running);

    /* 3. Not ready (online=0, scheduler_ok=0, ipi_ready=0) */
    for (int gate = 0; gate < 3; gate++) {
        init_ready_cpus(2);
        task_t u = make_user_task(42, 0, 3ULL, 5, 25);
        enqueue_task(&u, &percpu_data[0]);
        if (gate == 0) percpu_data[0].online = 0;
        if (gate == 1) percpu_data[0].scheduler_ok = 0;
        if (gate == 2) percpu_data[0].ipi_ready = 0;
        sched_balance(&percpu_data[1]);
        assert_eq(0, (int)u.cpu);
        assert_eq(0, (int)percpu_data[1].nr_running);
    }

    /* 4. Narrow mask */
    init_ready_cpus(2);
    t = make_user_task(42, 0, 1ULL, 5, 25);
    enqueue_task(&t, &percpu_data[0]);
    sched_balance(&percpu_data[1]);
    assert_eq(0, (int)t.cpu);

    /* 5. Source ownership mismatch */
    init_ready_cpus(2);
    t = make_user_task(42, 1, 3ULL, 5, 25);
    enqueue_task(&t, &percpu_data[0]);
    sched_balance(&percpu_data[1]);
    assert_eq(0, (int)percpu_data[1].nr_running);

    /* 6. Blocked / zombie */
    init_ready_cpus(2);
    t = make_user_task(42, 0, 3ULL, 5, 25);
    t.state = TASK_UNINTERRUPTIBLE;
    enqueue_task(&t, &percpu_data[0]);
    sched_balance(&percpu_data[1]);
    assert_eq(0, (int)percpu_data[1].nr_running);

    init_ready_cpus(2);
    t = make_user_task(42, 0, 3ULL, 5, 25);
    t.state = TASK_ZOMBIE;
    enqueue_task(&t, &percpu_data[0]);
    sched_balance(&percpu_data[1]);
    assert_eq(0, (int)percpu_data[1].nr_running);

    /* 7. on_cpu == 1 */
    init_ready_cpus(2);
    t = make_user_task(42, 0, 3ULL, 5, 25);
    t.on_cpu = 1;
    enqueue_task(&t, &percpu_data[0]);
    sched_balance(&percpu_data[1]);
    assert_eq(0, (int)percpu_data[1].nr_running);

    /* 8. irq_return_migratable == false */
    init_ready_cpus(2);
    t = make_user_task(42, 0, 3ULL, 5, 25);
    t.irq_return_migratable = false;
    enqueue_task(&t, &percpu_data[0]);
    sched_balance(&percpu_data[1]);
    assert_eq(0, (int)percpu_data[1].nr_running);

    /* 9. VM editing active */
    init_ready_cpus(2);
    t = make_user_task(42, 0, 3ULL, 5, 25);
    g_editing_task = &t;
    enqueue_task(&t, &percpu_data[0]);
    sched_balance(&percpu_data[1]);
    assert_eq(0, (int)percpu_data[1].nr_running);
}

TEST_FUNC(test_load_fixtures)
{
    /* Busy gap 1: no pull */
    init_ready_cpus(2);
    task_t d0 = make_user_task(10, 1, 3ULL, 10, 30);
    task_t s0 = make_user_task(20, 0, 3ULL, 10, 30);
    task_t s1 = make_user_task(21, 0, 3ULL, 11, 31);
    enqueue_task(&d0, &percpu_data[1]);
    enqueue_task(&s0, &percpu_data[0]);
    enqueue_task(&s1, &percpu_data[0]);
    sched_balance(&percpu_data[1]);
    assert_eq(2, (int)percpu_data[0].nr_running);
    assert_eq(1, (int)percpu_data[1].nr_running);

    /* Busy gap 2: pulls 1 */
    task_t s2 = make_user_task(22, 0, 3ULL, 12, 32);
    enqueue_task(&s2, &percpu_data[0]);
    sched_balance(&percpu_data[1]);
    assert_eq(2, (int)percpu_data[0].nr_running);
    assert_eq(2, (int)percpu_data[1].nr_running);

    /* Heaviest (CPU1) non-migratable, second heaviest (CPU2) donates to CPU0 */
    init_ready_cpus(3);
    task_t h0 = make_user_task(30, 1, 7ULL, 10, 30);
    task_t h1 = make_user_task(31, 1, 7ULL, 11, 31);
    h0.irq_return_migratable = false;
    h1.irq_return_migratable = false;
    enqueue_task(&h0, &percpu_data[1]);
    enqueue_task(&h1, &percpu_data[1]);

    task_t m0 = make_user_task(32, 2, 7ULL, 10, 30);
    enqueue_task(&m0, &percpu_data[2]);

    sched_balance(&percpu_data[0]);
    assert_eq(0, (int)m0.cpu);
    assert_eq(1, (int)percpu_data[0].nr_running);
    assert_eq(2, (int)percpu_data[1].nr_running);
    assert_eq(0, (int)percpu_data[2].nr_running);

    /* Tie in nr_running: lower CPU ID wins (CPU0 beats CPU2 for dst CPU1) */
    init_ready_cpus(3);
    task_t c0 = make_user_task(40, 0, 7ULL, 10, 30);
    task_t c2 = make_user_task(41, 2, 7ULL, 10, 30);
    enqueue_task(&c0, &percpu_data[0]);
    enqueue_task(&c2, &percpu_data[2]);
    sched_balance(&percpu_data[1]);
    assert_eq(1, (int)c0.cpu);
    assert_eq(2, (int)c2.cpu);
    assert_eq(0, (int)percpu_data[0].nr_running);
    assert_eq(1, (int)percpu_data[1].nr_running);
    assert_eq(1, (int)percpu_data[2].nr_running);
}

static task_t *g_drop_task = NULL;
static int g_drop_hook_armed = 0;

static void hook_drop_src_load(spinlock_T *l)
{
    if (!g_drop_hook_armed || l != &percpu_data[0].rq_lock)
        return;
    g_drop_hook_armed = 0;
    /* Only drop when neither lock is held (before first lock of pair). */
    dequeue_task(g_drop_task, &percpu_data[0]);
}

TEST_FUNC(test_interleave_load_drops_before_lock)
{
    init_ready_cpus(2);
    task_t d0 = make_user_task(10, 1, 3ULL, 10, 30);
    task_t s0 = make_user_task(20, 0, 3ULL, 10, 30);
    task_t s1 = make_user_task(21, 0, 3ULL, 11, 31);
    task_t s2 = make_user_task(22, 0, 3ULL, 12, 32);
    enqueue_task(&d0, &percpu_data[1]);
    enqueue_task(&s0, &percpu_data[0]);
    enqueue_task(&s1, &percpu_data[0]);
    enqueue_task(&s2, &percpu_data[0]);

    g_drop_task = &s2;
    g_drop_hook_armed = 1;
    g_lock_hook = hook_drop_src_load;
    sched_balance(&percpu_data[1]);
    g_lock_hook = NULL;

    /* Gap dropped from 2 to 1 before lock: no migration. */
    assert_eq(2, (int)percpu_data[0].nr_running);
    assert_eq(1, (int)percpu_data[1].nr_running);
}

static int g_wake_migrate_armed = 0;

static void hook_migrate_before_wake_lock(spinlock_T *l)
{
    if (!g_wake_migrate_armed || l != &percpu_data[0].rq_lock)
        return;
    g_wake_migrate_armed = 0;
    /* Run a real migration from CPU0 to CPU1 before task_wake locks CPU0. */
    sched_balance(&percpu_data[1]);
}

TEST_FUNC(test_interleave_wake_retries_after_migration)
{
    init_ready_cpus(2);
    task_t t = make_user_task(42, 0, 3ULL, 5, 25);
    enqueue_task(&t, &percpu_data[0]);

    g_wake_migrate_armed = 1;
    g_lock_hook = hook_migrate_before_wake_lock;
    task_wake(&t);
    g_lock_hook = NULL;

    assert_eq(1, (int)t.cpu);
    assert_true(t.on_rq);
    assert_eq(0, (int)percpu_data[0].nr_running);
    assert_eq(1, (int)percpu_data[1].nr_running);
    assert_true(pick_eevdf(&percpu_data[1]) == &t);
    /* Verify t appears only once in CPU1's rbtree. */
    dequeue_task(&t, &percpu_data[1]);
    assert_true(pick_eevdf(&percpu_data[1]) == NULL);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_positive_pull_user_task),
    TEST_ENTRY(test_negative_gates),
    TEST_ENTRY(test_load_fixtures),
    TEST_ENTRY(test_interleave_load_drops_before_lock),
    TEST_ENTRY(test_interleave_wake_retries_after_migration),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
