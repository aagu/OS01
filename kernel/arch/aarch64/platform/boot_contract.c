/* kernel/arch/aarch64/platform/boot_contract.c — BOOT-4 stage contract capability checks
 *
 * Implements arch_boot_validate for AArch64, verifying capability invariants
 * at three checkpoints: DEVICES, PRE_SMP, and RUNTIME.
 *
 * Checkers read authentic object state without mutating hardware or registers.
 * Base verification safely validates TPIDR_EL1 and BSP percpu invariants before
 * referencing percpu_data[0].
 */

#include <arch/boot.h>
#include <percpu/percpu.h>
#include <subsys/subsys.h>
#include <intr/softirq.h>
#include <time/timer.h>
#include <arch/aarch64/timer.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

static inline uint64_t platform_get_tpidr_el1(void)
{
#if defined(OS01_HOST_TEST)
    extern uint64_t mock_a64_tpidr_el1;
    return mock_a64_tpidr_el1;
#else
    uint64_t tpidr;
    __asm__ __volatile__("mrs %0, tpidr_el1" : "=r"(tpidr));
    return tpidr;
#endif
}

static inline uint64_t platform_get_daif(void)
{
#if defined(OS01_HOST_TEST)
    extern uint64_t mock_a64_daif;
    return mock_a64_daif;
#else
    uint64_t daif;
    __asm__ __volatile__("mrs %0, daif" : "=r"(daif));
    return daif;
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
    uint64_t tpidr = platform_get_tpidr_el1();
    if (tpidr != (uint64_t)&percpu_data[0]) {
        if (component) *component = "tpidr_el1";
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

    if (point == ARCH_BOOT_CHECK_DEVICES)
        return 0;

    /* 3. Pre-SMP invariants (N/A for AArch64) */
    if (point == ARCH_BOOT_CHECK_PRE_SMP)
        return 0;

    /* 4. Runtime invariants */
    if (__atomic_load_n(&percpu_data[0].ipi_ready, __ATOMIC_ACQUIRE) != 1) {
        if (component) *component = "ipi_ready";
        return -EBUSY;
    }

    uint64_t daif = platform_get_daif();
    if (daif & (1UL << 7)) {
        if (component) *component = "irq_enable";
        return -EINVAL;
    }

    if (!aarch64_timer_config_ready()) {
        if (component) *component = "timer_config";
        return -EAGAIN;
    }

    if (!aarch64_timer_local_running()) {
        if (component) *component = "cntp_timer";
        return -EIO;
    }

    return 0;
}
