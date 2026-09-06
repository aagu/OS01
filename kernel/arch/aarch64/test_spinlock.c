/* Boot-only shared spinlock test. SMP startup owns initialization and
 * AP lifecycle; this file owns iterations and BSP result verification. */
#include <stdint.h>
#include <stdbool.h>
#include <kernel/arch/cpu.h>
#include <kernel/arch/aarch64/spinlock.h>
#include <kernel/arch/aarch64/boot_log.h>
#include <kernel/arch/aarch64/smp.h>
#include "aarch64_percpu.h"
#include "reg.h"

#define ITERATIONS_PER_CORE 1000000U

void test_spinlock(void)
{
    static spinlock_T lock;
    static uint64_t counter;
    spin_init(&lock);
    counter = 0;
    for (uint32_t i = 0; i < ITERATIONS_PER_CORE; ++i) {
        spin_lock(&lock);
        ++counter;
        spin_unlock(&lock);
    }
    if (!spin_trylock(&lock)) {
        log_err("[spinlock] single-core FAIL trylock=0\n");
        return;
    }
    ++counter;
    spin_unlock(&lock);
    uint64_t flags = spin_lock_irqsave(&lock);
    ++counter;
    spin_unlock_irqrestore(&lock, flags);
    if (counter == 1000002)
        log_info("[spinlock] single-core 1M PASS\n");
    else
        log_err("[spinlock] single-core FAIL\n");
}

void smp_bench_iter(uint32_t cpu_id, uint32_t iterations)
{
    for (uint32_t i = 0; i < iterations; ++i) {
        spin_lock(&bench_lock);
        /* A plain increment protected only by the lock: lost updates
         * are observable in the final N * 1000000 total. */
        benchmark_total = benchmark_total + 1;
        spin_unlock(&bench_lock);
    }
    bench_done_set(cpu_id, 1);
}

bool test_spinlock_smp(uint32_t active)
{
    if (!active || active > NR_CPUS) return false;
    uint64_t hz = cntfrq_el0();
    if (!hz || hz > UINT64_MAX / 30) return false;
    const uint64_t start = arch_cycle_counter();
    for (uint32_t id = 1; id < active; ++id)
        boot_go_set(id, AARCH64_BOOT_GO_TEST);

    /* Main only invokes this function for the fully acknowledged set,
     * whose logical BSP is always slot zero. */
    smp_bench_iter(0, ITERATIONS_PER_CORE);
    uint32_t done_mask = 0;
    const uint32_t expected_mask = (1U << active) - 1;
    while (done_mask != expected_mask) {
        for (uint32_t id = 0; id < active; ++id)
            if (bench_done_get(id)) done_mask |= 1U << id;
        if ((uint64_t)(arch_cycle_counter() - start) >= hz * 30) break;
        arch_cpu_pause();
    }

    spin_lock(&bench_lock);
    uint32_t total = benchmark_total;
    spin_unlock(&bench_lock);
    for (uint32_t id = 0; id < active; ++id) {
        log_info("[spinlock] cpu=");
        kputu(id);
        log_info(" done=");
        kputu(done_mask & (1U << id) ? ITERATIONS_PER_CORE : 0);
        log_info("\n");
    }
    bool passed = done_mask == expected_mask && total == active * ITERATIONS_PER_CORE;
    log_info("[spinlock] active=");
    kputu(active);
    log_info(" iterations=1000000 total=");
    kputu(total);
    if (passed) log_info(" status=PASS\n");
    else log_err(" status=FAIL\n");
    return passed;
}
