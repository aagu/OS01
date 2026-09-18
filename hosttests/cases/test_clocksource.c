/* hosttests/cases/test_clocksource.c — aarch64 Timer Task 1.1 RED
 *
 * Why this test exists:
 *   The aarch64 Generic Timer (CNTP/CNTVCT) is registered into the GIC
 *   handler table at 100 Hz and `cntp_tick_handler` writes TVAL +
 *   `g_ticks` directly — but the unified kernel time framework
 *   (kernel/time/clocksource.c + kernel/time/tick.c) is not yet wired
 *   to aarch64. Before the GREEN step (`SUBSYS_INITCALL` for aarch64 +
 *   `cntp_tick_handler` -> `tick_handler()`) we lock down the
 *   contract of the time framework with host-side tests, against the
 *   REAL production kernel/time/clocksource.c.
 *
 * Suites:
 *   A. clocksource_compute_mult_shift algorithm — the freq -> (shift,mult)
 *      table. Hardcoded expected values from running the algorithm
 *      itself in a reference program; if the production algorithm
 *      drifts (e.g. someone changes the 1e9 constant), these fail.
 *   B. clocksource_init() — driven via the mock arch_cycle_freq; verifies
 *      that init reads the freq, computes mult/shift, sets
 *      clocksource_active=true, and stores clocksource_freq_hz() ==
 *      mocked value.
 *   C. clocksource_read_ns() against prepared mult/shift — verifies the
 *      inline (cycle + tsc_offset) * mult >> shift formula. Uses
 *      mocked arch_cycle_counter; tsc_offset is 0 (BSP).
 *   D. freq=0 fallback — when arch_cycle_freq returns 0, init must set
 *      clocksource_active=false and clocksource_freq_hz() returns 0;
 *      read_ns() falls back to jiffies*10ms.
 *
 * NOTE on mock surface: arch_cycle_freq / arch_cycle_counter are
 * `static inline` in the production <arch/cpu.h>. The host compile
 * short-circuits that header via clocksource_test_runtime.h and links
 * against extern symbols in clocksource_test_stubs.c, which read
 * host_mock_cycle_freq / host_mock_cycle_counter. Tests write those
 * globals directly to drive behaviour.
 */
#include <test_framework.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* clocksource_test_runtime.h is -included by the Makefile rule, BEFORE
 * any kernel header, so it can short-circuit `_ARCH_CPU_H` etc. Pull
 * the production API surface here. */
#include <time/clocksource.h>
#include <time/clocksource_internal.h>

/* ──────────────────────────────────────────────────────────────
 *  Suite A: clocksource_compute_mult_shift algorithm
 * ────────────────────────────────────────────────────────────── */
static void suite_compute_mult_shift(void)
{
    TEST_SUITE("compute_mult_shift algorithm");

    struct {
        uint64_t freq;
        uint32_t want_shift;
        uint32_t want_mult;
    } cases[] = {
        { 1000000ULL,        22, 4194304000u },  /* 1 MHz */
        { 62500000ULL,       27, 2147483648u },  /* 62.5 MHz (QEMU virt CNTP) */
        { 100000000ULL,      28, 2684354560u },  /* 100 MHz */
        { 1000000000ULL,     31, 2147483648u },  /* 1 GHz */
        { 2994000000ULL,     33, 2869049629u },  /* 2.994 GHz (typical TSC) */
        { 10000000ULL,       25, 3355443200u },  /* 10 MHz — sanity */
    };

    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        uint32_t got_shift = 0, got_mult = 0;
        clocksource_compute_mult_shift(cases[i].freq, &got_mult, &got_shift);
        if (got_shift != cases[i].want_shift) {
            printf("  [FAIL] freq=%llu shift got=%u want=%u\n",
                   (unsigned long long)cases[i].freq,
                   got_shift, cases[i].want_shift);
            __test_stats.failed++;
        } else {
            __test_stats.passed++;
        }
        __test_stats.total++;
        if (got_mult != cases[i].want_mult) {
            printf("  [FAIL] freq=%llu mult got=%u want=%u\n",
                   (unsigned long long)cases[i].freq,
                   got_mult, cases[i].want_mult);
            __test_stats.failed++;
        } else {
            __test_stats.passed++;
        }
        __test_stats.total++;
    }

    /* Invariant: for any freq in [1e6, 1e10] the formula
     * `(1e9 << shift) / freq == mult` must round-trip. With 32-bit
     * truncation, the inverse error is bounded. */
    for (unsigned i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        uint32_t s = 0, m = 0;
        clocksource_compute_mult_shift(cases[i].freq, &m, &s);
        uint64_t back = ((uint64_t)m * cases[i].freq) >> s; /* ns per freq-cycle */
        /* ns * freq_hz must approximate 1e9 within ~1% (mult is 32-bit). */
        if (back == 0) {
            __test_stats.total++;
            __test_stats.failed++;
            printf("  [FAIL] freq=%llu round-trip zero\n",
                   (unsigned long long)cases[i].freq);
        } else {
            uint64_t ratio = (back > 1000000000ULL) ? (back / 1000000000ULL)
                                                   : (1000000000ULL / back);
            assert_true(ratio <= 2); /* ≤ 2x off (cheap upper bound) */
        }
    }
}

/* ──────────────────────────────────────────────────────────────
 *  Suite B: clocksource_init() + arch_cycle_freq mock
 * ────────────────────────────────────────────────────────────── */
static void suite_clocksource_init(void)
{
    TEST_SUITE("clocksource_init with mocked arch_cycle_freq");

    /* Reset module state. */
    clocksource_active = false;
    clocksource_mult = clocksource_shift = 0;

    /* Case 1: QEMU virt CNTP default (62.5 MHz). */
    host_mock_cycle_freq = 62500000ULL;
    clocksource_init();
    assert_eq(true,  (int)clocksource_active);
    assert_eq((int)62500000ULL, (int)clocksource_freq_hz());
    assert_eq((int)2147483648u, (int)clocksource_mult);
    assert_eq((int)27u,         (int)clocksource_shift);

    /* Case 2: 1 GHz. */
    host_mock_cycle_freq = 1000000000ULL;
    clocksource_init();
    assert_eq(true, (int)clocksource_active);
    assert_eq((int)1000000000ULL, (int)clocksource_freq_hz());
    assert_eq((int)2147483648u, (int)clocksource_mult);
    assert_eq((int)31u,         (int)clocksource_shift);

    /* Case 3: 2.994 GHz (typical TSC). */
    host_mock_cycle_freq = 2994000000ULL;
    clocksource_init();
    assert_eq(true, (int)clocksource_active);
    assert_eq((int)2994000000ULL, (int)clocksource_freq_hz());
    assert_eq((int)33u, (int)clocksource_shift);

    /* Case 4: freq=0 → init must NOT activate (Suite D covers the
     * inactive branch in more depth, but assert the basic invariant). */
    host_mock_cycle_freq = 0;
    clocksource_init();
    assert_eq(false, (int)clocksource_active);
    assert_eq((int)0, (int)clocksource_freq_hz());
}

/* ──────────────────────────────────────────────────────────────
 *  Suite C: clocksource_read_ns() against prepared mult/shift
 * ────────────────────────────────────────────────────────────── */
static void suite_clocksource_read_ns(void)
{
    TEST_SUITE("clocksource_read_ns (active path)");

    /* Setup: 1 GHz, BSP (tsc_offset = 0). mult = 2^31, shift = 31. */
    clocksource_active = true;
    clocksource_mult  = 2147483648u;  /* 2^31 */
    clocksource_shift = 31;
    host_mock_cycle_freq = 1000000000ULL;
    host_mock_cycle_counter = 0;

    /* At cycle=0, ns should be 0. */
    host_mock_cycle_counter = 0;
    assert_eq(0, (int)clocksource_read_ns());

    /* At cycle=1e9, ns should be ~1e9 (1 second at 1 GHz). With 32-bit
     * mult the formula gives exactly 1e9 ns. */
    host_mock_cycle_counter = 1000000000ULL;
    uint64_t ns = clocksource_read_ns();
    assert_true(ns == 1000000000ULL);

    /* At cycle=2.5e8, ns should be ~2.5e8. */
    host_mock_cycle_counter = 250000000ULL;
    ns = clocksource_read_ns();
    assert_true(ns >= 249000000ULL && ns <= 251000000ULL);

    /* At cycle=1e6 (1ms at 1GHz), ns = 1e6. */
    host_mock_cycle_counter = 1000000ULL;
    ns = clocksource_read_ns();
    assert_true(ns >= 999000 && ns <= 1001000);

    /* tsc_offset contribution: simulate AP (tsc_offset nonzero).
     * Our host percpu has tsc_offset = 0; the suite relies on
     * percpu_data[0].tsc_offset being writable. */
    percpu_data[0].tsc_offset = 500;   /* bsp_tsc - ap_tsc */
    host_mock_cycle_counter = 1000;
    /* effective_cycle = 1500, ns = 1500 (shift=31 mult, 1 GHz) */
    ns = clocksource_read_ns();
    assert_true(ns >= 1499 && ns <= 1501);
    percpu_data[0].tsc_offset = 0;
}

static void suite_clocksource_read_ns_inactive(void)
{
    TEST_SUITE("clocksource_read_ns (inactive -> jiffies)");

    clocksource_active = false;
    jiffies = 0;
    assert_eq(0, (int)clocksource_read_ns());

    jiffies = 100;            /* 100 ticks * 10 ms = 1 s = 1e9 ns */
    assert_eq((int)(100ULL * 10000000ULL), (int)clocksource_read_ns());

    jiffies = 12345;
    assert_eq((int)(12345ULL * 10000000ULL), (int)clocksource_read_ns());

    /* Restore for any later suites. */
    jiffies = 0;
    clocksource_active = true;
}

/* ──────────────────────────────────────────────────────────────
 *  Suite D: freq=0 fallback (deeper than Suite B case 4)
 * ────────────────────────────────────────────────────────────── */
static void suite_freq_zero_fallback(void)
{
    TEST_SUITE("freq=0 fallback");

    clocksource_active = true;          /* pre-activate to prove init clears it */
    clocksource_mult  = 0xDEADBEEFu;
    clocksource_shift = 31;
    host_mock_cycle_freq = 0;

    clocksource_init();

    assert_eq(false, (int)clocksource_active);
    assert_eq((int)0, (int)clocksource_freq_hz());
    /* mult/shift may be left as-is (init returns early); the contract
     * is only that active=false and freq=0. */

    /* Re-establish active state for downstream tests. */
    host_mock_cycle_freq = 62500000ULL;
    clocksource_init();
    assert_eq(true, (int)clocksource_active);
}

/* ──────────────────────────────────────────────────────────────
 * Suite E — aarch64 clocksource integration contract
 *
 * Locks the framework values main.c's explicit clocksource_init() call
 * (Timer Task 1.2) will produce on aarch64 once the kernel is
 * built.  QEMU virt default CNTP freq is 62.5 MHz; the math is the
 * same as Suite A's 62.5 MHz row, but this is a separate contract on
 * the post-init globals (active / freq_hz / mult / shift) and on
 * read_ns roundtripping.
 *
 * RED→GREEN plan: these asserts are the GREEN hook; they were not in
 * the original commit b6ac05f.
 * ────────────────────────────────────────────────────────────── */
static void suite_aarch64_integration(void)
{
    TEST_SUITE("aarch64 clocksource integration contract");

    host_mock_cycle_freq = 62500000ULL;   /* QEMU virt CNTP freq */
    clocksource_init();
    assert_eq(true, (int)clocksource_active);
    assert_eq((int)62500000ULL, (int)clocksource_freq_hz());
    assert_eq((int)0x80000000u, (int)clocksource_mult);    /* 2^31 */
    assert_eq((int)27u, (int)clocksource_shift);

    /* read_ns at cycle=62500000 → exactly 1e9 ns (1 second). */
    host_mock_cycle_counter = 62500000ULL;
    assert_eq((int)1000000000ULL, (int)clocksource_read_ns());

    /* read_ns at cycle=625000 → exactly 10 000 000 ns (10 ms = 1 jiffy @ 100 Hz).
     * Math: 625000 * 2^31 >> 27 = 625000 × 16 = 10 000 000 ns. */
    host_mock_cycle_counter = 625000ULL;
    assert_eq((int)10000000ULL, (int)clocksource_read_ns());

    /* Monotonicity across two reads. */
    host_mock_cycle_counter = 1ULL;
    uint64_t ns_a = clocksource_read_ns();
    host_mock_cycle_counter = 1000ULL;
    uint64_t ns_b = clocksource_read_ns();
    assert_true(ns_b > ns_a);
}

/* ────────────────────────────────────────────────────────────── */
int main(void)
{
    suite_compute_mult_shift();
    suite_clocksource_init();
    suite_clocksource_read_ns();
    suite_clocksource_read_ns_inactive();
    suite_freq_zero_fallback();
    suite_aarch64_integration();

    printf("\n%s: %d total, %d passed, %d failed\n", "test_clocksource",
             __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
