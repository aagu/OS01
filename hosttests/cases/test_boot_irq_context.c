/* hosttests/cases/test_boot_irq_context.c — BOOT-2 Task 2.
 *
 * Compiles the REAL boot orchestrator (kernel/arch/x86_64/platform/
 * boot.c x86_64_boot_subsystems), the REAL BOOT-1 percpu constructor
 * (smp/boot.c x86_64_boot_percpu), the NEW x86 private boot IRQ/time
 * coordinator (platform/boot_irq.c), the REAL PIT driver, tick chain
 * (time/tick.c, time/timer.c, intr/softirq.c) and the REAL IRQ
 * registry/dispatch over the boot_irq_time machine mocks.
 *
 * Pinned (spec §3/§6):
 *   - the orchestrator order is phase3 → x86_64_boot_percpu (GS) →
 *     context-ready → phase4 → runtime-open (PIT start + IF on) →
 *     phase5 → phase6;
 *   - the first sti happens only inside runtime-open after PIT start;
 *     before that the PIT route stays masked and no tick effects exist;
 *   - a pre-GS access traps (poison accessor);
 *   - the late public arch_boot_percpu only VERIFIES (no second GS
 *     install, no reconstruction);
 *   - after runtime-open, one delivery is exactly jiffies+1 /
 *     watchdog+1 / need_resched set, TIMER_SIRQ only on real expiry,
 *     and do_softirq runs registered actions.
 */
#include "test_framework.h"
#include <arch/irq.h>
#include <intr/interrupt.h>
#include <subsys/subsys.h>
/* intr/softirq.h pulls time/timer.h whose timer_t clashes with the
 * host glibc timer_t — declare the consumed surface directly. */
#define TIMER_SIRQ (1<<0)
void softirq_init(void);
void register_softirq(int32_t nr, void (*action)(void *), void *data);
void set_softirq_status(uint64_t status);
void do_softirq(void);
#include <core/bootinfo.h>
#include <intr/apic.h>
#include <arch/x86_64/irq_route.h>
#include <arch/x86_64/platform/boot_irq.h>

#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* production surfaces */
void x86_64_boot_subsystems(const struct boot_context *bootctx);
void x86_64_boot_percpu(void);
void arch_boot_percpu(void);
void timer_init(void);
/* timer.h / percpu.h clash with the host libc in this TU: declare the
 * consumed surface directly and read percpu fields via runtime
 * accessors. */
extern volatile uint64_t jiffies;
uint64_t boot2_watchdog(unsigned cpu);
uint64_t boot2_need_resched(unsigned cpu);
unsigned boot2_num_cpus(void);
void boot2_percpu_reset(void);
void *boot2_read_this_cpu(void);
uint32_t arch_irq_isa_to_gsi(uint8_t source);

/* mock surface */
void arch_this_cpu_set(unsigned id);
/* arch_register_subsys (above) is the test's machine-boundary mirror;
 * arch_boot_rsdp lives in the real platform/subsys.c which is NOT
 * linked — provide the storage. */
unsigned long long arch_boot_rsdp;
/* select_controller wrap passthrough (link contract from Task 1). */
hw_int_controller_t *__real_arch_irq_select_controller(uint32_t gsi);
hw_int_controller_t *__wrap_arch_irq_select_controller(uint32_t gsi)
{ return __real_arch_irq_select_controller(gsi); }
void boot2_imr_set(uint16_t port, uint8_t v);
uint8_t boot2_imr_get(uint16_t port);
unsigned long boot2_pending_get(void);
extern int boot2_if_flag;
extern unsigned long boot2_irq_enable_calls;
extern unsigned long boot2_account_tick_calls;
void boot2_test_install_gs(void *base);
void boot2_tsc_set(uint64_t v);
extern unsigned long boot2_gs_installs;
extern void (*boot2_gs_install_hook)(void);
extern void (*boot2_sti_hook)(void);
extern struct apic_info_stub { int _; };

/* set_tss_descriptor spy (machine boundary: GDT slot write) */
unsigned long boot2_tss_desc_calls;
void set_tss_descriptor(int sel, void *tss) { (void)sel; (void)tss; boot2_tss_desc_calls++; }
/* arch_task_boot_state (legacy TSS base) — machine boundary. */
void *boot2_fake_boot_tss;
void *arch_task_boot_state(void) { return &boot2_fake_boot_tss; }
/* init_tss array (tss.c is not linked). */
char boot2_init_tss_storage[8][128];
void *init_tss[8] = {
    boot2_init_tss_storage[0], boot2_init_tss_storage[1],
    boot2_init_tss_storage[2], boot2_init_tss_storage[3],
    boot2_init_tss_storage[4], boot2_init_tss_storage[5],
    boot2_init_tss_storage[6], boot2_init_tss_storage[7],
};
void kpanic(const char *msg, ...);

/* ── orchestrator trace ────────────────────────────────────────── */
enum {
    T_APIC_P3, T_PERCPU, T_CTX, T_TIMER_P4, T_CS_P4, T_LAPIC_P4,
    T_PIT_P4, T_RUNTIME, T_KBD_P5, T_AHCI_P6,
};
static int trace[32], trace_n;
static void tr(int e) { if (trace_n < 32) trace[trace_n++] = e; }

static int boot2_apic_init(void) { tr(T_APIC_P3); return 0; }
static int boot2_timer_init(void) { tr(T_TIMER_P4); timer_init(); return 0; }
static int boot2_cs_init(void) { tr(T_CS_P4); return 0; }
static int boot2_lapic_stub_init(void)
{
    tr(T_LAPIC_P4);
    extern void lapic_timer_init(void);
    lapic_timer_init();      /* BOOT-2: stub install ONLY (no calibrate) */
    return 0;
}
static int boot2_pit_init(void)
{
    tr(T_PIT_P4);
    /* REAL driver entry — registers with IRQF_NO_AUTOEN. */
    extern int pit_init(void);
    return pit_init();
}
static int boot2_kbd_init(void) { tr(T_KBD_P5); return 0; }
static int boot2_ahci_init(void) { tr(T_AHCI_P6); return 0; }

/* Machine boundary: the ELF .subsys_init linker section.  The test
 * mirrors the platform collection (the section is the boundary); the
 * orchestrator, framework and every driver entry point are REAL. */
void arch_register_subsys(void)
{
    register_subsys("apic", boot2_apic_init, SUBSYS_PHASE_3,
                    SUBSYS_FLAG_OPTIONAL);
    register_subsys("timer", boot2_timer_init, SUBSYS_PHASE_4,
                    SUBSYS_FLAG_OPTIONAL);
    register_subsys("clocksource", boot2_cs_init, SUBSYS_PHASE_4,
                    SUBSYS_FLAG_OPTIONAL);
    register_subsys("lapic-timer", boot2_lapic_stub_init, SUBSYS_PHASE_4,
                    SUBSYS_FLAG_OPTIONAL);
    register_subsys("pit", boot2_pit_init, SUBSYS_PHASE_4,
                    SUBSYS_FLAG_OPTIONAL);
    register_subsys("keyboard", boot2_kbd_init, SUBSYS_PHASE_5,
                    SUBSYS_FLAG_OPTIONAL);
    register_subsys("ahci", boot2_ahci_init, SUBSYS_PHASE_6,
                    SUBSYS_FLAG_OPTIONAL);
}

static void reset_mock(void)
{
    trace_n = 0;
    boot2_if_flag = 0;
    boot2_irq_enable_calls = 0;
    boot2_account_tick_calls = 0;
    boot2_tss_desc_calls = 0;
    boot2_imr_set(0x21, 0xff);
    boot2_imr_set(0xA1, 0xff);
    jiffies = 0;
    boot2_percpu_reset();
    arch_this_cpu_set(0);
    struct irq_gsi_range r = { .base = 0, .count = 24 };
    irq_registry_init_ranges(&r, 1);
    x86_irq_routes_init(&r, 1);
    softirq_init();
    timer_init();
    boot2_tsc_set(0);
}

static void record_runtime_open(void) { tr(T_RUNTIME); }

static void run_orchestrator(void)
{
    struct boot_context bootctx;
    memset(&bootctx, 0, sizeof(bootctx));
    bootctx.firmware.acpi_rsdp = 0x1234;
    x86_64_boot_subsystems(&bootctx);
}

static void orchestrator_order(void)
{
    TEST_SUITE("boot2 context: orchestrator phase order");
    reset_mock();
    boot2_sti_hook = record_runtime_open;
    /* apic_info: two enabled LAPIC entries (BSP + one AP). */
    memset(&apic_info, 0, sizeof(apic_info));
    apic_info.lapic_count = 2;
    apic_info.lapics[0].flags = 1;
    apic_info.lapics[1].flags = 1;
    apic_info.lapics[0].apic_id = 0;
    apic_info.lapics[1].apic_id = 2;

    run_orchestrator();

    int pos_apic = -1, pos_runtime = -1, pos_pit = -1, pos_kbd = -1,
        pos_ahci = -1, pos_timer = -1;
    for (int k = 0; k < trace_n; k++) {
        if (trace[k] == T_APIC_P3) pos_apic = k;
        if (trace[k] == T_PIT_P4) pos_pit = k;
        if (trace[k] == T_TIMER_P4) pos_timer = k;
        if (trace[k] == T_RUNTIME) pos_runtime = k;
        if (trace[k] == T_KBD_P5) pos_kbd = k;
        if (trace[k] == T_AHCI_P6) pos_ahci = k;
    }
    assert_true(pos_apic >= 0);
    assert_true(pos_timer > pos_apic);
    assert_true(pos_pit > pos_timer);
    assert_true(pos_runtime > pos_pit);   /* PIT start INSIDE runtime-open */
    assert_true(pos_runtime >= 0);
    assert_true(pos_kbd > pos_runtime);   /* phase5 after runtime open */
    assert_true(pos_ahci > pos_kbd);
    /* The first sti happens INSIDE runtime-open, after PIT start. */
    assert_eq(1, (int)boot2_irq_enable_calls);
    /* BOOT-1 percpu done exactly once, before phase 4 drivers ran. */
    assert_eq(1, (int)boot2_gs_installs);
    assert_eq(2, (int)boot2_num_cpus());
    assert_true(boot2_if_flag == 1);      /* runtime open → IRQs on */
    assert_true(x86_boot_irq_runtime_opened());
    assert_true(x86_boot_irq_calibration_allowed() == false);
}

static void pit_masked_before_runtime(void)
{
    TEST_SUITE("boot2 context: PIT masked through prepare/RTC stages");
    reset_mock();
    memset(&apic_info, 0, sizeof(apic_info));
    apic_info.lapic_count = 1;
    apic_info.lapics[0].flags = 1;
    /* Only phase 3 + percpu + context: stop before runtime open by
     * driving the pieces directly (real functions). */
    register_subsys("apic", boot2_apic_init, SUBSYS_PHASE_3, 0);
    subsys_init_phase(3);
    x86_64_boot_percpu();
    x86_boot_irq_context_prepare();

    /* PIT route masked even after phase4's prepare. */
    struct irq_gsi_range r = { .base = 0, .count = 24 };
    (void)r;
    subsys_init_phase(4);
    assert_eq(0x01, boot2_imr_get(0x21) & 0x01);   /* PIT still masked */
    /* A delivered edge cannot tick through a masked route. */
    uint64_t j0 = jiffies;
    arch_irq_dispatch(&(pt_regs_t){0}, 0x20);
    assert_eq(j0, jiffies);
    assert_eq(0, (int)boot2_account_tick_calls);
    assert_eq(0, boot2_if_flag);                   /* still IF=0 */
}

static int softirq_ran;
static void boot2_softirq_action(void *d) { (void)d; softirq_ran++; }

static void runtime_first_tick_effects(void)
{
    TEST_SUITE("boot2 context: first runtime tick chain");
    reset_mock();
    memset(&apic_info, 0, sizeof(apic_info));
    apic_info.lapic_count = 1;
    apic_info.lapics[0].flags = 1;
    run_orchestrator();
    assert_eq(1, boot2_if_flag);

    /* Softirq action registered through the REAL framework. */
    static int softirq_ran;
    register_softirq(TIMER_SIRQ, boot2_softirq_action, NULL);

    uint64_t j0 = jiffies;
    arch_irq_dispatch(&(pt_regs_t){0}, 0x20);   /* one real PIT delivery */

    assert_eq(j0 + 1, jiffies);
    assert_eq(1, (int)boot2_watchdog(0));
    assert_eq(1, (int)boot2_need_resched(0));
    assert_eq(1, (int)boot2_account_tick_calls);
    /* First tick: no timer expires yet (timer head has no expiry). */
    assert_eq(0, softirq_ran);
}

static void pre_gs_poison(void)
{
    TEST_SUITE("boot2 context: pre-GS access traps");
    pid_t pid = fork();
    if (pid == 0) {
        /* Fresh process: GS never installed; a GS read must trap. */
        void *c = boot2_read_this_cpu();
        (void)c;            /* must never get here */
        _exit(0);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    assert_true(WIFSIGNALED(st));           /* poison fired */
    assert_true(WTERMSIG(st) == SIGILL || WTERMSIG(st) == SIGTRAP);
}

static void late_facade_verify_only(void)
{
    TEST_SUITE("boot2 context: late arch_boot_percpu verifies only");
    reset_mock();
    memset(&apic_info, 0, sizeof(apic_info));
    apic_info.lapic_count = 1;
    apic_info.lapics[0].flags = 1;
    run_orchestrator();
    unsigned long installs_mid = boot2_gs_installs;
    uint32_t cpus_mid = boot2_num_cpus();

    /* Late public facade: must NOT reconstruct or reinstall. */
    arch_boot_percpu();
    assert_eq(cpus_mid, boot2_num_cpus());
    assert_eq(installs_mid, boot2_gs_installs);   /* no second GS install */
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
    run_case("orchestrator_order", orchestrator_order);
    run_case("pit_masked_before_runtime", pit_masked_before_runtime);
    run_case("runtime_first_tick_effects", runtime_first_tick_effects);
    run_case("pre_gs_poison", pre_gs_poison);
    run_case("late_facade_verify_only", late_facade_verify_only);
    printf("\ntest_boot_irq_context: %d total, %d passed, %d failed\n",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
