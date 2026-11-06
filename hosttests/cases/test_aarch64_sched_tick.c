/* test_aarch64_sched_tick.c — aarch64 per-CPU tick accounting,
 * Task 1: the frozen CNTP configuration + the local timer start.
 *
 * The REAL kernel/arch/aarch64/platform/time.c is host-compiled via
 * ../mock/aarch64_sched_tick_runtime.h, which replaces ONLY the
 * machine / GIC / DTB / IRQ / log boundaries.  Every assertion below
 * is about the PRODUCTION algorithm:
 *
 *   - the counter-frequency / period / PPI validation,
 *   - the frozen-configuration publish (fields first, ready last) and
 *     its acquire on the local start,
 *   - the TVAL -> CTL -> ISB -> read-back arm order,
 *   - the ENABLE-clearing failure path,
 *   - the "the GIC handler slot is global and owned by the BSP"
 *     contract: the per-CPU entry point never registers a handler and
 *     a registration conflict is a hard failure.
 *
 * CASE ORDERING (load-bearing): the production configuration is
 * published exactly once per process and cannot be unpublished again,
 * so the "nothing published yet" precondition of the first case only
 * holds before the first successful publish.  timer_local_arm must
 * therefore stay the FIRST case of this binary.
 */

#include "test_framework.h"
#include "../mock/aarch64_sched_tick_runtime.h"

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

/* ── Case 1: the local arm ───────────────────────────────────── */

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

/* ── Case 2: the configuration validation ────────────────────── */

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

/* ── Case 3: the handler registration happens once ───────────── */

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

/* ── Case 4: a failed arm leaves the counter disabled ────────── */

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

TEST_LIST_BEGIN
    TEST_ENTRY(timer_local_arm),
    TEST_ENTRY(timer_bad_config),
    TEST_ENTRY(timer_register_once),
    TEST_ENTRY(timer_failed_arm_disables),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed != 0;
}
