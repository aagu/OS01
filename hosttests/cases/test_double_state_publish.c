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
 *
 *   BOOT-1 publication contracts (CPU lifecycle plan Task 1):
 *
 *   aarch64 secondary_idle (kernel/arch/aarch64/smp/smp.c):
 *    10. page-table AP verify BEFORE runtime online release publication
 *    11. runtime online release BEFORE boot ACK (boot_online_set)
 *    12. boot ACK BEFORE ipi_ready_publish_and_count (via IRQ + ISB)
 *        — NO_ACK suppresses only the ACK, never runtime online.
 *
 *   x86_64_boot_percpu (kernel/arch/x86_64/smp/boot.c):
 *    13. percpu_init BEFORE the BSP online release publication
 *    14. BSP online release BEFORE num_cpus publication
 *
 *   x86 ap_entry (kernel/arch/x86_64/smp/smp.c):
 *    15. online release BEFORE the TSC go/sample handshake
 *    16. tsc_boot written BEFORE tsc_sampled release
 *    17. IRQ enable BEFORE ipi_ready_publish_ap BEFORE scheduler_ok release
 *
 *   x86 smp_boot_aps (kernel/arch/x86_64/smp/smp.c):
 *    18. residual-state invariant check BEFORE INIT-SIPI-SIPI
 *    19. SIPI BEFORE bounded wait for online
 *    20. online wait BEFORE tsc_sync_go release
 *    21. sampled wait BEFORE bounded wait for scheduler_ok
 *    22. scheduler_ok wait BEFORE the ready marker; the trampoline
 *        data fill is a single site inside the loop, so filling for
 *        the NEXT CPU only happens after this CPU's scheduler_ok wait.
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
    {
        "smp.c AP: page-table verify before runtime online release",
        "aarch64_page_table_ap_verify(cpu_id)",
        "&percpu_data[cpu_id].online, 1, __ATOMIC_RELEASE",
    },
    {
        "smp.c AP: runtime online release before boot ACK",
        "&percpu_data[cpu_id].online, 1, __ATOMIC_RELEASE",
        "boot_online_set(cpu_id)",
    },
    {
        "smp.c AP: boot ACK before ipi_ready_publish_and_count",
        "boot_online_set(cpu_id)",
        "ipi_ready_publish_and_count(cpu_id)",
    },
};

/* BOOT-1: x86 BSP construction/launch ordering (boot.c). */
static const order_check_t k_x86_boot_percpu_checks[] = {
    {
        "x86 boot.c: percpu_init before BSP online release",
        "percpu_init(cpu_idx, apic_info.lapics[i].apic_id)",
        "&percpu_data[0].online, 1, __ATOMIC_RELEASE",
    },
    {
        "x86 boot.c: BSP online release before num_cpus publication",
        "&percpu_data[0].online, 1, __ATOMIC_RELEASE",
        "num_cpus = cpu_idx",
    },
};

/* BOOT-1: x86 AP publication ordering (smp.c ap_entry). */
static const order_check_t k_x86_ap_checks[] = {
    {
        "x86 ap_entry: online release before TSC go/sample handshake",
        "&cpu->online, 1, __ATOMIC_RELEASE",
        "cpu->tsc_boot = rdtsc()",
    },
    {
        "x86 ap_entry: tsc_boot written before tsc_sampled release",
        "cpu->tsc_boot = rdtsc()",
        "&cpu->tsc_sampled, 1, __ATOMIC_RELEASE",
    },
    {
        "x86 ap_entry: IRQ enable before ipi_ready_publish_ap",
        "arch_local_irq_enable()",
        "ipi_ready_publish_ap(cpu->cpu_id)",
    },
    {
        "x86 ap_entry: ipi_ready_publish_ap before scheduler_ok release",
        "ipi_ready_publish_ap(cpu->cpu_id)",
        "&cpu->scheduler_ok, 1, __ATOMIC_RELEASE",
    },
};

/* BOOT-1: x86 BSP two-stage wait ordering (smp.c smp_boot_aps). */
static const order_check_t k_x86_bsp_checks[] = {
    {
        "x86 smp_boot_aps: residual-state invariant check before SIPI",
        "__atomic_load_n(&percpu_data[i].scheduler_ok, __ATOMIC_ACQUIRE",
        "ICR_DELIVERY_INIT",
    },
    {
        "x86 smp_boot_aps: SIPI before bounded online wait",
        "ICR_DELIVERY_STARTUP",
        "x86_smp_wait_u32_acquire(&percpu_data[i].online",
    },
    {
        "x86 smp_boot_aps: online wait before tsc_sync_go release",
        "x86_smp_wait_u32_acquire(&percpu_data[i].online",
        "&percpu_data[i].tsc_sync_go, 1, __ATOMIC_RELEASE",
    },
    {
        "x86 smp_boot_aps: sampled wait before scheduler_ok wait",
        "&percpu_data[i].tsc_sampled",
        "x86_smp_wait_u32_acquire(&percpu_data[i].scheduler_ok",
    },
    {
        "x86 smp_boot_aps: scheduler_ok wait before ready marker",
        "x86_smp_wait_u32_acquire(&percpu_data[i].scheduler_ok",
        "[boot-1] ready cpu=",
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

static void suite_x86_boot_order(void)
{
    TEST_SUITE("double_state_publish x86 boot.c BSP publication order");
    char path[512];
    snprintf(path, sizeof(path), "%s/kernel/arch/x86_64/smp/boot.c", OS01_KERNEL_SRC);
    char *buf = read_file(path);
    assert_true(buf != NULL);
    if (!buf) return;

    char *scope = extract_scope(buf, "void x86_64_boot_percpu(void)",
                                "num_cpus = cpu_idx;");
    assert_true(scope != NULL);
    if (scope) {
        assert_true(run_order_checks(scope, k_x86_boot_percpu_checks,
                                     sizeof(k_x86_boot_percpu_checks) / sizeof(k_x86_boot_percpu_checks[0]),
                                     "x86_64_boot_percpu"));
        free(scope);
    }
    free(buf);
}

static void suite_x86_smp_order(void)
{
    TEST_SUITE("double_state_publish x86 smp.c AP tail + BSP two-stage order");
    char path[512];
    snprintf(path, sizeof(path), "%s/kernel/arch/x86_64/smp/smp.c", OS01_KERNEL_SRC);
    char *buf = read_file(path);
    assert_true(buf != NULL);
    if (!buf) return;

    /* AP tail: online → TSC handshake → IRQ → IPI → scheduler_ok. */
    char *scope_ap = extract_scope(buf, "void ap_entry(void)",
                                   "&cpu->scheduler_ok, 1, __ATOMIC_RELEASE);");
    assert_true(scope_ap != NULL);
    if (scope_ap) {
        assert_true(run_order_checks(scope_ap, k_x86_ap_checks,
                                     sizeof(k_x86_ap_checks) / sizeof(k_x86_ap_checks[0]),
                                     "ap_entry"));
        free(scope_ap);
    }

    /* BSP: invariant check → SIPI → online wait → go/sample →
     * scheduler wait → ready marker.  The trampoline fill is a single
     * site inside the loop ahead of the SIPI sequence, so the NEXT
     * CPU's fill happens only after this CPU's scheduler_ok wait. */
    char *scope_bsp = extract_scope(buf, "void smp_boot_aps(void)",
                                    "[boot-1] complete");
    assert_true(scope_bsp != NULL);
    if (scope_bsp) {
        size_t n = sizeof(k_x86_bsp_checks) / sizeof(k_x86_bsp_checks[0]);
        assert_true(run_order_checks(scope_bsp, k_x86_bsp_checks, n,
                                     "smp_boot_aps"));
        /* Single trampoline fill site, inside the loop before SIPI. */
        long fill = find_idx(scope_bsp, "tdata->gs_base");
        long sipi = find_idx(scope_bsp, "ICR_DELIVERY_STARTUP");
        long next_fill = -1;
        if (fill >= 0) {
            const char *rest = scope_bsp + fill + 1;
            const char *p2 = strstr(rest, "tdata->gs_base");
            next_fill = p2 ? (long)(p2 - scope_bsp) : -1;
        }
        assert_true(fill >= 0);
        assert_true(next_fill < 0);   /* exactly one fill site */
        assert_true(sipi > fill);
        free(scope_bsp);
    }
    free(buf);
}

int main(void)
{
    suite_kernel_main_order();
    suite_aarch64_boot_order();
    suite_ap_tail_order();
    suite_x86_boot_order();
    suite_x86_smp_order();
    suite_negative_scope_checks();
    printf("\ntest_double_state_publish: %d total, %d passed, %d failed\n",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
