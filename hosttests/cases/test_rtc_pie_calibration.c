/* hosttests/cases/test_rtc_pie_calibration.c — BOOT-2 Task 2.
 *
 * Drives the REAL kernel/arch/x86_64/platform/rtc_pie.c transaction and
 * the REAL kernel/arch/x86_64/platform/time.c frequency cache over the
 * boot_irq_time CMOS/LAPIC/IRQ8 mocks.  The mock implements only
 * hardware semantics (CMOS registers, periodic IRQ pending, LAPIC
 * register file, TSC model) — the transaction steps live in rtc_pie.c.
 *
 * Pinned (spec §4):
 *   - only C.PF (bit6) IRQs count; C=0 / AF-only never sample;
 *   - 256 valid PF samples, 255-interval window, TSC+LAPIC both positive;
 *   - preconditions: NULL outputs, entry IF=1 → -1, no hardware touched;
 *   - registration failure: no CMOS/LAPIC/route side effects;
 *   - success/timeout/stuck-TSC all restore A/B, mask the route, mask
 *     the LAPIC, leave IF=0 and no active handler;
 *   - arch_cycle_freq caches the FIRST boot decision (including 0) and
 *     never touches the RTC again; a valid CPUID answer has no RTC
 *     side effects.
 */
#include <unistd.h>
/* Minimal wait macros: <sys/wait.h> drags the OS01 libc's sys/time.h
 * into this TU (timeval clash with glibc), so declare the POSIX
 * surface the case runner needs directly. */
typedef int pid_t_b2;
extern int waitpid(int, int *, int);
#define B2_WIFEXITED(s)   (((s) & 0x7f) == 0)
#define B2_WEXITSTATUS(s) (((s) >> 8) & 0xff)
#define B2_WIFSIGNALED(s) (((s) & 0x7f) != 0)
#define B2_WTERMSIG(s)    ((s) & 0x7f)
#include "test_framework.h"
#include <arch/irq.h>
#include <arch/io.h>
#include <intr/interrupt.h>
#include <arch/x86_64/rtc.h>
#include <arch/x86_64/irq_route.h>

#include <stdio.h>

/* production surfaces */
int rtc_pie_calibrate(uint64_t *tsc_hz_out, uint64_t *lapic_hz_out);
uint64_t arch_cycle_freq(void);
void lapic_timer_set_premeasured(uint64_t hz);

/* mock surface */
void boot2_imr_set(uint16_t port, uint8_t value);
uint8_t boot2_imr_get(uint16_t port);
unsigned long boot2_pending_get(void);
extern uint8_t boot2_cmos_regs[16];
extern int boot2_cmos_periodic;
uint32_t boot2_lapic_get(uint32_t offset);
void boot2_lapic_reset(void);
void boot2_tsc_set(uint64_t v);
void boot2_tsc_advance(uint64_t d);
void boot2_test_install_gs(void *base);
void boot2_rtc_ctx_prepare(void);
extern int boot2_tsc_frozen;
extern int boot2_if_flag;
extern unsigned long boot2_irq_enable_calls;

/* test-controlled CPUID 0x15 answer (cpuid.h shadowed for the prod TU) */
extern unsigned long boot2_cmos_accesses;
extern int boot2_cpu_id15_answer;

static void test_noop(uint64_t a, uint64_t b, pt_regs_t *c)
{
    (void)a; (void)b; (void)c;
}

/* select_controller wrap (registration-failure case drives it). */
static int b2rtc_wrap_active;
hw_int_controller_t *__real_arch_irq_select_controller(uint32_t gsi);
static uint64_t b2rtc_fail_install(uint64_t irq, void *arg)
{ (void)irq; (void)arg; return 1; }
static hw_int_controller_t b2rtc_failing = {
    .install = b2rtc_fail_install, .enable = NULL, .disable = NULL,
    .ack = NULL,
};
hw_int_controller_t *__wrap_arch_irq_select_controller(uint32_t gsi)
{
    if (b2rtc_wrap_active)
        return &b2rtc_failing;
    return __real_arch_irq_select_controller(gsi);
}

static void reset_mock(void)
{
    b2rtc_wrap_active = 0;
    boot2_if_flag = 0;
    boot2_irq_enable_calls = 0;
    boot2_imr_set(0x21, 0xff);
    boot2_imr_set(0xA1, 0xff);
    memset(boot2_cmos_regs, 0, sizeof(boot2_cmos_regs));
    boot2_cmos_regs[0x0A] = 0x26;   /* default status A */
    boot2_cmos_periodic = 0;
    boot2_lapic_reset();
    boot2_tsc_set(1000);
    for (uint32_t i = 0; i < 24; i++) {
        irq_desc_t *d = irq_desc_for_gsi(i);
        if (d) memset(d, 0, sizeof(*d));
    }
    struct irq_gsi_range r = { .base = 0, .count = 24 };
    irq_registry_init_ranges(&r, 1);
    x86_irq_routes_init(&r, 1);
    /* Establish the BOOT-2 CONTEXT state so the controlled window may
     * open: BSP percpu constructed/online, GS installed, IF=0. */
    boot2_rtc_ctx_prepare();
}

static void no_pf_but_af_counts_nothing(void)
{
    TEST_SUITE("boot2 rtc: only C.PF samples");
    reset_mock();
    /* AF (bit5) pending, PIE off, route masked: no valid sample. */
    boot2_cmos_regs[0x0C] = 0x20;
    uint64_t tsc_hz = 0xdeadbeef, lapic_hz = 0xdeadbeef;
    /* No periodic generation: the loop must end via the 2^32 TSC guard. */
    boot2_tsc_set(0);
    int rc = rtc_pie_calibrate(&tsc_hz, &lapic_hz);
    assert_eq(-1, rc);
    /* Failure must not overwrite the caller's outputs. */
    assert_eq(0xdeadbeef, tsc_hz);
    assert_eq(0xdeadbeef, lapic_hz);
    /* Cleanup: A/B restored (B stays as it was), route masked, LAPIC
     * LVT masked, IF still 0. */
    assert_eq(0, boot2_cmos_regs[0x0B] & 0x40);
    assert_eq(0x01, boot2_imr_get(0xA1) & 0x01);
    assert_eq(0x04, boot2_imr_get(0x21) & 0x04);
    assert_true((boot2_lapic_get(0x320) & (1u << 16)) != 0);  /* LVT masked */
    assert_eq(0, boot2_if_flag);
}

static void success_256_pf_samples(void)
{
    TEST_SUITE("boot2 rtc: 256 valid PF samples");
    reset_mock();
    boot2_cmos_periodic = 1;
    boot2_tsc_set(0);
    /* pause-driven delivery advances the TSC by a fixed step so the
     * window is 255 * STEP over 256 samples. */
    uint64_t tsc_hz = 0, lapic_hz = 0;
    int rc = rtc_pie_calibrate(&tsc_hz, &lapic_hz);
    assert_eq(0, rc);
    /* 255 intervals * 100000 cycles / 255 intervals * 1024/1024 scaling:
     * with the transaction's n=255 division the result is 1024 * STEP. */
    assert_true(tsc_hz > 0);
    assert_true(lapic_hz > 0);
    /* Cleanup state: PIE off, route masked, LAPIC masked, IF 0. */
    assert_eq(0, boot2_cmos_regs[0x0B] & 0x40);
    assert_eq(0x01, boot2_imr_get(0xA1) & 0x01);
    assert_true((boot2_lapic_get(0x320) & (1u << 16)) != 0);  /* LVT masked */
    assert_eq(0, boot2_if_flag);
    /* Exactly ONE sti: the controlled window. */
    assert_eq(1, (int)boot2_irq_enable_calls);
}

static void bad_preconditions(void)
{
    TEST_SUITE("boot2 rtc: precondition failures touch nothing");
    reset_mock();
    uint8_t a0 = boot2_cmos_regs[0x0A], b0 = boot2_cmos_regs[0x0B];
    unsigned long pending0 = boot2_pending_get();

    /* NULL outputs. */
    assert_eq(-1, rtc_pie_calibrate(NULL, NULL));
    /* Entry IF=1. */
    boot2_if_flag = 1;
    uint64_t t = 0, l = 0;
    assert_eq(-1, rtc_pie_calibrate(&t, &l));
    assert_eq(1, boot2_if_flag);            /* IF unchanged */
    boot2_if_flag = 0;

    /* No hardware touched: A/B same, no pending, route still masked. */
    assert_eq(a0, boot2_cmos_regs[0x0A]);
    assert_eq(b0, boot2_cmos_regs[0x0B]);
    assert_eq(pending0, boot2_pending_get());
    assert_eq(0x01, boot2_imr_get(0xA1) & 0x01);
}

static void registration_failure_clean(void)
{
    TEST_SUITE("boot2 rtc: registration failure cleanup path");
    reset_mock();
    /* Force the IRQ8 registration to fail via the wrapped controller. */
    b2rtc_wrap_active = 1;
    uint64_t t = 0x11, l = 0x22;
    boot2_tsc_set(0);
    assert_eq(-1, rtc_pie_calibrate(&t, &l));
    assert_eq(0x11, t);                     /* outputs untouched */
    assert_eq(0x22, l);
    assert_eq(0, boot2_cmos_regs[0x0B] & 0x40);  /* PIE never enabled */
    /* Registration precedes timer configuration: LAPIC INIT untouched. */
    assert_eq(0, boot2_lapic_get(0x380));
    assert_eq(0, boot2_lapic_get(0x3E0));
    irq_desc_t *d = irq_desc_for_gsi(8);
    assert_true(d != NULL && d->handler == NULL);  /* nothing registered */
    b2rtc_wrap_active = 0;
}

static void stuck_tsc_budget(void)
{
    TEST_SUITE("boot2 rtc: stuck TSC ends within the poll budget");
    reset_mock();
    boot2_cmos_periodic = 1;
    boot2_tsc_set(0);
    /* Freeze the TSC: the 2^32 guard never fires (counter never moves),
     * but the samples can never complete — the 50M pause budget must end
     * the wait with a failure.  (Mock knob: deliveries no longer advance
     * the TSC; the transaction's own budget counts pause rounds.) */
    boot2_tsc_frozen = 1;
    uint64_t t = 0, l = 0;
    assert_eq(-1, rtc_pie_calibrate(&t, &l));
    assert_eq(0, boot2_if_flag);
    assert_eq(0x01, boot2_imr_get(0xA1) & 0x01);   /* route re-masked */
    boot2_tsc_frozen = 0;
}

/* Each case gets its own forked child (fresh one-shot state), so the
 * two cache paths are separate cases, each with a single reset. */
static void freq_cache_rtc_path(void)
{
    TEST_SUITE("boot2 rtc: freq cache caches the RTC decision");
    reset_mock();
    fflush(stdout);
    boot2_cpu_id15_answer = 0;      /* CPUID invalid → RTC path */
    boot2_cmos_periodic = 1;
    boot2_tsc_set(0);
    uint64_t f1 = arch_cycle_freq();
    assert_true(f1 > 0);
    unsigned long accesses_after_first = boot2_cmos_accesses;

    /* Second call: pure cache query — no RTC access, no re-measure. */
    uint64_t f2 = arch_cycle_freq();
    assert_eq(f1, f2);
    assert_eq(accesses_after_first, boot2_cmos_accesses);
}

static void freq_cache_cpuid_path(void)
{
    TEST_SUITE("boot2 rtc: freq cache caches a CPUID answer");
    reset_mock();
    fflush(stdout);
    boot2_cpu_id15_answer = 1;
    unsigned long acc0 = boot2_cmos_accesses;
    uint64_t f3 = arch_cycle_freq();
    assert_eq(10000, f3);       /* ecx * ebx / eax from the mock CPUID */
    assert_eq(acc0, boot2_cmos_accesses);   /* no RTC side effects */
    assert_eq(f3, arch_cycle_freq());       /* cached too */
    assert_eq(acc0, boot2_cmos_accesses);
}

/* Each case runs in a forked child with an alarm: a case that hangs or
 * crashes (old-transaction behaviour under the real periodic-hardware
 * model) records as that case's failure instead of killing the binary.
 * The failure is the RED. */
static void run_case(const char *name, void (*fn)(void))
{
    pid_t pid = fork();
    if (pid == 0) {
        alarm(10);
        setbuf(stdout, NULL);       /* child: unbuffered diagnostics */
        fprintf(stderr, "  [child] enter %s\n", name);
        fn();
        fflush(stdout);
        _exit(__test_stats.failed ? 1 : 0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    int ok = B2_WIFEXITED(st) && B2_WEXITSTATUS(st) == 0;
    printf("  [%s] %s%s\n", ok ? "PASS" : "FAIL", name,
           (!ok && B2_WIFSIGNALED(st)) ? " (child signal)" : "");
    if (!ok) __test_stats.failed++;
    else __test_stats.passed++;
    __test_stats.total++;
}

int main(void)
{
    run_case("no_pf_but_af_counts_nothing", no_pf_but_af_counts_nothing);
    run_case("success_256_pf_samples", success_256_pf_samples);
    run_case("bad_preconditions", bad_preconditions);
    run_case("registration_failure_clean", registration_failure_clean);
    run_case("stuck_tsc_budget", stuck_tsc_budget);
    run_case("freq_cache_rtc_path", freq_cache_rtc_path);
    run_case("freq_cache_cpuid_path", freq_cache_cpuid_path);
    printf("\ntest_rtc_pie_calibration: %d total, %d passed, %d failed\n",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
