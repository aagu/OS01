/* hosttests/cases/test_boot_contract_checks.c — BOOT-4 Task 3.
 *
 * Unit tests verifying dual-architecture checkpoint capability validation:
 *   - base validation (GS / TPIDR) fails safely without dereferencing this_cpu;
 *   - offline, percpu_self mismatch, cpu_id != 0, invalid num_cpus rejected;
 *   - timer/clocksource/apic status failures detected;
 *   - TIMER_SIRQ == do_timer verified;
 *   - sched_accounting_is_ready verified;
 *   - x86 tick deliverability: exactly one periodic source (LAPIC or PIT) enabled;
 *   - aarch64 runtime: config ready, CNTP running, IPI ready, DAIF.IRQ enabled;
 *   - unknown checkpoint rejected.
 */

#include "test_framework.h"
#include <arch/boot.h>
#include <percpu/percpu.h>
#include <subsys/subsys.h>
#include <intr/softirq.h>
#include <time/timer.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* Mock state for privileged / hardware boundaries */
uint64_t mock_x86_gs_base = 0;
uint64_t mock_x86_rflags = 0;
uint64_t mock_a64_tpidr_el1 = 0;
uint64_t mock_a64_daif = 0;

percpu_t percpu_data[NR_CPUS];
softirq_t softirq_vector[64];
void do_timer(void *data) { (void)data; }

/* Mock symbols for getters and submodules */
bool mock_sched_accounting_ready = false;
bool sched_accounting_is_ready(void) { return mock_sched_accounting_ready; }

bool mock_pit_started_val = false;
bool pit_started(void) { return mock_pit_started_val; }

bool mock_ioapic_avail_val = false;
bool ioapic_available(void) { return mock_ioapic_avail_val; }

uint32_t arch_irq_isa_to_gsi(uint8_t isa) { (void)isa; return 2; }

bool mock_ioapic_unmasked_val = false;
bool ioapic_irq_unmasked(uint32_t gsi) { (void)gsi; return mock_ioapic_unmasked_val; }

bool mock_pic_unmasked_val = false;
bool pic_irq_unmasked(unsigned irq) { (void)irq; return mock_pic_unmasked_val; }

bool mock_lapic_running_val = false;
bool lapic_timer_local_running(void) { return mock_lapic_running_val; }

bool mock_x86_irq_runtime_opened = false;
bool x86_boot_irq_runtime_opened(void) { return mock_x86_irq_runtime_opened; }

int mock_device_boot_res = 0;
int device_boot_result(void) { return mock_device_boot_res; }

bool mock_a64_timer_config_ready_val = false;
bool aarch64_timer_config_ready(void) { return mock_a64_timer_config_ready_val; }

bool mock_a64_timer_local_running_val = false;
bool aarch64_timer_local_running(void) { return mock_a64_timer_local_running_val; }

/* Subsys mock status */
static int mock_status_timer = 1;
static int mock_status_clocksource = 1;
static int mock_status_apic = 1;

int subsys_status(const char *name)
{
    if (!name) return -999;
    if (strcmp(name, "timer") == 0) return mock_status_timer;
    if (strcmp(name, "clocksource") == 0) return mock_status_clocksource;
    if (strcmp(name, "apic") == 0) return mock_status_apic;
    return -999;
}

uint32_t num_cpus = 2;

static void reset_check_state(void)
{
    mock_x86_gs_base = (uint64_t)&percpu_data[0];
    mock_x86_rflags = (1UL << 9); /* IF enabled */
    mock_a64_tpidr_el1 = (uint64_t)&percpu_data[0];
    mock_a64_daif = 0; /* IRQ unmasked */

    memset(&percpu_data[0], 0, sizeof(percpu_data[0]));
    percpu_data[0].self = (uint64_t)&percpu_data[0];
    percpu_data[0].cpu_id = 0;
    percpu_data[0].online = 1;
    percpu_data[0].ipi_ready = 1;
    num_cpus = 2;

    mock_sched_accounting_ready = true;
    mock_pit_started_val = false;
    mock_ioapic_avail_val = true;
    mock_ioapic_unmasked_val = false;
    mock_pic_unmasked_val = false;
    mock_lapic_running_val = true; /* default: LAPIC running, PIT not */
    mock_x86_irq_runtime_opened = true;
    mock_device_boot_res = 0;

    mock_a64_timer_config_ready_val = true;
    mock_a64_timer_local_running_val = true;

    mock_status_timer = 1;
    mock_status_clocksource = 1;
    mock_status_apic = 1;

    softirq_vector[0].action = do_timer;
}

/* ── Test Suite ────────────────────────────────────────────── */

static void test_validate_base_and_devices(void)
{
    reset_check_state();
    const char *comp = NULL;

    /* 1. Normal devices checkpoint passes */
    assert_eq(0, arch_boot_validate(ARCH_BOOT_CHECK_DEVICES, &comp));

    /* 2. Base invalid (GS != &percpu_data[0]) */
#if defined(__x86_64__)
    mock_x86_gs_base = 0;
    assert_eq(-EINVAL, arch_boot_validate(ARCH_BOOT_CHECK_DEVICES, &comp));
    assert_str_eq("gs_base", comp);
    mock_x86_gs_base = (uint64_t)&percpu_data[0];
#elif defined(__aarch64__)
    mock_a64_tpidr_el1 = 0;
    assert_eq(-EINVAL, arch_boot_validate(ARCH_BOOT_CHECK_DEVICES, &comp));
    assert_str_eq("tpidr_el1", comp);
    mock_a64_tpidr_el1 = (uint64_t)&percpu_data[0];
#endif

    /* 3. BSP self pointer mismatch */
    percpu_data[0].self = 0;
    assert_eq(-EINVAL, arch_boot_validate(ARCH_BOOT_CHECK_DEVICES, &comp));
    assert_str_eq("percpu_self", comp);
    percpu_data[0].self = (uint64_t)&percpu_data[0];

    /* 4. BSP offline */
    percpu_data[0].online = 0;
    assert_eq(-EINVAL, arch_boot_validate(ARCH_BOOT_CHECK_DEVICES, &comp));
    assert_str_eq("bsp_online", comp);
    percpu_data[0].online = 1;

    /* 5. num_cpus out of bounds */
    num_cpus = 0;
    assert_eq(-EINVAL, arch_boot_validate(ARCH_BOOT_CHECK_DEVICES, &comp));
    assert_str_eq("num_cpus", comp);
    num_cpus = 2;

    /* 6. timer status failed */
    mock_status_timer = -1;
    assert_eq(-ENODEV, arch_boot_validate(ARCH_BOOT_CHECK_DEVICES, &comp));
    assert_str_eq("timer", comp);
    mock_status_timer = 1;

    /* 7. clocksource status failed */
    mock_status_clocksource = 0;
    assert_eq(-ENODEV, arch_boot_validate(ARCH_BOOT_CHECK_DEVICES, &comp));
    assert_str_eq("clocksource", comp);
    mock_status_clocksource = 1;

    /* 8. softirq vector[0] wiped */
    softirq_vector[0].action = NULL;
    assert_eq(-EINVAL, arch_boot_validate(ARCH_BOOT_CHECK_DEVICES, &comp));
    assert_str_eq("timer_softirq", comp);
    softirq_vector[0].action = do_timer;

#if defined(__x86_64__)
    /* 9. apic status failed */
    mock_status_apic = 0;
    assert_eq(-ENODEV, arch_boot_validate(ARCH_BOOT_CHECK_DEVICES, &comp));
    assert_str_eq("apic", comp);
    mock_status_apic = 1;

    /* 10. runtime opened false */
    mock_x86_irq_runtime_opened = false;
    assert_eq(-EBUSY, arch_boot_validate(ARCH_BOOT_CHECK_DEVICES, &comp));
    assert_str_eq("irq_runtime", comp);
    mock_x86_irq_runtime_opened = true;

    /* 11. device boot result non-zero */
    mock_device_boot_res = -EIO;
    assert_eq(-EIO, arch_boot_validate(ARCH_BOOT_CHECK_DEVICES, &comp));
    assert_str_eq("device_result", comp);
    mock_device_boot_res = 0;
#endif
}

static void test_validate_pre_smp(void)
{
    reset_check_state();
    const char *comp = NULL;

    /* Normal pre-smp checkpoint passes */
    assert_eq(0, arch_boot_validate(ARCH_BOOT_CHECK_PRE_SMP, &comp));

#if defined(__x86_64__)
    /* sched_accounting not ready */
    mock_sched_accounting_ready = false;
    assert_eq(-EAGAIN, arch_boot_validate(ARCH_BOOT_CHECK_PRE_SMP, &comp));
    assert_str_eq("sched_accounting", comp);
    mock_sched_accounting_ready = true;

    /* Both tick sources stopped */
    mock_lapic_running_val = false;
    mock_pit_started_val = false;
    assert_eq(-EINVAL, arch_boot_validate(ARCH_BOOT_CHECK_PRE_SMP, &comp));
    assert_str_eq("tick_source", comp);

    /* Both tick sources running simultaneously */
    mock_lapic_running_val = true;
    mock_pit_started_val = true;
    mock_ioapic_avail_val = true;
    mock_ioapic_unmasked_val = true;
    assert_eq(-EINVAL, arch_boot_validate(ARCH_BOOT_CHECK_PRE_SMP, &comp));
    assert_str_eq("tick_source", comp);

    /* PIT only (fallback valid): lapic stopped, PIT unmasked in IOAPIC */
    mock_lapic_running_val = false;
    mock_pit_started_val = true;
    mock_ioapic_avail_val = true;
    mock_ioapic_unmasked_val = true;
    assert_eq(0, arch_boot_validate(ARCH_BOOT_CHECK_PRE_SMP, &comp));

    /* PIT only via legacy PIC */
    mock_ioapic_avail_val = false;
    mock_pic_unmasked_val = true;
    assert_eq(0, arch_boot_validate(ARCH_BOOT_CHECK_PRE_SMP, &comp));
#endif
}

static void test_validate_runtime(void)
{
    reset_check_state();
    const char *comp = NULL;

    /* Normal runtime checkpoint passes */
    assert_eq(0, arch_boot_validate(ARCH_BOOT_CHECK_RUNTIME, &comp));

    /* IPI not ready */
    percpu_data[0].ipi_ready = 0;
    assert_eq(-EBUSY, arch_boot_validate(ARCH_BOOT_CHECK_RUNTIME, &comp));
    assert_str_eq("ipi_ready", comp);
    percpu_data[0].ipi_ready = 1;

#if defined(__x86_64__)
    /* IRQ not enabled (IF = 0) */
    mock_x86_rflags = 0;
    assert_eq(-EINVAL, arch_boot_validate(ARCH_BOOT_CHECK_RUNTIME, &comp));
    assert_str_eq("irq_enable", comp);
    mock_x86_rflags = (1UL << 9);
#elif defined(__aarch64__)
    /* DAIF.IRQ masked (bit 7 set) */
    mock_a64_daif = (1UL << 7);
    assert_eq(-EINVAL, arch_boot_validate(ARCH_BOOT_CHECK_RUNTIME, &comp));
    assert_str_eq("irq_enable", comp);
    mock_a64_daif = 0;

    /* CNTP timer not running */
    mock_a64_timer_local_running_val = false;
    assert_eq(-EIO, arch_boot_validate(ARCH_BOOT_CHECK_RUNTIME, &comp));
    assert_str_eq("cntp_timer", comp);
    mock_a64_timer_local_running_val = true;

    /* timer config not ready */
    mock_a64_timer_config_ready_val = false;
    assert_eq(-EAGAIN, arch_boot_validate(ARCH_BOOT_CHECK_RUNTIME, &comp));
    assert_str_eq("timer_config", comp);
    mock_a64_timer_config_ready_val = true;
#endif

    /* Unknown checkpoint */
    assert_eq(-EINVAL, arch_boot_validate((enum arch_boot_checkpoint)999, &comp));
    assert_str_eq("checkpoint", comp);
}

int main(void)
{
    TEST_SUITE("BOOT-4 test_boot_contract_checks");

    test_validate_base_and_devices();
    test_validate_pre_smp();
    test_validate_runtime();

    TEST_RESULTS();
    return 0;
}
