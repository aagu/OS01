/* test_aarch64_sched_tick.c — aarch64 per-CPU tick accounting:
 * Task 1 (frozen CNTP configuration + local timer start) and Task 2
 * (per-CPU scheduler tick accounting).
 *
 * The REAL production objects are host-compiled via
 * ../mock/aarch64_sched_tick_runtime.h:
 *
 *   kernel/arch/aarch64/platform/time.c   (the CNTP backend + the ISR)
 *   kernel/arch/aarch64/cpu/sched_tick.c  (the per-CPU scheduler tick)
 *   kernel/sched/fair.c                    (update_curr)
 *   kernel/sched/accounting.c              (the per-CPU usage counters)
 *
 * The mock replaces ONLY the machine / GIC / DTB / IRQ / CPU-identity
 * boundaries.  Every assertion below is about the PRODUCTION algorithm:
 * the configuration validation, the publish/acquire protocol, the arm
 * order, the ready / started / admission gates, the exact busy/idle
 * accounting increments and the tick-driven vruntime charge.
 *
 * The fixture delivers a REAL CNTP interrupt by calling the handler
 * that the production backend registered with the (mocked) GIC — the
 * ISR under test is the production ISR.
 *
 * CASE ORDERING (load-bearing):
 *   - `timer_local_arm` must stay FIRST: the production configuration
 *     is published exactly once per process and cannot be
 *     unpublished again, so its "nothing published yet" precondition
 *     only holds before the first successful publish.
 *   - `early_tick_never_reads_current` and the first half of
 *     `start_requires_admission_idle_and_init` must run BEFORE
 *     aarch64_sched_tick_init(): the global ready gate is a one-shot
 *     publication and cannot be closed again.
 */

#define AARCH64_SCHED_TICK_HOST_TU 1

#include "test_framework.h"
#include "../mock/aarch64_sched_tick_runtime.h"

/* ── Fixture state ────────────────────────────────────────────── */
static task_t g_idle[4];
static task_t g_busy;

/* ── Trace helpers ───────────────────────────────────────────── */
static int trace_count_op(enum aarch64_timer_op op)
{
    int n = 0;
    for (int i = 0; i < aarch64_timer_trace_count && i < AARCH64_TIMER_TRACE_MAX; i++) {
        if (aarch64_timer_trace[i].op == op)
            n++;
    }
    return n;
}

static int trace_find_op(enum aarch64_timer_op op, int from)
{
    for (int i = from; i < aarch64_timer_trace_count && i < AARCH64_TIMER_TRACE_MAX; i++) {
        if (aarch64_timer_trace[i].op == op)
            return i;
    }
    return -1;
}

static int trace_find_last_op(enum aarch64_timer_op op)
{
    for (int i = aarch64_timer_trace_count - 1; i >= 0; i--) {
        if (aarch64_timer_trace[i].op == op)
            return i;
    }
    return -1;
}

static bool log_contains(const char *needle)
{
    for (int i = 0; i < aarch64_timer_log_count && i < AARCH64_TIMER_LOG_MAX; i++) {
        if (strstr(aarch64_timer_log_buf[i], needle) != NULL)
            return true;
    }
    return false;
}

/* ── Task 1: the local arm ───────────────────────────────────── */

static void timer_local_arm(void)
{
    aarch64_timer_mock_reset();

    /* (a) Nothing published yet: the local start must refuse and must
     *     not touch the counter at all. */
    assert_false(aarch64_timer_start_local());
    assert_eq(0, aarch64_timer_mock_register_calls);
    assert_eq(0, trace_count_op(AARCH64_TIMER_OP_TVAL_WRITE));
    assert_eq(0, trace_count_op(AARCH64_TIMER_OP_CTL_WRITE));

    /* (b) A published configuration: the local start arms THIS CPU's
     *     counter and never registers a GIC handler — the BSP owns the
     *     global handler slot. */
    aarch64_timer_mock_reset();
    aarch64_timer_mock_freq = 62500000ULL;
    aarch64_timer_mock_ppi = 30U;
    assert_true(aarch64_timer_publish_config());
    assert_eq(1, aarch64_timer_mock_register_calls);
    assert_eq(30, aarch64_timer_mock_register_intid);

    aarch64_timer_trace_count = 0;
    assert_true(aarch64_timer_start_local());
    assert_eq(1, aarch64_timer_mock_register_calls);   /* still only the publish */

    /* Arm order: TVAL -> CTL -> ISB -> read-back. */
    int i_tval = trace_find_op(AARCH64_TIMER_OP_TVAL_WRITE, 0);
    int i_ctlw = trace_find_op(AARCH64_TIMER_OP_CTL_WRITE, 0);
    int i_isb  = trace_find_op(AARCH64_TIMER_OP_ISB, 0);
    int i_ctlr = trace_find_last_op(AARCH64_TIMER_OP_CTL_READ);
    assert_true(i_tval >= 0);
    assert_true(i_ctlw > i_tval);
    assert_true(i_isb > i_ctlw);
    assert_true(i_ctlr > i_isb);

    /* The frozen period is programmed and the counter runs unmasked. */
    assert_eq(625000, aarch64_timer_mock_tval);
    assert_eq(1, (int)(aarch64_timer_mock_ctl & 1UL));            /* ENABLE */
    assert_eq(0, (int)(aarch64_timer_mock_ctl & (1UL << 1)));      /* IMASK  */

    /* (c) A local counter frequency that differs from the published one
     *     is a hard failure: the two CPUs' samples would not be
     *     comparable, so the counter must not be armed. */
    aarch64_timer_trace_count = 0;
    aarch64_timer_mock_freq = 62500001ULL;
    assert_false(aarch64_timer_start_local());
    assert_eq(0, trace_count_op(AARCH64_TIMER_OP_TVAL_WRITE));
    assert_eq(0, trace_count_op(AARCH64_TIMER_OP_CTL_WRITE));
}

/* ── Task 1: the configuration validation ────────────────────── */

static void timer_bad_config(void)
{
    struct {
        uint64_t freq;
        uint32_t ppi;
        bool     ok;
    } cases[] = {
        { 0ULL,                                     30U, false }, /* no frequency   */
        { 99ULL,                                    30U, false }, /* below 100 Hz  */
        { 100ULL * ((uint64_t)INT32_MAX + 1ULL),   30U, false }, /* period overflow*/
        { 62500000ULL,                              15U, false }, /* SGI, not a PPI */
        { 62500000ULL,                              32U, false }, /* SPI, not a PPI */
        { 62500000ULL,                              16U, true  }, /* lowest PPI     */
        { 62500000ULL,                              31U, true  }, /* highest PPI    */
        { 100ULL,                                   30U, true  }, /* the nominal rate*/
        { 62500000ULL,                              30U, true  }, /* QEMU virt      */
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        aarch64_timer_mock_reset();
        aarch64_timer_mock_freq = cases[i].freq;
        aarch64_timer_mock_ppi = cases[i].ppi;

        bool published = aarch64_timer_publish_config();
        if (cases[i].ok) {
            assert_true(published);
            assert_eq(1, aarch64_timer_mock_register_calls);
            assert_eq(cases[i].ppi, aarch64_timer_mock_register_intid);
            /* The published period is exactly freq / 100. */
            assert_true(aarch64_timer_start_local());
            assert_eq(cases[i].freq / 100, aarch64_timer_mock_tval);
        } else {
            /* A rejected configuration must not register a handler and
             * must not touch the counter. */
            assert_false(published);
            assert_eq(0, aarch64_timer_mock_register_calls);
            assert_eq(0, trace_count_op(AARCH64_TIMER_OP_TVAL_WRITE));
            assert_eq(0, trace_count_op(AARCH64_TIMER_OP_CTL_WRITE));
        }
    }
}

/* ── Task 1: the handler registration happens once ───────────── */

static void timer_register_once(void)
{
    aarch64_timer_mock_reset();
    aarch64_timer_mock_freq = 62500000ULL;
    aarch64_timer_mock_ppi = 30U;

    /* The BSP start registers the CNTP handler exactly once and arms
     * its own counter. */
    assert_true(arch_tick_start());
    assert_eq(1, aarch64_timer_mock_register_calls);
    assert_eq(30, aarch64_timer_mock_register_intid);
    assert_eq(625000, aarch64_timer_mock_tval);

    /* The "[cntp]" protocol line is byte-identical to the pre-existing
     * one (the aarch64 log backend adds no prefix). */
    assert_true(log_contains("[cntp] freq=62500000 Hz, period=625000 ticks (100 Hz)\n"));

    /* A second BSP start hits the "already registered" conflict (-2).
     * The GIC handler slot is global, so this is a HARD failure — the
     * old code tolerated an unknown repeat registration. */
    int tval_writes = trace_count_op(AARCH64_TIMER_OP_TVAL_WRITE);
    assert_false(arch_tick_start());
    assert_eq(2, aarch64_timer_mock_register_calls);
    assert_eq(tval_writes, trace_count_op(AARCH64_TIMER_OP_TVAL_WRITE));
}

/* ── Task 1: a failed arm leaves the counter disabled ────────── */

static void timer_failed_arm_disables(void)
{
    /* (a) A read-back that does not show ENABLE is a failed arm: the
     *     start fails AND the counter is left disabled. */
    aarch64_timer_mock_reset();
    aarch64_timer_mock_freq = 62500000ULL;
    aarch64_timer_mock_ppi = 30U;
    aarch64_timer_mock_readback_fails = 1;
    assert_false(arch_tick_start());
    assert_eq(1, aarch64_timer_mock_register_calls);
    assert_eq(0, (int)(aarch64_timer_mock_ctl & 1UL));   /* ENABLE cleared */

    /* (b) A CTL that refuses to clear IMASK is a failed arm too: the
     *     tick would never be delivered. */
    aarch64_timer_mock_reset();
    aarch64_timer_mock_freq = 62500000ULL;
    aarch64_timer_mock_ppi = 30U;
    assert_true(aarch64_timer_publish_config());
    aarch64_timer_mock_imask_sticky = 1;
    assert_false(aarch64_timer_start_local());
    assert_eq(0, (int)(aarch64_timer_mock_ctl & 1UL));   /* ENABLE cleared */
}

/* ══ Task 2 ════════════════════════════════════════════════════ */

/* Publish the frozen configuration and arm the BSP counter through
 * the production boot path; prepare the per-CPU metadata. */
static void task2_prepare(void)
{
    aarch64_timer_mock_reset();
    mock_accepted_mask = 0x3ULL;          /* CPU0 + CPU1 admitted */
    aarch64_timer_mock_freq = 62500000ULL;
    aarch64_timer_mock_ppi = 30U;
    assert_true(arch_tick_start());

    for (uint32_t c = 0; c < 2; c++) {
        memset(&g_idle[c], 0, sizeof(g_idle[c]));
        g_idle[c].pid = 0;
        g_idle[c].cpu = c;
        percpu_data[c].cpu_id = c;
        percpu_data[c].online = 1;
        percpu_data[c].idle = &g_idle[c];
        __atomic_store_n(&percpu_data[c].scheduler_ok, 1, __ATOMIC_RELEASE);
    }
    mock_sched_this_cpu = &percpu_data[0];
}

/* Start the local scheduler tick on both CPUs. */
static void task2_start_ticks(void)
{
    mock_sched_this_cpu = &percpu_data[0];
    assert_true(aarch64_sched_tick_start());
    mock_sched_this_cpu = &percpu_data[1];
    assert_true(aarch64_sched_tick_start());
}

/* ── Task 2: an early tick never reads `current` ──────────────── */

static void early_tick_never_reads_current(void)
{
    aarch64_timer_mock_reset();
    mock_sched_this_cpu = &percpu_data[0];
    percpu_data[0].cpu_id = 0;
    mock_current_poisoned = 1;

    /* Before the initialization the tick is completely inert: it must
     * not read `current` and must not touch the accounting locks (they
     * do not exist yet). */
    aarch64_sched_tick();
    assert_eq(0, mock_current_reads);
    assert_eq(0, mock_lock_ops);

    /* The start before the initialization fails the same way. */
    assert_false(aarch64_sched_tick_start());
    assert_eq(0, mock_current_reads);
    assert_eq(0, mock_lock_ops);

    mock_current_poisoned = 0;
}

/* ── Task 2: the start gates ──────────────────────────────────── */

static void start_requires_admission_idle_and_init(void)
{
    task2_prepare();

    /* (a) Before the initialization the start fails without touching
     *     the accounting locks or `current`. */
    mock_sched_this_cpu = &percpu_data[0];
    assert_false(aarch64_sched_tick_start());
    assert_eq(0, mock_lock_ops);
    assert_eq(0, mock_current_reads);

    /* (b) Initialize. */
    aarch64_sched_tick_init();

    /* (c) A CPU outside the admission mask is refused — and it must
     *     not arm the counter. */
    int tval_before = trace_count_op(AARCH64_TIMER_OP_TVAL_WRITE);
    percpu_data[2].cpu_id = 2;
    percpu_data[2].idle = &g_idle[2];
    mock_sched_this_cpu = &percpu_data[2];
    assert_false(aarch64_sched_tick_start());
    assert_eq(tval_before, trace_count_op(AARCH64_TIMER_OP_TVAL_WRITE));
    assert_eq(0, mock_current_reads);

    /* (d) An admitted CPU whose idle task is not published yet is
     *     refused as well. */
    percpu_data[1].idle = NULL;
    mock_sched_this_cpu = &percpu_data[1];
    assert_false(aarch64_sched_tick_start());
    assert_eq(tval_before, trace_count_op(AARCH64_TIMER_OP_TVAL_WRITE));
    assert_eq(0, mock_current_reads);

    /* (e) With the idle task published the start succeeds and the AP
     *     arms its OWN counter. */
    percpu_data[1].idle = &g_idle[1];
    assert_true(aarch64_sched_tick_start());
    assert_eq(tval_before + 1, trace_count_op(AARCH64_TIMER_OP_TVAL_WRITE));
}

/* ── Task 2: the exact busy / idle increments ─────────────────── */

static void busy7_idle5(void)
{
    task2_prepare();
    task2_start_ticks();

    memset(&g_busy, 0, sizeof(g_busy));
    g_busy.pid = 4242;
    g_busy.deadline = 1000;      /* far away: no deadline request */
    mock_sched_this_cpu = &percpu_data[1];
    mock_sched_current = &g_busy;

    for (int i = 0; i < 7; i++)
        aarch64_sched_tick();

    struct cpu_usage_snapshot snap;
    assert_eq(0, sched_accounting_read(1, &snap));
    assert_eq(7, snap.busy_ticks);
    assert_eq(0, snap.idle_ticks);
    assert_eq(7, g_busy.runtime_ticks);
    assert_eq(7, g_busy.vruntime);

    /* Five idle ticks: the idle task is never charged. */
    mock_sched_current = &g_idle[1];
    for (int i = 0; i < 5; i++)
        aarch64_sched_tick();

    assert_eq(0, sched_accounting_read(1, &snap));
    assert_eq(7, snap.busy_ticks);
    assert_eq(5, snap.idle_ticks);
    assert_eq(7, g_busy.runtime_ticks);
    assert_eq(7, g_busy.vruntime);
    assert_eq(0, g_idle[1].runtime_ticks);
    assert_eq(0, g_idle[1].vruntime);

    /* The usage lock and the runqueue lock are never nested. */
    assert_eq(0, mock_lock_nesting_violations);
    assert_eq(0, mock_assert_failures);
}

/* ── Task 2: the deadline request and the pending flag ────────── */

static void deadline_and_pending(void)
{
    task2_prepare();
    task2_start_ticks();

    memset(&g_busy, 0, sizeof(g_busy));
    g_busy.pid = 4243;
    g_busy.deadline = 3;
    mock_sched_this_cpu = &percpu_data[1];
    mock_sched_current = &g_busy;
    percpu_data[1].need_resched = 0;

    /* Before the deadline no reschedule is requested. */
    aarch64_sched_tick();
    assert_eq(0, (int)percpu_data[1].need_resched);
    aarch64_sched_tick();
    assert_eq(0, (int)percpu_data[1].need_resched);

    /* The deadline is reached: the request is published. */
    aarch64_sched_tick();
    assert_eq(1, (int)percpu_data[1].need_resched);

    /* A pending request is never cleared by the tick. */
    aarch64_sched_tick();
    assert_eq(1, (int)percpu_data[1].need_resched);
    assert_eq(4, g_busy.vruntime);
}

/* ── Task 2: the BSP scans, the AP does not ───────────────────── */

static void bsp_idle_requests_scan_ap_idle_does_not(void)
{
    task2_prepare();
    task2_start_ticks();

    /* The BSP: an idle tick still requests the cooperative scan, so an
     * empty runqueue still reaches the blocker scan. */
    mock_sched_this_cpu = &percpu_data[0];
    mock_sched_current = &g_idle[0];
    percpu_data[0].need_resched = 0;
    aarch64_sched_tick();
    assert_eq(1, (int)percpu_data[0].need_resched);

    /* The AP: an idle tick must NOT request a schedule. */
    mock_sched_this_cpu = &percpu_data[1];
    mock_sched_current = &g_idle[1];
    percpu_data[1].need_resched = 0;
    aarch64_sched_tick();
    assert_eq(0, (int)percpu_data[1].need_resched);
}

/* ── Task 2: a repeated start is idempotent ───────────────────── */

static void lifecycle_idempotent(void)
{
    task2_prepare();
    task2_start_ticks();

    struct cpu_usage_snapshot before, after;
    assert_eq(0, sched_accounting_read(1, &before));
    int tval = trace_count_op(AARCH64_TIMER_OP_TVAL_WRITE);
    int ctlw = trace_count_op(AARCH64_TIMER_OP_CTL_WRITE);

    assert_true(aarch64_sched_tick_start());   /* the repeated start */

    assert_eq(0, sched_accounting_read(1, &after));
    assert_eq(tval, trace_count_op(AARCH64_TIMER_OP_TVAL_WRITE));  /* no rearm */
    assert_eq(ctlw, trace_count_op(AARCH64_TIMER_OP_CTL_WRITE));  /* no control write */
    assert_eq(before.epoch, after.epoch);                        /* no epoch move */
    assert_eq(before.busy_ticks, after.busy_ticks);              /* no reset */
    assert_eq(before.idle_ticks, after.idle_ticks);
}

/* ── Task 2: only the BSP advances the global clock ───────────── */

static void global_tick_cpu0_only(void)
{
    task2_prepare();
    task2_start_ticks();

    /* Deliver a REAL CNTP interrupt through the registered handler. */
    assert_not_null(aarch64_timer_mock_handler);

    memset(&g_busy, 0, sizeof(g_busy));
    g_busy.pid = 4244;
    g_busy.deadline = 1000;
    mock_sched_current = &g_busy;

    int global_before = aarch64_timer_mock_tick_calls;
    mock_sched_this_cpu = &percpu_data[0];
    uint64_t wd0 = percpu_data[0].watchdog_counter;
    aarch64_timer_mock_handler(30, aarch64_timer_mock_handler_param, NULL);
    assert_eq(global_before + 1, aarch64_timer_mock_tick_calls);
    assert_eq(wd0 + 1, percpu_data[0].watchdog_counter);

    int global_after = aarch64_timer_mock_tick_calls;
    mock_sched_this_cpu = &percpu_data[1];
    uint64_t wd1 = percpu_data[1].watchdog_counter;
    aarch64_timer_mock_handler(30, aarch64_timer_mock_handler_param, NULL);
    /* The AP never advances the global time base ... */
    assert_eq(global_after, aarch64_timer_mock_tick_calls);
    /* ... but it does advance its own watchdog counter. */
    assert_eq(wd1 + 1, percpu_data[1].watchdog_counter);
}

TEST_LIST_BEGIN
    TEST_ENTRY(timer_local_arm),
    TEST_ENTRY(timer_bad_config),
    TEST_ENTRY(timer_register_once),
    TEST_ENTRY(timer_failed_arm_disables),
    TEST_ENTRY(early_tick_never_reads_current),
    TEST_ENTRY(start_requires_admission_idle_and_init),
    TEST_ENTRY(busy7_idle5),
    TEST_ENTRY(deadline_and_pending),
    TEST_ENTRY(bsp_idle_requests_scan_ap_idle_does_not),
    TEST_ENTRY(lifecycle_idempotent),
    TEST_ENTRY(global_tick_cpu0_only),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed != 0;
}
