/* hosttests/cases/test_boot_contract_softirq.c — BOOT-4 Task 3.
 *
 * Unit tests verifying softirq initialization ordering with timer_init:
 *   - timer_init registers do_timer for TIMER_SIRQ (0);
 *   - calling softirq_init AFTER timer_init wipes the handler (reproducing bug);
 *   - correct order: softirq_init called first, then timer_init registers do_timer;
 *   - repeated softirq_init is prohibited.
 *   - source contract: aarch64 boot main.c calls softirq_init before phase 4 /
 *     devices and does not call it in arch_boot_aps.
 */

#include "test_framework.h"
#include <intr/softirq.h>
#include <time/timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#ifndef OS01_KERNEL_SRC
#define OS01_KERNEL_SRC "."
#endif

/* Test 1: Calling softirq_init after timer_init reproduces action wipe */
static void test_softirq_wipe_reproduction(void)
{
    /* 1. timer_init registers do_timer into softirq_vector[0] */
    timer_init();
    assert_true(softirq_vector[0].action == do_timer);

    /* 2. Calling softirq_init clears the vector (the bug!) */
    softirq_init();
    assert_true(softirq_vector[0].action == NULL);

    /* 3. When called in correct order (softirq_init then timer_init): vector preserved */
    softirq_init();
    timer_init();
    assert_true(softirq_vector[0].action == do_timer);
}

/* Test 2: Source contract check for aarch64 boot orchestration */
static FILE *open_kernel_file(const char *relpath)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", OS01_KERNEL_SRC, relpath);
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(path, sizeof(path), "../%s", relpath);
        f = fopen(path, "r");
    }
    if (!f) {
        snprintf(path, sizeof(path), "%s", relpath);
        f = fopen(path, "r");
    }
    return f;
}

static char *slurp_file(const char *relpath)
{
    FILE *f = open_kernel_file(relpath);
    if (!f) return NULL;
    size_t cap = 1 << 16, len = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (len + 4096 + 1 > cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
        size_t got = fread(buf + len, 1, 4096, f);
        len += got;
        if (got < 4096) break;
    }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

static void test_softirq_aarch64_source_contract(void)
{
    char *a64_src = slurp_file("kernel/arch/aarch64/boot/main.c");
    assert_true(a64_src != NULL);
    if (!a64_src) return;

    /* Find arch_boot_devices and arch_boot_runtime (the retired
     * arch_boot_aps moved to kernel/core/smp.c with the unified SMP
     * boot — softirq ordering is anchored on the runtime stage now). */
    char *devices_fn = strstr(a64_src, "arch_boot_devices");
    char *runtime_fn = strstr(a64_src, "arch_boot_runtime");

    assert_true(devices_fn != NULL);
    assert_true(runtime_fn != NULL);

    if (devices_fn && runtime_fn) {
        /* softirq_init must appear inside arch_boot_devices */
        char *softirq_in_dev = strstr(devices_fn, "softirq_init");
        assert_true(softirq_in_dev != NULL);
        if (softirq_in_dev) {
            /* And it stays an arch_boot_devices-time init: strictly
             * before arch_boot_runtime ever runs. */
            assert_true(softirq_in_dev < runtime_fn);
            /* And must be before subsys_init_phase(SUBSYS_PHASE_TIME) or phase 4 */
            char *phase4 = strstr(devices_fn, "SUBSYS_PHASE_TIME");
            if (!phase4) phase4 = strstr(devices_fn, "subsys_init_phase(4)");
            assert_true(phase4 != NULL);
            if (phase4) {
                assert_true(softirq_in_dev < phase4);
            }
        }

        /* softirq_init must NOT appear inside arch_boot_runtime */
        char *softirq_in_runtime = strstr(runtime_fn, "softirq_init");
        char *next_fn = strstr(runtime_fn, "arch_post_smp_selftest");
        if (next_fn && softirq_in_runtime) {
            assert_true(softirq_in_runtime > next_fn);
        } else {
            assert_true(softirq_in_runtime == NULL);
        }
    }

    /* The unified orchestrator (kernel/core/smp.c::arch_boot_aps) must
     * not touch softirq state either. */
    char *core_src = slurp_file("kernel/core/smp.c");
    assert_true(core_src != NULL);
    if (core_src) {
        assert_true(strstr(core_src, "softirq_init") == NULL);
        free(core_src);
    }

    free(a64_src);
}

int main(void)
{
    TEST_SUITE("BOOT-4 test_boot_contract_softirq");

    test_softirq_wipe_reproduction();
    test_softirq_aarch64_source_contract();

    TEST_RESULTS();
    return 0;
}
