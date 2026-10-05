/* hosttests/cases/test_double_state_publish.c — M3 Task 11 BSP double-state
 * publish ordering source-scan.
 *
 * QEMU aarch64 boots real firmware, so the BSP-side publish ordering
 * (percpu_init / num_cpus / smp_starting_enter before the first possible
 * PSCI CPU_ON; ipi_ready published only after IRQs are unmasked) cannot be
 * observed from a host binary. Like test_gic_marker_lines.c, this test
 * reads the kernel sources into memory and asserts the ORDER constraints
 * directly on the source text (first-occurrence index comparison).
 *
 * Order constraints asserted:
 *
 *   main.c (kernel/arch/aarch64/boot/main.c):
 *     1. percpu_install_gs(0) + percpu_init(0, ...) BEFORE smp_boot_aps()
 *     2. release store to percpu_data[0].online BEFORE smp_boot_aps()
 *     3. release store to num_cpus (dtb_cpu_count()) BEFORE smp_boot_aps()
 *     4. smp_starting_enter() BEFORE smp_boot_aps()
 *     5. arch_local_irq_enable() BEFORE ipi_ready_publish_and_count(0)
 *
 *   smp.c (kernel/arch/aarch64/smp/smp.c, AP tail — Task 10):
 *     6. arch_local_irq_enable() BEFORE ipi_ready_publish_and_count(cpu_id)
 *     7. gic_target_bit_init(cpu_id) BEFORE ipi_ready_publish_and_count(cpu_id)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "test_framework.h"

#ifndef OS01_KERNEL_SRC
#error "OS01_KERNEL_SRC must be defined to the kernel source root"
#endif

/* fread-grow slurp — the libc stdio.h shim (force-included via
 * test_platform.h) exposes no fseek/ftell, so avoid them. */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap);
    if (!buf) { fclose(f); return NULL; }
    for (;;) {
        if (n + 1024 > cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); fclose(f); return NULL; }
            buf = nb; cap *= 2;
        }
        size_t got = fread(buf + n, 1, 1024, f);
        n += got;
        if (got < 1024) break;
    }
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* First-occurrence index helpers; -1 when the needle is absent. */
static long find_idx(const char *hay, const char *needle)
{
    const char *p = strstr(hay, needle);
    return p ? (long)(p - hay) : -1L;
}

typedef struct {
    const char *label;
    const char *before;   /* this must appear ... */
    const char *after;    /* ... before this does */
} order_check_t;

static const order_check_t k_main_checks[] = {
    {
        "main.c: percpu_install_gs(0) before smp_boot_aps()",
        "percpu_install_gs(0)",
        "smp_boot_aps()",
    },
    {
        "main.c: percpu_init(0, mpidr_bsp) before smp_boot_aps()",
        "percpu_init(0, mpidr_bsp)",
        "smp_boot_aps()",
    },
    {
        "main.c: online=1 release store before smp_boot_aps()",
        "&percpu_data[0].online, 1, __ATOMIC_RELEASE",
        "smp_boot_aps()",
    },
    {
        "main.c: num_cpus release store before smp_boot_aps()",
        "&num_cpus, dtb_cpu_count(), __ATOMIC_RELEASE",
        "smp_boot_aps()",
    },
    {
        "main.c: smp_starting_enter() before smp_boot_aps()",
        "smp_starting_enter()",
        "smp_boot_aps()",
    },
    {
        "main.c: arch_local_irq_enable() before ipi_ready_publish_and_count(0)",
        "arch_local_irq_enable()",
        "ipi_ready_publish_and_count(0)",
    },
};

static const order_check_t k_smp_checks[] = {
    {
        "smp.c: gic_target_bit_init(cpu_id) before ipi_ready_publish_and_count",
        "gic_target_bit_init(cpu_id)",
        "ipi_ready_publish_and_count(cpu_id)",
    },
    {
        "smp.c: arch_local_irq_enable() before ipi_ready_publish_and_count",
        "arch_local_irq_enable()",
        "ipi_ready_publish_and_count(cpu_id)",
    },
};

static bool run_order_checks(const char *buf, const order_check_t *checks,
                             size_t n, const char *file_desc)
{
    bool ok = true;
    for (size_t i = 0; i < n; ++i) {
        long b = find_idx(buf, checks[i].before);
        long a = find_idx(buf, checks[i].after);
        if (b < 0) {
            printf("  [MISS] %s: missing needle \"%s\" in %s\n",
                   checks[i].label, checks[i].before, file_desc);
            ok = false;
        } else if (a < 0) {
            printf("  [MISS] %s: missing needle \"%s\" in %s\n",
                   checks[i].label, checks[i].after, file_desc);
            ok = false;
        } else if (b > a) {
            printf("  [ORD ] %s: \"%s\" at %ld after \"%s\" at %ld\n",
                   checks[i].label, checks[i].before, b,
                   checks[i].after, a);
            ok = false;
        }
    }
    return ok;
}

static void suite_main_order(void)
{
    TEST_SUITE("double_state_publish main.c ordering");
    char path[512];
    snprintf(path, sizeof(path), "%s/kernel/arch/aarch64/boot/main.c",
             OS01_KERNEL_SRC);
    char *buf = read_file(path);
    assert_true(buf != NULL);
    if (!buf) return;
    assert_true(run_order_checks(buf, k_main_checks,
                                sizeof(k_main_checks) / sizeof(k_main_checks[0]),
                                "main.c"));
    /* Substring sanity: the BSP publishes through the same one-shot
     * function as the APs, and enters the SMP gate before CPU_ON. */
    assert_true(find_idx(buf, "ipi_ready_publish_and_count(0)") >= 0);
    assert_true(find_idx(buf, "smp_starting_enter()") >= 0);
    free(buf);
}

static void suite_ap_tail_order(void)
{
    TEST_SUITE("double_state_publish smp.c AP tail ordering");
    char path[512];
    snprintf(path, sizeof(path), "%s/kernel/arch/aarch64/smp/smp.c",
             OS01_KERNEL_SRC);
    char *buf = read_file(path);
    assert_true(buf != NULL);
    if (!buf) return;
    assert_true(run_order_checks(buf, k_smp_checks,
                                sizeof(k_smp_checks) / sizeof(k_smp_checks[0]),
                                "smp.c"));
    free(buf);
}

int main(void)
{
    suite_main_order();
    suite_ap_tail_order();
    printf("\ntest_double_state_publish: %d total, %d passed, %d failed\n",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
