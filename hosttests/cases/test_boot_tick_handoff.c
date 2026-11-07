/* hosttests/cases/test_boot_tick_handoff.c — BOOT-2 Task 3.
 *
 * Drives the REAL kernel/arch/x86_64/platform/time.c (arch_tick_start
 * BSP handoff + freq cache) and the REAL lapic_timer.c (bounded
 * calibration + start) over the boot_irq_time register/TSC/IRQ mocks.
 *
 * Pinned (spec §5.2):
 *   - handoff runs in an IRQ-off region: entry IF 0 or 1 is restored;
 *   - mask PIT → try LAPIC → publish accounting EXACTLY once; on LAPIC
 *     success the PIT stays masked, on failure the LAPIC is left
 *     masked/stopped and the PIT is re-unmasked (false = valid fallback);
 *   - PIT not prepared → fatal before any accounting publish;
 *   - the LAPIC TSC window is bounded (stuck TSC → failure, not a hang);
 *   - lapic_timer_start rejects 0/overflow and fails masked;
 *   - the ÷2 decrement-rate semantics are unchanged (compat baseline).
 */
#include <unistd.h>
#include "test_framework.h"
#include <arch/irq.h>
#include <intr/interrupt.h>
#include <arch/x86_64/platform/boot_irq.h>
#include <arch/x86_64/irq_route.h>
#include <sys/wait.h>

#include <stdio.h>
#include <signal.h>
#include <setjmp.h>

/* production surfaces */
bool arch_tick_start(void);
void lapic_timer_calibrate(void);
bool lapic_timer_start(uint32_t freq_hz);
void lapic_timer_set_premeasured(uint64_t hz);
void lapic_timer_init(void);
int pit_init(void);
int pit_start(void);
bool pit_prepared(void);

/* mock surface */
extern int boot2_if_flag;
extern unsigned long boot2_irq_enable_calls;
extern unsigned long boot2_account_tick_calls;
extern int boot2_tsc_frozen;
extern int boot2_cpu_id15_answer;
uint32_t boot2_lapic_get(uint32_t offset);
void boot2_lapic_reset(void);
void boot2_imr_set(uint16_t port, uint8_t v);
uint8_t boot2_imr_get(uint16_t port);
void boot2_tsc_set(uint64_t v);
void boot2_percpu_reset(void);
void boot2_rtc_ctx_prepare(void);
unsigned boot2_num_cpus(void);
extern uint8_t boot2_cmos_regs[16];
extern int boot2_cmos_periodic;

/* test knobs (defined in the runtime, read by shadowed headers) */
extern uint64_t boot2_clocksource_freq;
extern uint64_t boot2_rdtscp_value;

/* wrap passthrough (link contract from Task 1; this suite drives the
 * real selection). */
hw_int_controller_t *__real_arch_irq_select_controller(uint32_t gsi);
hw_int_controller_t *__wrap_arch_irq_select_controller(uint32_t gsi)
{ return __real_arch_irq_select_controller(gsi); }

static void reset_mock(void)
{
    boot2_if_flag = 0;
    boot2_irq_enable_calls = 0;
    boot2_account_tick_calls = 0;
    boot2_tsc_frozen = 0;
    boot2_cpu_id15_answer = 0;
    boot2_clocksource_freq = 0;
    boot2_rdtscp_value = 0;
    boot2_imr_set(0x21, 0xff);
    boot2_imr_set(0xA1, 0xff);
    memset(boot2_cmos_regs, 0, sizeof(boot2_cmos_regs));
    boot2_cmos_periodic = 0;
    boot2_lapic_reset();
    boot2_percpu_reset();
    /* fresh percpu + context state for this child */
    struct irq_gsi_range r = { .base = 0, .count = 24 };
    irq_registry_init_ranges(&r, 1);
    x86_irq_routes_init(&r, 1);
    boot2_rtc_ctx_prepare();
}

static void handoff_success_lapic(void)
{
    TEST_SUITE("boot2 handoff: LAPIC success keeps PIT masked");
    reset_mock();
    assert_eq(0, pit_init());
    assert_eq(0, pit_start());          /* PIT is the running source */
    lapic_timer_set_premeasured(1000000);
    lapic_timer_calibrate();            /* premeasure path: hz ready */
    boot2_if_flag = 1;                  /* entry IF=1 must be restored */

    assert_eq(true, arch_tick_start());

    /* LAPIC running: LVT unmasked with periodic bit. */
    uint32_t lvt = boot2_lapic_get(0x320);
    assert_eq(0, lvt & (1u << 16));     /* unmasked */
    assert_true(lvt & (1u << 17));      /* periodic */
    /* PIT stayed masked through the handoff. */
    assert_eq(0x01, boot2_imr_get(0x21) & 0x01);
    /* Accounting published exactly once (the handoff, not a per-tick). */
    assert_eq(1, (int)boot2_account_tick_calls);
    assert_eq(1, boot2_if_flag);        /* entry IF restored */
}

static void handoff_fallback_pit(void)
{
    TEST_SUITE("boot2 handoff: LAPIC failure re-unmasks the PIT");
    reset_mock();
    assert_eq(0, pit_init());
    assert_eq(0, pit_start());
    /* No premeasure, clocksource=0 → TSC window skipped → uncalibrated
     * LAPIC → start refuses. */
    boot2_if_flag = 0;

    assert_eq(false, arch_tick_start());

    /* LAPIC stopped/masked after the failed attempt. */
    assert_true((boot2_lapic_get(0x320) & (1u << 16)) != 0);
    /* PIT is the valid fallback: unmasked, accounting published once. */
    assert_eq(0x00, boot2_imr_get(0x21) & 0x01);
    assert_eq(1, (int)boot2_account_tick_calls);
    assert_eq(0, boot2_if_flag);
}

static int fatal_hit;
static jmp_buf fatal_jb;
extern void (*boot2_kpanic_hook)(void);
static void fatal_jmp_back(void) { longjmp(fatal_jb, 1); }
void kpanic(const char *msg, ...);

static void handoff_no_pit_fatal(void)
{
    TEST_SUITE("boot2 handoff: no prepared PIT is fatal, no publish");
    reset_mock();
    /* pit_init deliberately NOT called. */
    lapic_timer_set_premeasured(0);
    boot2_clocksource_freq = 0;
    boot2_kpanic_hook = fatal_jmp_back;
    if (setjmp(fatal_jb) == 0) {
        fatal_hit = 1;
        arch_tick_start();
        fatal_hit = 0;                  /* no fatal: protocol violation */
    } else {
        fatal_hit = 2;                  /* fatal captured */
    }
    assert_eq(2, fatal_hit);
    assert_eq(0, (int)boot2_account_tick_calls);   /* never published */
}

static void calibrate_bounded_stuck_tsc(void)
{
    TEST_SUITE("boot2 handoff: stuck TSC ends within the pause budget");
    reset_mock();
    assert_eq(0, pit_init());
    /* TSC window path: clocksource gives a frequency, premeasure empty. */
    boot2_clocksource_freq = 1000000000;
    lapic_timer_set_premeasured(0);
    boot2_tsc_frozen = 1;               /* TSC never advances */

    /* The OLD unbounded loop hangs here; the bounded budget must return
     * with the LAPIC still uncalibrated (hz=0 → start refuses). */
    lapic_timer_calibrate();
    assert_eq(false, lapic_timer_start(100));
    assert_true((boot2_lapic_get(0x320) & (1u << 16)) != 0);
    boot2_tsc_frozen = 0;
}

static void start_rejects_zero_freq(void)
{
    TEST_SUITE("boot2 handoff: lapic_timer_start rejects freq=0");
    reset_mock();
    assert_eq(0, pit_init());
    lapic_timer_set_premeasured(1000000);
    lapic_timer_calibrate();
    /* A zero/overflowing argument must fail WITHOUT unmasking. */
    assert_eq(false, lapic_timer_start(0));
    assert_true((boot2_lapic_get(0x320) & (1u << 16)) != 0);
    /* The good path still works and leaves the timer unmasked. */
    assert_eq(true, lapic_timer_start(100));
    assert_eq(0, boot2_lapic_get(0x320) & (1u << 16));
}

static void div2_semantics_compat(void)
{
    TEST_SUITE("boot2 handoff: ÷2 decrement-rate semantics unchanged");
    reset_mock();
    assert_eq(0, pit_init());
    /* premeasure path: lapic_timer_hz == premeasured (÷2-folded). */
    lapic_timer_set_premeasured(1000000);
    lapic_timer_calibrate();
    assert_eq(true, lapic_timer_start(100));
    /* init_count = hz / freq = 10000. */
    assert_eq(10000u, boot2_lapic_get(0x380));
    assert_eq(0u, boot2_lapic_get(0x3E0));   /* divisor 0 = ÷2 preserved */
}

static void run_case(const char *name, void (*fn)(void))
{
    pid_t pid = fork();
    if (pid == 0) {
        alarm(10);
        setbuf(stdout, NULL);
        fn();
        fflush(stdout);
        _exit(__test_stats.failed ? 1 : 0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    printf("  [%s] %s%s\n", ok ? "PASS" : "FAIL", name,
           (!ok && WIFSIGNALED(st)) ? " (child signal/fatal)" : "");
    if (!ok) __test_stats.failed++;
    else __test_stats.passed++;
    __test_stats.total++;
}

int main(void)
{
    run_case("handoff_success_lapic", handoff_success_lapic);
    run_case("handoff_fallback_pit", handoff_fallback_pit);
    run_case("handoff_no_pit_fatal", handoff_no_pit_fatal);
    run_case("calibrate_bounded_stuck_tsc", calibrate_bounded_stuck_tsc);
    run_case("start_rejects_zero_freq", start_rejects_zero_freq);
    run_case("div2_semantics_compat", div2_semantics_compat);
    printf("\ntest_boot_tick_handoff: %d total, %d passed, %d failed\n",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
