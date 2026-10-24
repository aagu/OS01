/*
 * test_monitor_accounting.c — per-CPU tick accounting
 *
 * Monitor plan Task 2 / monitor design spec §2
 * (docs/superpowers/specs/2026-10-09-monitor-design.md):
 *   - 100 idle samples + 100 busy samples → busy=idle=100 and the busy
 *     task's runtime_ticks == 100 (idle tasks never accumulate runtime);
 *   - ticks before the CPU's scheduler_ok are dropped;
 *   - a CPU whose timer start was published started=false reads
 *     valid=0 (unavailable, never 0%);
 *   - repeated reads copy without mutating;
 *   - a real stop / re-online cycle changes the epoch and resets the
 *     counters; re-publishing the same running parameters does not;
 *   - the production BSP route (LAPIC fail → PIT success publish →
 *     duplicate per-cpu-callback publish) ends valid=1 with the epoch
 *     unchanged;
 *   - an AP whose lapic_timer_start failed reads valid=0;
 *   - legal unenumerated CPU slots read online=0; enumerated slots
 *     follow the percpu online flag without resetting saved accounting;
 *   - the usage locks are really taken (irqsave spin path observed via
 *     poll_test_spinlock_acquire) and the hot path allocates nothing;
 *   - read-side counter copies occur between acquisition and release
 *     of the tick writer's lock, excluding post-release writer ticks.
 *
 * Build: the PRODUCTION kernel/sched/accounting.c is host-compiled via
 * accounting_test_runtime.h (test-controlled cpu_id()/percpu_data[]/
 * current); everything asserted here runs against that object.
 *
 * State note: the production usage[] array is static and persists
 * across sub-tests (there is no reset API), so every test works on its
 * own CPU id or reuses one with DELTAS around the operations under test.
 * CPU map: 0 = busy/idle counting, 4 = scheduler-not-ready,
 * 5 = BSP PIT-fallback route, 6 = stop/restart epoch, 7 = duplicate
 * publish, 1 = failed-AP unavailable. Online-boundary tests reuse 7
 * after an explicit timer stop/start, and read 2 without changing usage.
 */

#include "test_framework.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sched/accounting.h>
/* Included explicitly because the test TU does not preinclude it; the
 * production TU gets it via -include on the Makefile rule (pattern of
 * test_tlb_serial_protocol.c). */
#include "../mock/accounting_test_runtime.h"

/* ── Control surface (declared in accounting_test_runtime.h) ── */

percpu_t percpu_data[NR_CPUS];
uint32_t num_cpus = NR_CPUS;
volatile uint32_t accounting_test_cpu;
task_t *accounting_test_current;

/* ── Allocation tripwire: the hot path must not allocate ── */

static unsigned long test_alloc_count;
static unsigned long test_free_count;

void *kmalloc(size_t size)
{
    test_alloc_count++;
    return malloc(size ? size : 1);
}
size_t kfree(void *ptr)
{
    test_free_count++;
    free(ptr);
    return 0;
}

/* ── irqsave observation: production must take the usage locks ── */

static unsigned long test_lock_acquires;
static void observe_accounting_acquire(spinlock_T *lock);
static void observe_accounting_release(spinlock_T *lock);

void poll_test_spinlock_acquire(spinlock_T *lock)
{
    test_lock_acquires++;
    observe_accounting_acquire(lock);
}

void poll_test_spinlock_release(spinlock_T *lock)
{
    observe_accounting_release(lock);
}

/* ── Fixture helpers ── */

static task_t fixture_idle;
static task_t fixture_busy;

static void reset_fixture_cpu(unsigned cpu)
{
    memset(&percpu_data[cpu], 0, sizeof(percpu_data[cpu]));
    percpu_data[cpu].cpu_id = cpu;
    percpu_data[cpu].online = 1;
    percpu_data[cpu].scheduler_ok = 1;
    memset(&fixture_idle, 0, sizeof(fixture_idle));
    memset(&fixture_busy, 0, sizeof(fixture_busy));
    percpu_data[cpu].idle = &fixture_idle;
    accounting_test_cpu = cpu;
    accounting_test_current = &fixture_idle;
    test_lock_acquires = 0;
}

/* N ticks with `cur` as the interrupted task. */
static void run_ticks(unsigned n, task_t *cur)
{
    accounting_test_current = cur;
    for (unsigned i = 0; i < n; i++)
        sched_account_tick();
}

/* Mode 1 identifies the real tick writer's lock. Mode 2 injects ticks
 * at read lock boundaries. Nested writer calls run with tracing off. */
static int observe_read_mode;
static spinlock_T *tick_writer_lock;
static unsigned read_acquires, read_releases, read_depth, read_lock_errors;

static void observe_accounting_acquire(spinlock_T *lock)
{
    if (observe_read_mode == 1) {
        tick_writer_lock = lock;
        return;
    }
    if (observe_read_mode != 2)
        return;
    if (lock != tick_writer_lock || read_depth != 0)
        read_lock_errors++;
    read_acquires++;
    read_depth++;
    observe_read_mode = 0;
    run_ticks(2, &fixture_busy);
    run_ticks(1, &fixture_idle);
    observe_read_mode = 2;
}

static void observe_accounting_release(spinlock_T *lock)
{
    if (observe_read_mode != 2)
        return;
    if (lock != tick_writer_lock || read_depth != 1)
        read_lock_errors++;
    read_releases++;
    if (read_depth)
        read_depth--;
    observe_read_mode = 0;
    run_ticks(4, &fixture_busy);
    run_ticks(6, &fixture_idle);
    observe_read_mode = 2;
}

/* ── Tests ──────────────────────────────────────────── */

/* 100 idle samples + 100 busy samples → busy=idle=100; the busy task
 * accumulated runtime_ticks == 100 and the idle task 0 (spec §2.2).
 * Idle detection is by POINTER comparison with the CPU's idle task. */
TEST_FUNC(busy_idle_and_runtime_accounting) {
    reset_fixture_cpu(0);
    sched_accounting_init();
    sched_accounting_cpu_start(0, 100, true);   /* tick source running */

    struct cpu_usage_snapshot before, after;
    assert_eq(0, sched_accounting_read(0, &before));

    run_ticks(100, &fixture_idle);   /* current == idle  → idle sample */
    run_ticks(100, &fixture_busy);   /* current != idle  → busy sample */

    assert_eq(0, sched_accounting_read(0, &after));
    assert_eq(100, (int)(after.busy_ticks - before.busy_ticks));
    assert_eq(100, (int)(after.idle_ticks - before.idle_ticks));
    assert_eq((uint64_t)100, fixture_busy.runtime_ticks);
    assert_eq((uint64_t)0, fixture_idle.runtime_ticks);
    assert_eq(100, (int)after.tick_hz);
    assert_eq(1, (int)after.valid);
    assert_eq(1, (int)after.online);
}

/* Ticks observed before the CPU's scheduler is up leave no trace: no
 * busy/idle increments and no task runtime, even though the timer and
 * accounting are valid. */
TEST_FUNC(scheduler_not_ready_drops_ticks) {
    reset_fixture_cpu(4);
    sched_accounting_init();
    sched_accounting_cpu_start(4, 100, true);
    percpu_data[4].scheduler_ok = 0;   /* scheduler not yet on this CPU */

    struct cpu_usage_snapshot before, after;
    assert_eq(0, sched_accounting_read(4, &before));

    run_ticks(10, &fixture_busy);
    assert_eq((uint64_t)0, fixture_busy.runtime_ticks);

    percpu_data[4].scheduler_ok = 1;   /* scheduler comes up */
    run_ticks(1, &fixture_busy);

    assert_eq(0, sched_accounting_read(4, &after));
    assert_eq(0, (int)(after.busy_ticks - before.busy_ticks -
                       1));   /* only the post-ok tick counted */
    assert_eq(0, (int)(after.idle_ticks - before.idle_ticks));
    assert_eq((uint64_t)1, fixture_busy.runtime_ticks);
}

/* started=false publishes an unavailable CPU: valid=0 and the tick
 * hot path drops samples instead of counting them. */
TEST_FUNC(timer_not_started_reports_invalid) {
    reset_fixture_cpu(1);
    sched_accounting_init();
    sched_accounting_cpu_start(1, 100, false);   /* ap_entry saw failure */

    struct cpu_usage_snapshot snap;
    assert_eq(0, sched_accounting_read(1, &snap));
    assert_eq(0, (int)snap.valid);
    assert_eq(1, (int)snap.online);

    run_ticks(5, &fixture_busy);
    assert_eq((uint64_t)0, fixture_busy.runtime_ticks);

    struct cpu_usage_snapshot after;
    assert_eq(0, sched_accounting_read(1, &after));
    assert_eq(0, (int)(after.busy_ticks - snap.busy_ticks));
    assert_eq(0, (int)(after.idle_ticks - snap.idle_ticks));
}

/* Repeated reads return identical snapshots and never mutate the
 * counters or the task runtime. */
TEST_FUNC(repeated_read_is_non_mutating) {
    reset_fixture_cpu(2);
    sched_accounting_init();
    sched_accounting_cpu_start(2, 100, true);
    run_ticks(7, &fixture_busy);

    struct cpu_usage_snapshot a, b;
    assert_eq(0, sched_accounting_read(2, &a));
    assert_eq(0, sched_accounting_read(2, &b));
    assert_eq((uint64_t)a.busy_ticks, b.busy_ticks);
    assert_eq((uint64_t)a.idle_ticks, b.idle_ticks);
    assert_eq((uint64_t)a.epoch, b.epoch);
    assert_eq((int)a.tick_hz, (int)b.tick_hz);
    assert_eq((int)a.online, (int)b.online);
    assert_eq((int)a.valid, (int)b.valid);

    struct cpu_usage_snapshot c;
    run_ticks(0, &fixture_busy);   /* no new ticks in between */
    assert_eq(0, sched_accounting_read(2, &c));
    assert_eq((uint64_t)a.busy_ticks, c.busy_ticks);
    assert_eq((uint64_t)a.idle_ticks, c.idle_ticks);
    assert_eq((uint64_t)7, fixture_busy.runtime_ticks);
}

/* A real stop / re-online cycle is a NEW lifecycle each time: the
 * epoch moves and the counters restart from zero. */
TEST_FUNC(stop_and_restart_changes_epoch) {
    reset_fixture_cpu(6);
    sched_accounting_init();
    sched_accounting_cpu_start(6, 100, true);
    run_ticks(3, &fixture_busy);

    struct cpu_usage_snapshot running;
    assert_eq(0, sched_accounting_read(6, &running));
    assert_eq(1, (int)running.valid);

    /* CPU goes down (or its timer start failed on re-entry). */
    sched_accounting_cpu_start(6, 100, false);
    struct cpu_usage_snapshot stopped;
    assert_eq(0, sched_accounting_read(6, &stopped));
    assert_eq(0, (int)stopped.valid);
    assert_true(stopped.epoch != running.epoch);

    /* Re-online: new lifecycle → epoch moves again, counters reset. */
    sched_accounting_cpu_start(6, 100, true);
    struct cpu_usage_snapshot reborn;
    assert_eq(0, sched_accounting_read(6, &reborn));
    assert_eq(1, (int)reborn.valid);
    assert_true(reborn.epoch != stopped.epoch);
    assert_eq((uint64_t)0, reborn.busy_ticks);
    assert_eq((uint64_t)0, reborn.idle_ticks);
}

/* Re-publishing the SAME running parameters is idempotent: the epoch
 * stays put and the live counters are not reset. */
TEST_FUNC(duplicate_publish_keeps_epoch) {
    reset_fixture_cpu(7);
    sched_accounting_init();
    sched_accounting_cpu_start(7, 100, true);
    run_ticks(3, &fixture_busy);

    struct cpu_usage_snapshot first;
    assert_eq(0, sched_accounting_read(7, &first));

    sched_accounting_cpu_start(7, 100, true);   /* same lifecycle again */

    struct cpu_usage_snapshot second;
    assert_eq(0, sched_accounting_read(7, &second));
    assert_eq((uint64_t)first.epoch, second.epoch);
    assert_eq(1, (int)second.valid);
    assert_eq((uint64_t)first.busy_ticks, second.busy_ticks);
    assert_eq((uint64_t)first.idle_ticks, second.idle_ticks);
}

/* Production BSP route: LAPIC start fails → arch_tick_start falls back
 * to the enabled PIT and publishes valid=1; the (duplicate) per-cpu
 * callback publish afterwards must not change the epoch.  The LAPIC
 * bool alone must never be mistaken for "no tick source". */
TEST_FUNC(bsp_pit_fallback_route_keeps_epoch) {
    reset_fixture_cpu(5);
    sched_accounting_init();
    percpu_data[5].scheduler_ok = 0;   /* still booting */

    /* arch_tick_start(): lapic_timer_start() failed → PIT fallback
     * publishes the BSP lifecycle unconditionally. */
    sched_accounting_cpu_start(5, 100, true);

    struct cpu_usage_snapshot pit;
    assert_eq(0, sched_accounting_read(5, &pit));
    /* Source is live (hz published), but valid still requires this
     * CPU's scheduler_ok — which is down "while booting". */
    assert_eq(0, (int)pit.valid);
    assert_eq(100, (int)pit.tick_hz);
    assert_eq(1, (int)pit.online);

    percpu_data[5].scheduler_ok = 1;   /* scheduler comes up */
    run_ticks(2, &fixture_busy);

    /* The BSP per-cpu subsys callback re-publishes the same params. */
    sched_accounting_cpu_start(5, 100, true);

    struct cpu_usage_snapshot after_cb;
    assert_eq(0, sched_accounting_read(5, &after_cb));
    assert_eq(1, (int)after_cb.valid);   /* PIT fallback is a valid source */
    assert_eq((uint64_t)pit.epoch, after_cb.epoch);   /* epoch unchanged */
    assert_eq((uint64_t)2, after_cb.busy_ticks - pit.busy_ticks);
}

/* Invalid ids are rejected, not read out-of-bounds. */
TEST_FUNC(read_rejects_invalid_cpu) {
    sched_accounting_init();
    struct cpu_usage_snapshot snap;
    assert_eq(-EINVAL, sched_accounting_read(NR_CPUS, &snap));
    assert_eq(-EINVAL, sched_accounting_read((unsigned)-1, &snap));
    assert_eq(-EINVAL, sched_accounting_read(0, NULL));
}

/* A legal slot outside the enumerated CPU range is readable but offline,
 * even if its percpu online flag is stale. Reading must not reset usage. */
TEST_FUNC(read_unenumerated_cpu_reports_offline) {
    const unsigned cpu = NR_CPUS - 1;
    const uint32_t saved_num_cpus = num_cpus;
    reset_fixture_cpu(cpu);
    sched_accounting_init();
    sched_accounting_cpu_start(cpu, 100, false);
    sched_accounting_cpu_start(cpu, 100, true);
    run_ticks(3, &fixture_busy);
    run_ticks(2, &fixture_idle);

    struct cpu_usage_snapshot before;
    assert_eq(0, sched_accounting_read(cpu, &before));
    assert_true(before.epoch > 0);

    const struct {
        uint32_t count;
        uint32_t want_online;
    } cases[] = {
        {1, 0},                 /* strictly above num_cpus */
        {NR_CPUS - 1, 0},       /* exactly at num_cpus */
        {NR_CPUS, 1},           /* last slot becomes enumerated */
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        num_cpus = cases[i].count;
        struct cpu_usage_snapshot out;
        memset(&out, 0xa5, sizeof(out));
        assert_eq(0, sched_accounting_read(cpu, &out));
        assert_eq(cases[i].want_online, out.online);
        assert_eq((uint64_t)3, out.busy_ticks);
        assert_eq((uint64_t)2, out.idle_ticks);
        assert_eq(before.epoch, out.epoch);
        assert_eq(100, out.tick_hz);
        assert_eq(1, out.valid);  /* timer validity is independent */
    }
    num_cpus = saved_num_cpus;
}

/* Below num_cpus, online must follow the actual percpu flag rather than
 * enumeration or the timer's independent valid bit. */
TEST_FUNC(read_enumerated_cpu_tracks_online_flag) {
    const unsigned cpu = 2;
    reset_fixture_cpu(cpu);
    sched_accounting_init();
    sched_accounting_cpu_start(cpu, 100, true);
    struct cpu_usage_snapshot before, out;
    assert_eq(0, sched_accounting_read(cpu, &before));
    assert_eq(1, before.online);
    assert_eq(1, before.valid);

    percpu_data[cpu].online = 0;
    assert_eq(0, sched_accounting_read(cpu, &out));
    assert_eq(0, out.online);
    assert_eq(1, out.valid);
    assert_eq(before.busy_ticks, out.busy_ticks);
    assert_eq(before.idle_ticks, out.idle_ticks);
    assert_eq(before.epoch, out.epoch);
    assert_eq(before.tick_hz, out.tick_hz);

    percpu_data[cpu].online = 1;
    assert_eq(0, sched_accounting_read(cpu, &out));
    assert_eq(1, out.online);
    assert_eq(before.epoch, out.epoch);
}

/* The accounting hot path takes the per-CPU usage lock via the irqsave
 * spin path and performs zero allocations. */
TEST_FUNC(hot_path_locks_and_never_allocates) {
    reset_fixture_cpu(3);
    sched_accounting_init();
    sched_accounting_cpu_start(3, 100, true);

    test_lock_acquires = 0;
    unsigned long alloc_before = test_alloc_count;
    unsigned long free_before = test_free_count;

    run_ticks(10, &fixture_busy);

    struct cpu_usage_snapshot snap;
    assert_eq(0, sched_accounting_read(3, &snap));

    /* Every accounted tick took the usage lock once, every read took
     * it once (irqsave spin path → poll_test_spinlock_acquire). */
    assert_eq(11, (int)test_lock_acquires);
    assert_eq(alloc_before, test_alloc_count);
    assert_eq(free_before, test_free_count);
}

/* Without an idle identity no tick can be classified. Do not fabricate
 * busy time or runtime, erase old counters, or restart the timer epoch. */
TEST_FUNC(missing_idle_drops_ticks_and_recovers) {
    reset_fixture_cpu(0);
    sched_accounting_init();
    sched_accounting_cpu_start(0, 100, true);
    run_ticks(3, &fixture_busy);
    run_ticks(2, &fixture_idle);

    struct cpu_usage_snapshot before, after;
    assert_eq(0, sched_accounting_read(0, &before));
    uint64_t busy_runtime = fixture_busy.runtime_ticks;
    uint64_t idle_runtime = fixture_idle.runtime_ticks;
    unsigned long alloc_before = test_alloc_count;
    unsigned long free_before = test_free_count;
    unsigned long locks_before = test_lock_acquires;
    percpu_data[0].idle = NULL;

    /* Cover a non-idle current, the real idle task before its pointer is
     * published, and an absent current. None has a classifiable sample. */
    run_ticks(10, &fixture_busy);
    run_ticks(10, &fixture_idle);
    run_ticks(10, NULL);
    assert_eq(locks_before, test_lock_acquires);
    assert_eq(0, sched_accounting_read(0, &after));
    assert_eq(before.busy_ticks, after.busy_ticks);
    assert_eq(before.idle_ticks, after.idle_ticks);
    assert_eq(before.epoch, after.epoch);
    assert_eq(before.tick_hz, after.tick_hz);
    assert_eq(busy_runtime, fixture_busy.runtime_ticks);
    assert_eq(idle_runtime, fixture_idle.runtime_ticks);

    percpu_data[0].idle = &fixture_idle;
    run_ticks(1, &fixture_busy);
    run_ticks(1, &fixture_idle);
    assert_eq(0, sched_accounting_read(0, &after));
    assert_eq(before.busy_ticks + 1, after.busy_ticks);
    assert_eq(before.idle_ticks + 1, after.idle_ticks);
    assert_eq(before.epoch, after.epoch);
    assert_eq(before.tick_hz, after.tick_hz);
    assert_eq(1, after.valid);
    assert_eq(busy_runtime + 1, fixture_busy.runtime_ticks);
    assert_eq(idle_runtime, fixture_idle.runtime_ticks);
    assert_eq(alloc_before, test_alloc_count);
    assert_eq(free_before, test_free_count);
}

/* The result must include pre-acquire writer ticks and exclude
 * post-release ticks, using the very same lock as the tick writer. */
TEST_FUNC(read_copies_counters_inside_tick_writer_lock) {
    reset_fixture_cpu(0);
    sched_accounting_init();
    sched_accounting_cpu_start(0, 100, false);
    sched_accounting_cpu_start(0, 100, true);
    tick_writer_lock = NULL;
    observe_read_mode = 1;
    run_ticks(1, &fixture_busy);
    observe_read_mode = 0;
    assert_not_null(tick_writer_lock);
    run_ticks(2, &fixture_busy);
    run_ticks(2, &fixture_idle);

    struct cpu_usage_snapshot before, out, after;
    assert_eq(0, sched_accounting_read(0, &before));
    read_acquires = read_releases = read_depth = read_lock_errors = 0;
    observe_read_mode = 2;
    int rc = sched_accounting_read(0, &out);
    observe_read_mode = 0;
    assert_eq(0, rc);
    assert_eq((uint64_t)5, out.busy_ticks);
    assert_eq((uint64_t)3, out.idle_ticks);
    assert_eq(before.epoch, out.epoch);
    assert_eq(100, out.tick_hz);
    assert_eq(1, out.online);
    assert_eq(1, out.valid);
    assert_eq(1, read_acquires);
    assert_eq(1, read_releases);
    assert_eq(0, read_depth);
    assert_eq(0, read_lock_errors);

    assert_eq(0, sched_accounting_read(0, &after));
    assert_eq((uint64_t)9, after.busy_ticks);
    assert_eq((uint64_t)9, after.idle_ticks);
    assert_eq(before.epoch, after.epoch);
    assert_eq((uint64_t)9, fixture_busy.runtime_ticks);
}

TEST_LIST_BEGIN
    TEST_ENTRY(busy_idle_and_runtime_accounting),
    TEST_ENTRY(scheduler_not_ready_drops_ticks),
    TEST_ENTRY(timer_not_started_reports_invalid),
    TEST_ENTRY(repeated_read_is_non_mutating),
    TEST_ENTRY(stop_and_restart_changes_epoch),
    TEST_ENTRY(duplicate_publish_keeps_epoch),
    TEST_ENTRY(bsp_pit_fallback_route_keeps_epoch),
    TEST_ENTRY(read_rejects_invalid_cpu),
    TEST_ENTRY(read_unenumerated_cpu_reports_offline),
    TEST_ENTRY(read_enumerated_cpu_tracks_online_flag),
    TEST_ENTRY(hot_path_locks_and_never_allocates),
    TEST_ENTRY(missing_idle_drops_ticks_and_recovers),
    TEST_ENTRY(read_copies_counters_inside_tick_writer_lock),
TEST_LIST_END

int main(void) {
    for (int i = 0; i < __test_table_size; i++) __test_table[i].fn();
    int failed = __test_stats.failed;
    TEST_RESULTS();
    return failed != 0;
}
