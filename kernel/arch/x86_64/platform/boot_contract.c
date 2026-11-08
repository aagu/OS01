/* kernel/arch/x86_64/platform/boot_contract.c — BOOT-4 stage contract capability checks
 *
 * Implements arch_boot_validate for x86_64, verifying capability invariants
 * at three checkpoints: DEVICES, PRE_SMP, and RUNTIME.
 *
 * Checkers read authentic object state without mutating hardware or registers.
 * Base verification safely validates GS base and BSP percpu invariants before
 * referencing percpu_data[0].
 */

#include <arch/boot.h>
#include <percpu/percpu.h>
#include <subsys/subsys.h>
#include <intr/softirq.h>
#include <time/timer.h>
#include <device/boot.h>
#include <intr/apic.h>
#include <intr/pic.h>
#include <driver/pit.h>
#include <sched/accounting.h>
#include <arch/irq.h>
#include <arch/x86_64/platform/boot_irq.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

static inline uint64_t platform_get_gs_base(void)
{
#if defined(OS01_HOST_TEST)
    extern uint64_t mock_x86_gs_base;
    return mock_x86_gs_base;
#else
    uint32_t lo, hi;
    __asm__ __volatile__("rdmsr" : "=a"(lo), "=d"(hi) : "c"(0xC0000101));
    return ((uint64_t)hi << 32) | lo;
#endif
}

static inline uint64_t platform_get_rflags(void)
{
#if defined(OS01_HOST_TEST)
    extern uint64_t mock_x86_rflags;
    return mock_x86_rflags;
#else
    uint64_t rflags;
    __asm__ __volatile__("pushfq; popq %0" : "=r"(rflags) :: "memory");
    return rflags;
#endif
}

int arch_boot_validate(enum arch_boot_checkpoint point, const char **component)
{
    if (point != ARCH_BOOT_CHECK_DEVICES &&
        point != ARCH_BOOT_CHECK_PRE_SMP &&
        point != ARCH_BOOT_CHECK_RUNTIME) {
        if (component) *component = "checkpoint";
        return -EINVAL;
    }

    /* 1. Base validation — safe pre-check before dereferencing percpu */
    uint64_t gs = platform_get_gs_base();
    if (gs != (uint64_t)&percpu_data[0]) {
        if (component) *component = "gs_base";
        return -EINVAL;
    }

    if (percpu_data[0].self != (uint64_t)&percpu_data[0]) {
        if (component) *component = "percpu_self";
        return -EINVAL;
    }

    if (percpu_data[0].cpu_id != 0) {
        if (component) *component = "cpu_id";
        return -EINVAL;
    }

    if (__atomic_load_n(&percpu_data[0].online, __ATOMIC_ACQUIRE) != 1) {
        if (component) *component = "bsp_online";
        return -EINVAL;
    }

    if (num_cpus < 1 || num_cpus > NR_CPUS) {
        if (component) *component = "num_cpus";
        return -EINVAL;
    }

    /* 2. Subsystems and time/softirq invariants */
    if (subsys_status("timer") != 1) {
        if (component) *component = "timer";
        return -ENODEV;
    }

    if (subsys_status("clocksource") != 1) {
        if (component) *component = "clocksource";
        return -ENODEV;
    }

    if (softirq_vector[0].action != do_timer) {
        if (component) *component = "timer_softirq";
        return -EINVAL;
    }

    if (subsys_status("apic") != 1) {
        if (component) *component = "apic";
        return -ENODEV;
    }

    if (!x86_boot_irq_runtime_opened()) {
        if (component) *component = "irq_runtime";
        return -EBUSY;
    }

    int dev_res = device_boot_result();
    if (dev_res != 0) {
        if (component) *component = "device_result";
        return dev_res;
    }

    if (point == ARCH_BOOT_CHECK_DEVICES)
        return 0;

    /* 3. Pre-SMP invariants */
    if (!sched_accounting_is_ready()) {
        if (component) *component = "sched_accounting";
        return -EAGAIN;
    }

    bool lapic_on = lapic_timer_local_running();
    bool pit_delivering = false;
    if (pit_started()) {
        if (ioapic_available()) {
            pit_delivering = ioapic_irq_unmasked(arch_irq_isa_to_gsi(0));
        } else {
            pit_delivering = pic_irq_unmasked(0);
        }
    }

    if ((lapic_on && pit_delivering) || (!lapic_on && !pit_delivering)) {
        if (component) *component = "tick_source";
        return -EINVAL;
    }

    if (point == ARCH_BOOT_CHECK_PRE_SMP)
        return 0;

    /* 4. Runtime invariants */
    if (__atomic_load_n(&percpu_data[0].ipi_ready, __ATOMIC_ACQUIRE) != 1) {
        if (component) *component = "ipi_ready";
        return -EBUSY;
    }

    uint64_t rflags = platform_get_rflags();
    if (!(rflags & (1UL << 9))) {
        if (component) *component = "irq_enable";
        return -EINVAL;
    }

    return 0;
}
