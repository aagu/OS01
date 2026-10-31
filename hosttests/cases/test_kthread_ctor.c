/* test_kthread_ctor.c — the arch-neutral kernel-thread constructor
 * kthread_alloc_and_publish() + the boot creation window
 * (aarch64 EL1 scheduler plan Phase 2, Task 1 Step 2).
 *
 * The REAL kernel/sched/kthread.c AND the REAL kernel/sched/boot.c are
 * host-compiled via ../mock/switch_complete_test_runtime.h (fail-index
 * slab, mock percpu, spy enqueue/pick/identity/arch hooks); every case
 * below drives the production constructor and the production window
 * state machine — no test-local reimplementation of either exists.
 *
 * Suites (order matters — the window latch in the real boot.c is
 * process-global and closes permanently):
 *
 *   1. window starts closed: an unpinned create with no eligible CPU
 *      is -EINVAL before sched_boot_creation_begin() ever ran.
 *   2. legal x86 boot fallback: with the window open and CPU0 online
 *      but scheduler_ok == 0, an unpinned create queues on CPU0 while
 *      scheduler_ok remains 0.
 *   3. rollback, each fault point (raw task-union alloc, thread-struct
 *      alloc, arch resource prepare): negative errno, task-list length
 *      unchanged, target rq nr_running unchanged, zero live slab bytes,
 *      empty outputs, no arch resource leak.
 *   4. pinned API never gets the exception + failed-pin-then-create.
 *   5. empty affinity (num_cpus == 0) is rejected.
 *   6. the window closes permanently at sched_boot_start() and never
 *      reopens, even after scheduler_ok is cleared and begin() reruns.
 *   7. runtime creation with an eligible CPU, flags contract
 *      (0 and PF_KTHREAD accepted, anything else -EINVAL).
 *
 * Real execution after boot publication is proved by the x86 UP focused
 * QEMU case assigned to Phase 5, not by this mock harness.
 */

#include "test_framework.h"
#include "../mock/switch_complete_test_runtime.h"
#include <errno.h>

static uint64_t ctor_fn(uint64_t arg)
{
    return arg;
}

/* ── Helpers ─────────────────────────────────────────────────── */

static int task_list_len(void)
{
    if (init_task_union.task.list.next == NULL)
        return 0;
    int n = 0;
    for (list_t *p = init_task_union.task.list.next;
         p != &init_task_union.task.list; p = p->next)
        n++;
    return n;
}

static task_t *first_created_task(void)
{
    if (task_list_len() == 0)
        return NULL;
    list_t *p = init_task_union.task.list.next;
    return (task_t *)((char *)p - __builtin_offsetof(task_t, list));
}

/* The shared boot-phase precondition: CPU0 up, scheduler NOT started. */
static void boot_phase_cpu0_up(void)
{
    num_cpus = 1;
    percpu_data[0].online = 1;
    percpu_data[0].scheduler_ok = 0;
    percpu_data[0].nr_running = 0;
}

/* ── 1. Window starts closed: runtime no-target is -EINVAL ── */
static void suite_window_initially_closed(void)
{
    TEST_SUITE("window starts closed (runtime no-target)");
    switch_tail_reset();
    boot_phase_cpu0_up();
    /* No sched_boot_creation_begin() yet: scheduler_ok == 0 means no
     * eligible CPU, and a runtime "no target" is NOT a boot-phase case. */
    assert_true(!sched_boot_creation_open());
    assert_true(kernel_thread(ctor_fn, 0, PF_KTHREAD) == -EINVAL);
    assert_eq(0, task_list_len());
    assert_eq(0, (int)percpu_data[0].nr_running);
    assert_true((size_t)0 == mock_kmalloc_live_bytes);
    assert_true(create_kthread(ctor_fn, 0, "x") == NULL);
}

/* ── 2. Legal boot fallback queues CPU0, scheduler_ok stays 0 ── */
static void suite_boot_fallback(void)
{
    TEST_SUITE("boot fallback queues CPU0 while scheduler_ok == 0");
    switch_tail_reset();
    boot_phase_cpu0_up();
    sched_boot_creation_begin();
    assert_true(sched_boot_creation_open());

    task_t *tsk = NULL;
    int pid = 0;
    int rc = kthread_alloc_and_publish(ctor_fn, 42, NULL, 0, false, &tsk, &pid);

    assert_eq(0, rc);
    assert_true(tsk != NULL);
    assert_true(pid >= 1);
    assert_eq(pid, (int)tsk->pid);

    /* queued on CPU0 — while scheduler_ok is STILL 0 */
    assert_eq(0, (int)tsk->cpu);
    assert_eq(1, task_list_len());
    assert_true(first_created_task() == tsk);   /* it is on the global list */
    assert_eq(1, (int)percpu_data[0].nr_running);
    assert_true(tsk->on_rq);
    assert_eq(0, (int)percpu_data[0].scheduler_ok);

    /* published as a plain PF_KTHREAD with the default affinity reach */
    assert_true((tsk->flags & PF_KTHREAD) != 0);
    assert_true((tsk->flags & PF_SELF_REAP) == 0);
    assert_eq(1ULL, tsk->allowed_cpus);      /* num_cpus == 1 → mask bit 0 */
    assert_true(tsk->state == TASK_RUNNING);

    /* identity/metadata: comm inherited from the creator under the lock,
     * parent recorded, empty blocker */
    assert_true(strcmp(tsk->comm, "creator") == 0);
    assert_true(tsk->parent == switch_tail_current);
    assert_eq(0, tsk->blocker.type);

    /* the arch hook ran and acquired its resources */
    assert_eq(1, mock_arch_prepare_calls);
    assert_eq(1, mock_arch_files_live);
    assert_eq(1, mock_notify_remote_calls);
}

/* ── 3. Rollback at each fault point ─────────────────────────── */
static void assert_clean_rollback(const char *what, int rc)
{
    assert_true(rc < 0); /* negative errno */
    assert_eq(0, task_list_len());
    assert_eq(0, (int)percpu_data[0].nr_running);
    assert_true((size_t)0 == mock_kmalloc_live_bytes); /* zero live bytes */
    assert_eq(0, mock_arch_files_live);                /* no arch leak */
    (void)what;
}

/* P5 Task 4 (spec §10.1 item 3): after EACH failure point, a subsequent
 * NORMAL creation must succeed and publish a live task — the rollback
 * leaves no residue that poisons the next attempt. */
static void assert_retry_succeeds(const char *what)
{
    mock_kmalloc_fail_at = 0;              /* the NEXT creation is normal */
    mock_arch_prepare_rc = 0;
    task_t *retry = (task_t *)1;
    int retry_pid = 0;
    int rc = kthread_alloc_and_publish(ctor_fn, 7, NULL, 0, false,
                                       &retry, &retry_pid);
    assert_eq(0, rc);
    assert_true(retry != NULL);
    assert_true(retry_pid > 0);
    assert_eq(1, task_list_len());
    assert_eq(1, (int)percpu_data[0].nr_running);
    assert_true((size_t)0 != mock_kmalloc_live_bytes);
    (void)what;
}

static void suite_rollback(void)
{
    TEST_SUITE("rollback: raw task-union allocation");
    switch_tail_reset();
    boot_phase_cpu0_up();
    mock_kmalloc_fail_at = 1;              /* the task-union kmalloc */
    task_t *tsk = (task_t *)1;
    int pid = 5;
    int rc = kthread_alloc_and_publish(ctor_fn, 0, NULL, 0, false, &tsk, &pid);
    assert_clean_rollback("raw alloc", rc);
    assert_true(rc == -ENOMEM);
    assert_true(tsk == NULL);              /* failure leaves outputs empty */
    assert_eq(0, pid);
    assert_retry_succeeds("retry after raw alloc");

    TEST_SUITE("rollback: thread-struct allocation");
    switch_tail_reset();
    boot_phase_cpu0_up();
    mock_kmalloc_fail_at = 2;              /* the thread_t kmalloc */
    tsk = (task_t *)1;
    pid = 5;
    rc = kthread_alloc_and_publish(ctor_fn, 0, NULL, 0, false, &tsk, &pid);
    assert_clean_rollback("thread alloc", rc);
    assert_true(rc == -ENOMEM);
    assert_true(tsk == NULL);
    assert_eq(0, pid);
    assert_true(spy_kfree_calls >= 1);     /* the raw stack was released */
    assert_retry_succeeds("retry after thread alloc");

    TEST_SUITE("rollback: FPU allocation");
    switch_tail_reset();
    boot_phase_cpu0_up();
    mock_kmalloc_fail_at = 3;              /* task + thread + FPU area */
    tsk = (task_t *)1;
    pid = 5;
    rc = kthread_alloc_and_publish(ctor_fn, 0, NULL, 0, false, &tsk, &pid);
    assert_clean_rollback("FPU alloc", rc);
    assert_true(rc == -ENOMEM);
    assert_eq(0, mock_arch_files_live);
    assert_eq(0, mock_arch_fpu_live);
    assert_true(tsk == NULL);
    assert_eq(0, pid);
    assert_true(spy_kfree_calls >= 2);      /* thread + raw stack released */
    assert_retry_succeeds("retry after FPU alloc");

    TEST_SUITE("rollback: arch resource prepare");
    switch_tail_reset();
    boot_phase_cpu0_up();
    mock_arch_prepare_rc = -ENOMEM;
    tsk = (task_t *)1;
    pid = 5;
    rc = kthread_alloc_and_publish(ctor_fn, 0, NULL, 0, false, &tsk, &pid);
    assert_clean_rollback("arch prepare", rc);
    assert_true(rc == -ENOMEM);
    assert_eq(1, mock_arch_prepare_calls);
    assert_true(tsk == NULL);
    assert_eq(0, pid);
    assert_true(spy_kfree_calls >= 2);     /* thread + raw stack released */
    assert_eq(0, mock_arch_fpu_live);       /* hook-owned FPU rollback */

    /* A later successful constructor owns a real mock FPU allocation;
     * exercise the actual common switch tail and verify it returns all
     * slab bytes and objects to the pre-attempt baseline. */
    mock_arch_prepare_rc = 0;
    size_t baseline_bytes = mock_kmalloc_live_bytes;
    task_t *retry = NULL;
    int retry_pid = 0;
    rc = kthread_alloc_and_publish(ctor_fn, 1, NULL, 0, false,
                                   &retry, &retry_pid);
    assert_eq(0, rc);
    assert_true(retry != NULL);
    assert_true(retry_pid > 0);
    assert_eq(1, mock_arch_fpu_live);
    list_del(&retry->list);
    retry->flags |= PF_SELF_REAP;
    arch_kthread_drop_files(retry);
    task_switch_complete(retry);
    assert_eq(0, mock_arch_fpu_live);
    assert_eq((int)baseline_bytes, (int)mock_kmalloc_live_bytes);
}

/* ── 3b. Publication lock order + enqueue handoff (P5 Task 4) ─── */
static void suite_publication_order(void)
{
    TEST_SUITE("publication takes task_list_lock then rq_lock, releases reverse");
    switch_tail_reset();
    boot_phase_cpu0_up();
    sched_boot_creation_begin();
    task_t *tsk = NULL;
    int pid = 0;
    assert_eq(0, kthread_alloc_and_publish(ctor_fn, 9, NULL, 0, false,
                                           &tsk, &pid));
    /* Exactly four lock operations: lock list, lock rq, unlock rq,
     * unlock list — the frozen publication order. */
    assert_eq(4, mock_lock_log_count);
    assert_true(mock_lock_log_addr[0] == &task_list_lock);
    assert_eq(1, mock_lock_log_is_lock[0]);
    assert_true(mock_lock_log_addr[1] == &percpu_data[0].rq_lock);
    assert_eq(1, mock_lock_log_is_lock[1]);
    assert_true(mock_lock_log_addr[2] == &percpu_data[0].rq_lock);
    assert_eq(0, mock_lock_log_is_lock[2]);
    assert_true(mock_lock_log_addr[3] == &task_list_lock);
    assert_eq(0, mock_lock_log_is_lock[3]);

    TEST_SUITE("publication hands the task to the runqueue exactly once");
    /* The on_cpu/on_rq handoff the host can see: exactly one enqueue
     * under the rq lock (no double enqueue, no refusal), TASK_RUNNING
     * at publication, and the remote-notification issued after the
     * locks drop.  (The full on_cpu/on_rq WAKE re-check logic lives in
     * sched/core.c and is proven live by the aarch64 blocker case's
     * duplicate_enqueue=0 evidence.) */
    assert_eq(1, mock_enqueue_calls);
    assert_eq(0, mock_enqueue_refused);
    assert_eq(1, mock_notify_remote_calls);
    assert_true(tsk->state == TASK_RUNNING);
    assert_true(tsk->on_rq);
}

/* ── 4. Pinned API never gets the exception ──────────────────── */
static void suite_pinned_api(void)
{
    TEST_SUITE("kernel_thread_on_cpu rejects a not-yet-scheduler CPU");
    switch_tail_reset();
    boot_phase_cpu0_up();
    sched_boot_creation_begin();           /* window open — irrelevant */

    assert_true(kernel_thread_on_cpu(ctor_fn, 0, 0) == -EINVAL);
    assert_true(kernel_thread_on_cpu(ctor_fn, 0, NR_CPUS) == -EINVAL);
    assert_true(kernel_thread_on_cpu(ctor_fn, 0, NR_CPUS + 5) == -EINVAL);
    num_cpus = 1;
    assert_true(kernel_thread_on_cpu(ctor_fn, 0, num_cpus + 1) == -EINVAL);
    percpu_data[0].online = 0;
    assert_true(kernel_thread_on_cpu(ctor_fn, 0, 0) == -EINVAL);
    percpu_data[0].online = 1;
    assert_eq(0, task_list_len());
    assert_eq(0, (int)percpu_data[0].nr_running);

    TEST_SUITE("failed pin then normal create");
    /* The failed pinned attempts must not poison the next creation. */
    int pid = kernel_thread(ctor_fn, 0, PF_KTHREAD);
    assert_true(pid >= 1);
    assert_eq(1, task_list_len());
    assert_eq(1, (int)percpu_data[0].nr_running);
}

/* ── 5. Empty affinity ───────────────────────────────────────── */
static void suite_empty_affinity(void)
{
    TEST_SUITE("empty affinity is rejected");
    switch_tail_reset();
    num_cpus = 0;                          /* default mask would be empty */
    percpu_data[0].online = 1;
    percpu_data[0].scheduler_ok = 0;
    assert_true(kernel_thread(ctor_fn, 0, PF_KTHREAD) == -EINVAL);
    assert_true(create_kthread(ctor_fn, 0, "e") == NULL);
    assert_eq(0, task_list_len());
    assert_eq(0, (int)percpu_data[0].nr_running);
}

/* ── 6. The window closes permanently at sched_boot_start() ──── */
static void suite_window_close_permanent(void)
{
    TEST_SUITE("sched_boot_start closes the window permanently");
    switch_tail_reset();
    boot_phase_cpu0_up();
    assert_true(sched_boot_creation_open());

    sched_boot_start();
    assert_true(!sched_boot_creation_open());
    assert_eq(1, (int)percpu_data[0].scheduler_ok);

    /* A later begin() must NOT reopen it... */
    sched_boot_creation_begin();
    assert_true(!sched_boot_creation_open());

    /* ...so even with scheduler_ok cleared again (CPU offline for the
     * picker), the boot fallback is gone forever: -EINVAL. */
    percpu_data[0].scheduler_ok = 0;
    assert_true(kernel_thread(ctor_fn, 0, PF_KTHREAD) == -EINVAL);
    assert_eq(0, task_list_len());
    assert_eq(0, (int)percpu_data[0].nr_running);
}

/* ── 7. Runtime creation + flags contract ────────────────────── */
static void suite_runtime_create(void)
{
    TEST_SUITE("runtime create with an eligible CPU");
    switch_tail_reset();
    num_cpus = 2;
    percpu_data[0].online = 1;
    percpu_data[0].scheduler_ok = 1;
    percpu_data[1].online = 1;             /* online but NOT scheduler_ok */
    percpu_data[1].scheduler_ok = 0;
    assert_true(!sched_boot_creation_open());

    /* unpinned: only CPU0 is eligible */
    int pid = kernel_thread(ctor_fn, 7, PF_KTHREAD);
    assert_true(pid >= 1);
    assert_eq(1, (int)percpu_data[0].nr_running);
    assert_eq(0, (int)percpu_data[1].nr_running);

    /* named create_kthread publishes the supplied name before
     * publication (not the creator's comm) */
    task_t *named = create_kthread(ctor_fn, 8, "lwip_test");
    assert_true(named != NULL);
    assert_true(strcmp(named->comm, "lwip_test") == 0);
    assert_eq(2, (int)percpu_data[0].nr_running);

    /* pinned to the scheduler-ready CPU works */
    int pinned = kernel_thread_on_cpu(ctor_fn, 0, 0);
    assert_true(pinned >= 1);
    assert_eq(3, (int)percpu_data[0].nr_running);

    /* pinned to the online-but-not-scheduler_ok CPU is -EINVAL */
    assert_true(kernel_thread_on_cpu(ctor_fn, 0, 1) == -EINVAL);
    assert_eq(3, (int)percpu_data[0].nr_running);
    assert_eq(0, (int)percpu_data[1].nr_running);

    TEST_SUITE("flags contract: only 0 and PF_KTHREAD");
    switch_tail_reset();
    num_cpus = 1;
    percpu_data[0].online = 1;
    percpu_data[0].scheduler_ok = 1;
    assert_true(kernel_thread(ctor_fn, 0, 0) >= 1);                 /* flags 0 */
    assert_true(kernel_thread(ctor_fn, 0, PF_KTHREAD) >= 1);        /* kthread */
    assert_true(kernel_thread(ctor_fn, 0, ~0ULL) == -EINVAL);       /* garbage */
    assert_true(kernel_thread(ctor_fn, 0, PF_SELF_REAP) == -EINVAL);
    assert_eq(2, task_list_len());         /* the two accepted shapes only */
    assert_true((size_t)0 != mock_kmalloc_live_bytes); /* they live until reset */
}

int main(void)
{
    suite_window_initially_closed();
    suite_boot_fallback();
    suite_rollback();
    suite_publication_order();
    suite_pinned_api();
    suite_empty_affinity();
    suite_window_close_permanent();
    suite_runtime_create();
    printf("\n%s: %d total, %d passed, %d failed\n", "test_kthread_ctor",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
