/* test_sched_notify.c — P1 Task 1: the production cross-CPU notify
 * path and the blocker-scan admission policy (aarch64 AP cooperative
 * scheduler plan P1, Task 1 step 2).
 *
 * The REAL kernel/sched/core.c AND the REAL kernel/sched/balance.c are
 * host-compiled via ../mock/sched_notify_test_runtime.h; the arch
 * facade hooks are controllable spies in this TU (the REAL per-arch
 * backends are exercised by test_sched_notify_x86_backend.c and
 * test_sched_notify_aarch64_backend.c, which link those objects).
 *
 * Suites:
 *   1. local notify: no arch hook, no need_resched write.
 *   2. remote notify: need_resched published BEFORE the hook fires;
 *      the hook receives the target cpu.
 *   3. invalid target (>= NR_CPUS): refused, nothing published.
 *   4. not-ready target (beyond num_cpus / offline / scheduler_ok==0):
 *      refused, nothing published.
 *   5. blocker_wait with the scan policy closed: -EOPNOTSUPP, the
 *      condition callback is never read, no state is changed.
 *   6. schedule() with the scan policy closed: the scanner never runs
 *      (callback unread, task_list_lock untaken).
 *   7. schedule() with the scan policy open: the scanner wakes a
 *      blocked task on a remote CPU through the production
 *      blocker_wake -> task_wake path (need_resched published).
 *   8. the policy derives from cpu_id() when the arch does not pin it
 *      (aarch64 contract: BSP true, AP false).
 *   9. task_wake enqueues and publishes need_resched only for a remote
 *      target.
 *  10. task_finish_switch re-enqueues a wake-during-switch prev.
 *  11. blocker_wait with the policy open blocks and resumes through
 *      the REAL schedule().
 */

/* Selects the controllable arch-hook spy definitions in the mock
 * header (the backend binaries link the REAL backends instead and must
 * not define them).  Must be set BEFORE the mock include. */
#define SCHED_NOTIFY_TEST_COMMON_SPIES 1

#include "test_framework.h"
#include "../mock/sched_notify_test_runtime.h"
#include <errno.h>

static int cb_calls;
static bool cb_return;
static bool cb_check_blocker_type;

static bool check_cb(task_t *waiter)
{
    cb_calls++;
    if (cb_check_blocker_type && waiter->blocker.type == BLOCKER_NONE)
        return false;   /* emulate "callback needs an installed blocker" */
    return cb_return;
}

/* Build a blocked task fixture and hang it on the global list. */
static task_t blocked_fixture;
static void install_blocked_task(int cpu)
{
    memset(&blocked_fixture, 0, sizeof(blocked_fixture));
    blocked_fixture.state = TASK_INTERRUPTIBLE;
    blocked_fixture.pid = 42;
    blocked_fixture.cpu = (uint32_t)cpu;
    blocked_fixture.allowed_cpus = 1ULL << cpu;
    blocked_fixture.blocker.type = BLOCKER_WAITPID;
    blocked_fixture.blocker.check = check_cb;
    blocked_fixture.blocker.signal_can_wake = false;
    list_init(&blocked_fixture.list);
    list_add_to_before(&init_task_union.task.list, &blocked_fixture.list);
}

/* ── 1-4. notify admission + publication order ─────────────────── */
static void suite_notify_local_no_hook(void)
{
    TEST_SUITE("local notify publishes nothing and never calls the hook");
    sched_notify_reset();
    percpu_data[0].need_resched = 0;
    sched_notify_remote_cpu(0);
    assert_eq(0, mock_arch_notify_calls);
    assert_eq(0, (int)percpu_data[0].need_resched);
    assert_eq(0, mock_log_err_calls);
}

static void suite_notify_remote_order(void)
{
    TEST_SUITE("remote notify publishes need_resched before the arch hook");
    sched_notify_reset();
    num_cpus = 2;
    percpu_data[1].online = 1;
    percpu_data[1].scheduler_ok = 1;
    sched_notify_remote_cpu(1);
    assert_eq(1, mock_arch_notify_calls);
    assert_eq(1, (int)mock_arch_notified_cpu);
    /* The spy snapshots percpu_data[1].need_resched at hook-entry time:
     * it must already read 1 — publication precedes notification. */
    assert_eq(1, (int)mock_arch_notify_need_at_hook);
    assert_eq(1, (int)percpu_data[1].need_resched);
}

static void suite_notify_invalid(void)
{
    TEST_SUITE("invalid notify target is refused without publication");
    sched_notify_reset();
    sched_notify_remote_cpu(NR_CPUS);
    sched_notify_remote_cpu(NR_CPUS + 7);
    assert_eq(0, mock_arch_notify_calls);
    assert_eq(0, (int)percpu_data[0].need_resched);
    assert_true(mock_log_err_calls >= 2);
}

static void suite_notify_not_ready(void)
{
    TEST_SUITE("not-ready notify targets are refused without publication");
    sched_notify_reset();
    num_cpus = 2;

    /* beyond the live topology */
    percpu_data[3].online = 1;
    percpu_data[3].scheduler_ok = 1;
    sched_notify_remote_cpu(3);
    assert_eq(0, mock_arch_notify_calls);
    assert_eq(0, (int)percpu_data[3].need_resched);

    /* offline */
    percpu_data[1].online = 0;
    percpu_data[1].scheduler_ok = 1;
    sched_notify_remote_cpu(1);
    assert_eq(0, mock_arch_notify_calls);
    assert_eq(0, (int)percpu_data[1].need_resched);

    /* online but scheduler not started */
    percpu_data[1].online = 1;
    percpu_data[1].scheduler_ok = 0;
    sched_notify_remote_cpu(1);
    assert_eq(0, mock_arch_notify_calls);
    assert_eq(0, (int)percpu_data[1].need_resched);
    assert_true(mock_log_err_calls >= 3);
}

/* ── 5. blocker_wait ignores the scan policy since P2 ──────────── */
static void suite_blocker_wait_policy_closed(void)
{
    TEST_SUITE("blocker_wait works even with the scan policy closed (P2)");
    sched_notify_reset();
    mock_arch_scan_blockers = 0;    /* policy closed (a former AP) */
    cb_calls = 0;
    cb_return = true;               /* already-true condition */
    task_t sleeper;
    memset(&sleeper, 0, sizeof(sleeper));
    sleeper.state = TASK_RUNNING;
    sleeper.pid = 7;
    sleeper.cpu = 1;
    mock_sched_current = &sleeper;

    /* P2 (spec §5.2a): the SGI wakeup delivered the notification
     * cross-CPU, so the P1 refusal is gone — an already-true
     * condition returns 0 through the normal path. */
    int rc = blocker_wait(check_cb, BLOCKER_WAITPID, false);
    assert_eq(0, rc);
    assert_eq(1, cb_calls);                    /* callback consulted */
    assert_eq(TASK_RUNNING, sleeper.state);    /* state untouched */
    assert_eq(BLOCKER_NONE, sleeper.blocker.type);
    assert_true(sleeper.blocker.check == NULL);
}

/* ── 6-7. schedule()'s scan gate ───────────────────────────────── */
static void suite_schedule_scan_gate_closed(void)
{
    TEST_SUITE("schedule with the policy closed never runs the scanner");
    sched_notify_reset();
    mock_arch_scan_blockers = 0;
    cb_calls = 0;
    cb_return = true;
    install_blocked_task(1);
    /* current = idle on CPU0, scheduler started: schedule() self-paths. */
    mock_sched_current = &init_task_union.task;
    int list_ops = 0;
    for (int i = 0; i < mock_lock_log_count; i++)
        if (mock_lock_log_addr[i] == &task_list_lock)
            list_ops++;

    schedule();

    assert_eq(0, cb_calls);                       /* callback unread */
    assert_eq(TASK_INTERRUPTIBLE, blocked_fixture.state);
    assert_eq(BLOCKER_WAITPID, blocked_fixture.blocker.type);
    assert_eq(0, mock_enqueue_calls);
    for (int i = 0; i < mock_lock_log_count; i++)
        if (mock_lock_log_addr[i] == &task_list_lock)
            list_ops--;
    assert_eq(0, list_ops);                       /* scanner lock untaken */
}

static void suite_schedule_scan_gate_open(void)
{
    TEST_SUITE("schedule with the policy open wakes a blocked AP task");
    sched_notify_reset();
    num_cpus = 2;
    percpu_data[1].online = 1;
    percpu_data[1].scheduler_ok = 1;
    mock_arch_scan_blockers = 1;    /* policy open (x86 / aarch64 BSP) */
    cb_calls = 0;
    cb_return = true;
    install_blocked_task(1);
    mock_sched_current = &init_task_union.task;

    schedule();

    /* The scanner evaluates the callback twice per P1 production flow:
     * once in sched_unblock_blocked's pre-check and once inside
     * blocker_wake's condition verification (P2 Task 2 restructures
     * this protocol; P1 keeps it). */
    assert_eq(2, cb_calls);
    /* the production blocker_wake -> task_wake path enqueued it */
    assert_eq(1, mock_enqueue_calls);
    assert_true(mock_enqueue_last == &blocked_fixture);
    assert_true(mock_enqueue_rq == &percpu_data[1]);
    assert_true(blocked_fixture.on_rq);
    assert_eq(1, (int)percpu_data[1].need_resched);  /* remote publish */
    assert_eq(BLOCKER_NONE, blocked_fixture.blocker.type);
}

/* ── 8. policy derivation when the arch does not pin it ────────── */
static void suite_policy_derives_from_cpu_id(void)
{
    TEST_SUITE("scan policy derives from cpu_id when unpinned");
    sched_notify_reset();
    mock_arch_scan_blockers = -1;   /* derive from cpu_id() */
    assert_eq(0, mock_scan_blockers_calls);
    assert_true(arch_sched_scan_blockers() == true);   /* BSP (cpu 0) */
    assert_eq(1, mock_scan_blockers_calls);
    mock_sched_this_cpu = &percpu_data[2];
    assert_true(arch_sched_scan_blockers() == false);  /* AP (cpu 2) */
    assert_eq(2, mock_scan_blockers_calls);
}

/* ── 9. task_wake publication ──────────────────────────────────── */
static void suite_task_wake_publication(void)
{
    TEST_SUITE("task_wake enqueues once and publishes remote need_resched");
    sched_notify_reset();
    num_cpus = 2;
    percpu_data[1].online = 1;
    percpu_data[1].scheduler_ok = 1;
    task_t w;
    memset(&w, 0, sizeof(w));
    w.state = TASK_UNINTERRUPTIBLE;
    w.pid = 9;
    w.cpu = 1;
    w.allowed_cpus = 2ULL;

    task_wake(&w);
    assert_eq(1, mock_enqueue_calls);
    assert_true(mock_enqueue_last == &w);
    assert_eq(1, (int)percpu_data[1].need_resched);
    assert_eq(TASK_RUNNING, w.state);
    assert_eq(1, (int)w.cpu);

    /* local target: enqueue, no need_resched write */
    sched_notify_reset();
    w.cpu = 0;
    w.allowed_cpus = 1ULL;
    w.on_rq = false;
    w.state = TASK_UNINTERRUPTIBLE;
    task_wake(&w);
    assert_eq(1, mock_enqueue_calls);
    assert_eq(0, (int)percpu_data[0].need_resched);
}

/* ── 10. finish-switch re-enqueue of a wake-during-switch prev ─── */
static void suite_finish_switch_reenqueue(void)
{
    TEST_SUITE("task_finish_switch re-enqueues prev and publishes need");
    sched_notify_reset();
    num_cpus = 2;
    percpu_data[1].online = 1;
    percpu_data[1].scheduler_ok = 1;
    task_t prev;
    memset(&prev, 0, sizeof(prev));
    prev.state = TASK_RUNNING;    /* woken while switching out */
    prev.pid = 11;
    prev.cpu = 1;
    prev.allowed_cpus = 2ULL;
    prev.on_rq = false;
    prev.on_cpu = 1;              /* its stack is still in use */

    task_finish_switch(&prev);
    assert_eq(1, mock_enqueue_calls);
    assert_true(mock_enqueue_last == &prev);
    assert_eq(1, (int)percpu_data[1].need_resched);
    assert_eq(0, (int)prev.on_cpu);

    /* a prev already re-enqueued (on_rq) is not double-enqueued */
    sched_notify_reset();
    prev.on_rq = true;
    prev.on_cpu = 1;
    task_finish_switch(&prev);
    assert_eq(0, mock_enqueue_calls);
    assert_eq(0, (int)percpu_data[0].need_resched);
}

/* ── 11. blocker_wait normal flow with the policy open ─────────── */
static void suite_blocker_wait_open_flow(void)
{
    TEST_SUITE("blocker_wait with the policy open blocks and resumes");
    sched_notify_reset();
    mock_arch_scan_blockers = 1;
    cb_calls = 0;
    cb_return = true;
    task_t sleeper;
    memset(&sleeper, 0, sizeof(sleeper));
    sleeper.state = TASK_RUNNING;
    sleeper.pid = 13;
    sleeper.cpu = 0;
    sleeper.allowed_cpus = 1ULL;
    sleeper.thread = &init_thread;
    mock_sched_current = &sleeper;

    /* condition already true: returns 0 without ever calling schedule() */
    assert_eq(0, blocker_wait(check_cb, BLOCKER_WAITPID, false));
    assert_eq(1, cb_calls);
    assert_eq(TASK_RUNNING, sleeper.state);
    assert_eq(BLOCKER_NONE, sleeper.blocker.type);

    /* condition false: installs the blocker, schedules, resumes clean.
     * The real schedule() self-paths (current is a plain RUNNING task;
     * the scanner never finds a matching entry because the fixture list
     * holds only the idle task). */
    cb_return = false;
    assert_eq(0, blocker_wait(check_cb, BLOCKER_WAITPID, false));
    assert_eq(3, cb_calls);   /* initial + double-check + resume re-check */
    assert_eq(TASK_RUNNING, sleeper.state);
    assert_eq(BLOCKER_NONE, sleeper.blocker.type);
    assert_true(sleeper.in_schedule == 0);
}

int main(void)
{
    suite_notify_local_no_hook();
    suite_notify_remote_order();
    suite_notify_invalid();
    suite_notify_not_ready();
    suite_blocker_wait_policy_closed();
    suite_schedule_scan_gate_closed();
    suite_schedule_scan_gate_open();
    suite_policy_derives_from_cpu_id();
    suite_task_wake_publication();
    suite_finish_switch_reenqueue();
    suite_blocker_wait_open_flow();
    printf("\n%s: %d total, %d passed, %d failed\n", "test_sched_notify",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
