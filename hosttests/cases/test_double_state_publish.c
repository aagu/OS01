/* hosttests/cases/test_double_state_publish.c — M3 Task 11 BSP double-state
 * publish ordering source-scan.
 *
 * QEMU aarch64 boots real firmware, so the BSP-side publish ordering
 * (percpu_init / num_cpus / smp_starting_enter before the first possible
 * PSCI CPU_ON; ipi_ready published only after IRQs are unmasked) cannot be
 * observed from a host binary. Like test_gic_marker_lines.c, this test
 * reads the kernel sources into memory and asserts the ORDER constraints
 * directly on the source text using semantic scoped checks.
 *
 * Order constraints asserted:
 *
 *   kernel_main (kernel/core/main.c):
 *     1. arch_boot_percpu() BEFORE arch_boot_aps()
 *
 *   arch_boot_percpu (kernel/arch/aarch64/boot/main.c):
 *     2. percpu_install_gs(0) BEFORE percpu_init(0, mpidr_bsp)
 *     3. percpu_init(0, mpidr_bsp) BEFORE release store to percpu_data[0].online
 *     4. release store to percpu_data[0].online BEFORE release store to num_cpus
 *     5. release store to num_cpus BEFORE smp_starting_enter()
 *
 *   arch_boot_aps (kernel/arch/aarch64/boot/main.c):
 *     6. smp_boot_aps() BEFORE arch_local_irq_enable()
 *     7. arch_local_irq_enable() BEFORE ipi_ready_publish_and_count(0)
 *
 *   smp.c (kernel/arch/aarch64/smp/smp.c, AP tail — Task 10):
 *     8. arch_local_irq_enable() BEFORE ipi_ready_publish_and_count(cpu_id)
 *     9. gic_target_bit_init(cpu_id) BEFORE ipi_ready_publish_and_count(cpu_id)
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

/* Extract a substring slice between start_needle and end_needle. */
static char *extract_scope(const char *buf, const char *start_needle, const char *end_needle)
{
    const char *start = strstr(buf, start_needle);
    if (!start) return NULL;
    const char *end = strstr(start + strlen(start_needle), end_needle);
    if (!end) return NULL;
    size_t len = (size_t)(end - start) + strlen(end_needle);
    char *slice = malloc(len + 1);
    if (!slice) return NULL;
    memcpy(slice, start, len);
    slice[len] = '\0';
    return slice;
}

typedef struct {
    const char *label;
    const char *before;   /* this must appear ... */
    const char *after;    /* ... before this does */
} order_check_t;

static const order_check_t k_kernel_main_checks[] = {
    {
        "kernel_main: arch_boot_percpu() before arch_boot_aps()",
        "arch_boot_percpu()",
        "arch_boot_aps()",
    },
};

static const order_check_t k_percpu_checks[] = {
    {
        "arch_boot_percpu: percpu_install_gs(0) before percpu_init",
        "percpu_install_gs(0)",
        "percpu_init(0, mpidr_bsp)",
    },
    {
        "arch_boot_percpu: percpu_init before online=1 release store",
        "percpu_init(0, mpidr_bsp)",
        "&percpu_data[0].online, 1, __ATOMIC_RELEASE",
    },
    {
        "arch_boot_percpu: online=1 before num_cpus release store",
        "&percpu_data[0].online, 1, __ATOMIC_RELEASE",
        "&num_cpus, dtb_cpu_count(), __ATOMIC_RELEASE",
    },
    {
        "arch_boot_percpu: num_cpus release store before smp_starting_enter",
        "&num_cpus, dtb_cpu_count(), __ATOMIC_RELEASE",
        "smp_starting_enter()",
    },
};

static const order_check_t k_aps_checks[] = {
    {
        "arch_boot_aps: smp_boot_aps() before arch_local_irq_enable()",
        "smp_boot_aps()",
        "arch_local_irq_enable()",
    },
    {
        "arch_boot_aps: arch_local_irq_enable() before ipi_ready_publish_and_count(0)",
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

static void suite_kernel_main_order(void)
{
    TEST_SUITE("double_state_publish kernel_main ordering");
    char path[512];
    snprintf(path, sizeof(path), "%s/kernel/core/main.c", OS01_KERNEL_SRC);
    char *buf = read_file(path);
    assert_true(buf != NULL);
    if (!buf) return;

    char *scope = extract_scope(buf, "int kernel_main(", "while (1) arch_cpu_halt();");
    assert_true(scope != NULL);
    if (scope) {
        assert_true(run_order_checks(scope, k_kernel_main_checks,
                                     sizeof(k_kernel_main_checks) / sizeof(k_kernel_main_checks[0]),
                                     "kernel_main"));
        free(scope);
    }
    free(buf);
}

static void suite_aarch64_boot_order(void)
{
    TEST_SUITE("double_state_publish aarch64 boot scoped ordering");
    char path[512];
    snprintf(path, sizeof(path), "%s/kernel/arch/aarch64/boot/main.c", OS01_KERNEL_SRC);
    char *buf = read_file(path);
    assert_true(buf != NULL);
    if (!buf) return;

    /* 1. arch_boot_percpu scope */
    char *scope_percpu = extract_scope(buf, "void arch_boot_percpu(void)", "smp_starting_enter();\n}");
    assert_true(scope_percpu != NULL);
    if (scope_percpu) {
        assert_true(run_order_checks(scope_percpu, k_percpu_checks,
                                     sizeof(k_percpu_checks) / sizeof(k_percpu_checks[0]),
                                     "arch_boot_percpu"));
        free(scope_percpu);
    }

    /* 2. arch_boot_aps scope */
    char *scope_aps = extract_scope(buf, "void arch_boot_aps(void)", "aarch64_shootdown_probe();\n    }");
    assert_true(scope_aps != NULL);
    if (scope_aps) {
        assert_true(run_order_checks(scope_aps, k_aps_checks,
                                     sizeof(k_aps_checks) / sizeof(k_aps_checks[0]),
                                     "arch_boot_aps"));
        free(scope_aps);
    }

    /* Substring sanity */
    assert_true(find_idx(buf, "ipi_ready_publish_and_count(0)") >= 0);
    assert_true(find_idx(buf, "smp_starting_enter()") >= 0);
    free(buf);
}

static void suite_ap_tail_order(void)
{
    TEST_SUITE("double_state_publish smp.c AP tail ordering");
    char path[512];
    snprintf(path, sizeof(path), "%s/kernel/arch/aarch64/smp/smp.c", OS01_KERNEL_SRC);
    char *buf = read_file(path);
    assert_true(buf != NULL);
    if (!buf) return;
    assert_true(run_order_checks(buf, k_smp_checks,
                                 sizeof(k_smp_checks) / sizeof(k_smp_checks[0]),
                                 "smp.c"));
    free(buf);
}

static void suite_negative_scope_checks(void)
{
    TEST_SUITE("double_state_publish negative & swap test fixtures");

    /* 1. Swapped caller order in kernel_main must FAIL */
    const char *bad_caller = "int kernel_main(void) { arch_boot_aps(); arch_boot_percpu(); }";
    assert_false(run_order_checks(bad_caller, k_kernel_main_checks,
                                  sizeof(k_kernel_main_checks) / sizeof(k_kernel_main_checks[0]),
                                  "bad_caller"));

    /* 2. Swapped definition order in file must still PASS with scoped checks */
    const char *swapped_defs =
        "void arch_boot_aps(void) {\n"
        "    smp_boot_aps();\n"
        "    arch_local_irq_enable();\n"
        "    ipi_ready_publish_and_count(0);\n"
        "}\n"
        "void arch_boot_percpu(void) {\n"
        "    percpu_install_gs(0);\n"
        "    percpu_init(0, mpidr_bsp);\n"
        "    __atomic_store_n(&percpu_data[0].online, 1, __ATOMIC_RELEASE);\n"
        "    __atomic_store_n(&num_cpus, dtb_cpu_count(), __ATOMIC_RELEASE);\n"
        "    smp_starting_enter();\n"
        "}\n"
        "int kernel_main(void) {\n"
        "    arch_boot_percpu();\n"
        "    arch_boot_aps();\n"
        "    while (1) arch_cpu_halt();\n"
        "}\n";

    char *scope_main = extract_scope(swapped_defs, "int kernel_main(", "while (1) arch_cpu_halt();");
    assert_true(scope_main != NULL);
    assert_true(run_order_checks(scope_main, k_kernel_main_checks, 1, "swapped_main"));
    free(scope_main);

    char *scope_percpu = extract_scope(swapped_defs, "void arch_boot_percpu(void)", "smp_starting_enter();\n}");
    assert_true(scope_percpu != NULL);
    assert_true(run_order_checks(scope_percpu, k_percpu_checks,
                                 sizeof(k_percpu_checks) / sizeof(k_percpu_checks[0]), "swapped_percpu"));
    free(scope_percpu);

    char *scope_aps = extract_scope(swapped_defs, "void arch_boot_aps(void)", "ipi_ready_publish_and_count(0);\n}");
    assert_true(scope_aps != NULL);
    assert_true(run_order_checks(scope_aps, k_aps_checks,
                                 sizeof(k_aps_checks) / sizeof(k_aps_checks[0]), "swapped_aps"));
    free(scope_aps);
}

int main(void)
{
    suite_kernel_main_order();
    suite_aarch64_boot_order();
    suite_ap_tail_order();
    suite_negative_scope_checks();
    printf("\ntest_double_state_publish: %d total, %d passed, %d failed\n",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
