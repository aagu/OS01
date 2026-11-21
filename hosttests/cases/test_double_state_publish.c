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
 * Order constraints asserted (unified SMP boot, spec 2026-11-19):
 *
 *   kernel_main (kernel/core/main.c):
 *     1. arch_boot_percpu() BEFORE arch_boot_aps()
 *
 *   arch_boot_aps (kernel/core/smp.c — the unified orchestrator):
 *     2. arch_smp_boot_aps(num_cpus, smp_ap_common_entry, percpu_data)
 *        BEFORE the [boot-1] complete marker
 *     3. the complete marker BEFORE arch_smp_post_boot_hook(num_cpus)
 *     4. arch_smp_post_boot_hook(num_cpus) BEFORE subsys_prepare_cpus()
 *
 *   smp_ap_common_entry (kernel/core/smp.c — the unified AP state machine):
 *     5. arch_smp_ap_cpu_init BEFORE the online=1 release store
 *     6. online=1 release BEFORE arch_smp_ap_post_online
 *     7. arch_smp_ap_post_online BEFORE arch_local_irq_enable()
 *     8. arch_local_irq_enable() BEFORE arch_smp_ap_publish_ipi_ready
 *     9. arch_smp_ap_publish_ipi_ready BEFORE arch_smp_ap_enter_idle
 *
 *   arch_boot_percpu (kernel/arch/aarch64/boot/main.c):
 *    10. percpu_install_gs(0) BEFORE percpu_init(0, mpidr_bsp)
 *    11. percpu_init(0, mpidr_bsp) BEFORE release store to percpu_data[0].online
 *    12. release store to percpu_data[0].online BEFORE release store to num_cpus
 *    13. release store to num_cpus BEFORE smp_starting_enter()
 *
 *   arch_boot_runtime (kernel/arch/aarch64/boot/main.c):
 *    14. arch_local_irq_enable() BEFORE ipi_ready_publish_and_count(0)
 *    15. ipi_ready_publish_and_count(0) BEFORE aarch64_shootdown_probe()
 *
 *   aarch64 smp.c hooks (kernel/arch/aarch64/smp/smp.c):
 *    16. cpu_init: page-table AP verify is the local-init tail
 *    17. post_online: boot ACK BEFORE the go wait's gic_target_bit_init
 *    18. publish_ipi_ready: ISB BEFORE ipi_ready_publish_and_count
 *
 *   x86_64_boot_percpu (kernel/arch/x86_64/smp/boot.c):
 *    19. percpu_init BEFORE the BSP online release publication
 *    20. BSP online release BEFORE num_cpus publication
 *
 *   x86 AP hooks (kernel/arch/x86_64/smp/smp.c):
 *    21. cpu_init: lgdt/lidt/selectors/TSS BEFORE lapic_configure_nmi
 *    22. post_online: tsc_boot written BEFORE tsc_sampled release
 *    23. enter_idle: scheduler_ok release is the idle-loop entry
 *
 *   x86 arch_smp_boot_aps (kernel/arch/x86_64/smp/smp.c):
 *    24. residual-state invariant check BEFORE INIT-SIPI-SIPI
 *    25. SIPI BEFORE bounded wait for online
 *    26. online wait BEFORE tsc_sync_go release
 *    27. sampled wait BEFORE bounded wait for scheduler_ok
 *    28. scheduler_ok wait BEFORE the ready marker; the trampoline
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

/* kernel/core/smp.c::arch_boot_aps — the unified BSP orchestrator. */
static const order_check_t k_core_aps_checks[] = {
    {
        "core smp.c: arch_smp_boot_aps before the complete marker",
        "arch_smp_boot_aps(num_cpus, smp_ap_common_entry, percpu_data)",
        "[boot-1] complete discovered=%s ready=%s",
    },
    {
        "core smp.c: complete marker before arch_smp_post_boot_hook",
        "[boot-1] complete discovered=%s ready=%s",
        "arch_smp_post_boot_hook(num_cpus)",
    },
    {
        "core smp.c: arch_smp_post_boot_hook before subsys_prepare_cpus",
        "arch_smp_post_boot_hook(num_cpus)",
        "subsys_prepare_cpus()",
    },
};

/* kernel/core/smp.c::smp_ap_common_entry — the unified AP state machine. */
static const order_check_t k_ap_common_checks[] = {
    {
        "common entry: arch_smp_ap_cpu_init before online=1 release",
        "arch_smp_ap_cpu_init(cpu_id, cpu);",
        "&cpu->online, 1u, __ATOMIC_RELEASE",
    },
    {
        "common entry: online=1 release before arch_smp_ap_post_online",
        "&cpu->online, 1u, __ATOMIC_RELEASE",
        "arch_smp_ap_post_online(cpu_id, cpu)",
    },
    {
        "common entry: arch_smp_ap_post_online before arch_local_irq_enable",
        "arch_smp_ap_post_online(cpu_id, cpu)",
        "arch_local_irq_enable();",
    },
    {
        "common entry: arch_local_irq_enable before arch_smp_ap_publish_ipi_ready",
        "arch_local_irq_enable();",
        "arch_smp_ap_publish_ipi_ready(cpu_id, cpu)",
    },
    {
        "common entry: arch_smp_ap_publish_ipi_ready before arch_smp_ap_enter_idle",
        "arch_smp_ap_publish_ipi_ready(cpu_id, cpu)",
        "arch_smp_ap_enter_idle(cpu_id, cpu)",
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

/* kernel/arch/aarch64/boot/main.c::arch_boot_runtime keeps the IRQ ->
 * IPI-ACK -> shootdown-probe ordering (stage 8.2 boundary). */
static const order_check_t k_a64_runtime_checks[] = {
    {
        "arch_boot_runtime: arch_local_irq_enable before ipi_ready_publish_and_count(0)",
        "arch_local_irq_enable();",
        "ipi_ready_publish_and_count(0)",
    },
    {
        "arch_boot_runtime: ipi_ready_publish_and_count(0) before aarch64_shootdown_probe",
        "ipi_ready_publish_and_count(0)",
        "aarch64_shootdown_probe()",
    },
};

/* aarch64 hook bodies (kernel/arch/aarch64/smp/smp.c). */
static const order_check_t k_a64_cpu_init_checks[] = {
    {
        "aarch64 cpu_init: gic_cpu_init before gic_target_bit_init",
        "gic_cpu_init();",
        "gic_target_bit_init(cpu_id);",
    },
    {
        "aarch64 cpu_init: percpu_init before the page-table AP verify",
        "percpu_init(cpu_id, mpidr_ap)",
        "aarch64_page_table_ap_verify(cpu_id)",
    },
};

static const order_check_t k_a64_post_online_checks[] = {
    {
        "aarch64 post_online: boot ACK before go wait",
        "boot_online_set(cpu_id)",
        "boot_go_get(cpu_id)",
    },
};

static const order_check_t k_a64_publish_checks[] = {
    {
        "aarch64 publish_ipi_ready: ISB before ipi_ready_publish_and_count",
        "__asm__ __volatile__(\"isb\" ::: \"memory\")",
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

/* BOOT-1: x86 AP hook bodies (smp.c). */
static const order_check_t k_x86_cpu_init_checks[] = {
    {
        "x86 cpu_init: lapic_configure_nmi after set_tss64/load_TR",
        "set_tss64(",
        "lapic_configure_nmi();",
    },
    {
        "x86 cpu_init: load_TR before lapic_configure_nmi",
        "load_TR(",
        "lapic_configure_nmi();",
    },
    {
        "x86 cpu_init: fault injection ends cpu_init (before online)",
        "lapic_configure_nmi();",
        "boot1_fault_before_online(cpu_id)",
    },
};

static const order_check_t k_x86_post_online_checks[] = {
    {
        "x86 post_online: tsc_boot written before tsc_sampled release",
        "cpu->tsc_boot = rdtsc()",
        "&cpu->tsc_sampled, 1, __ATOMIC_RELEASE",
    },
};

static const order_check_t k_x86_publish_checks[] = {
    {
        "x86 publish_ipi_ready: ipi_ready_publish_ap call site",
        "ipi_ready_publish_ap(cpu_id)",
        "boot1_fault_after_ipi_ready(cpu_id)",
    },
};

static const order_check_t k_x86_enter_idle_checks[] = {
    {
        "x86 enter_idle: scheduler_ok release before the hlt loop",
        "&cpu->scheduler_ok, 1, __ATOMIC_RELEASE",
        "arch_cpu_halt()",
    },
};

/* BOOT-1: x86 BSP two-stage wait ordering (smp.c arch_smp_boot_aps). */
static const order_check_t k_x86_bsp_checks[] = {
    {
        "x86 arch_smp_boot_aps: residual-state invariant check before SIPI",
        "__atomic_load_n(&per_cpu_data[i].scheduler_ok, __ATOMIC_ACQUIRE",
        "ICR_DELIVERY_INIT",
    },
    {
        "x86 arch_smp_boot_aps: SIPI before bounded online wait",
        "ICR_DELIVERY_STARTUP",
        "x86_smp_wait_u32_acquire(&per_cpu_data[i].online",
    },
    {
        "x86 arch_smp_boot_aps: online wait before tsc_sync_go release",
        "x86_smp_wait_u32_acquire(&per_cpu_data[i].online",
        "&per_cpu_data[i].tsc_sync_go, 1, __ATOMIC_RELEASE",
    },
    {
        "x86 arch_smp_boot_aps: sampled wait before scheduler_ok wait",
        "&per_cpu_data[i].tsc_sampled",
        "x86_smp_wait_u32_acquire(&per_cpu_data[i].scheduler_ok",
    },
    {
        "x86 arch_smp_boot_aps: scheduler_ok wait before ready marker",
        "x86_smp_wait_u32_acquire(&per_cpu_data[i].scheduler_ok",
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

static void suite_core_smp_order(void)
{
    TEST_SUITE("double_state_publish core/smp.c orchestrator + common entry");
    char path[512];
    snprintf(path, sizeof(path), "%s/kernel/core/smp.c", OS01_KERNEL_SRC);
    char *buf = read_file(path);
    assert_true(buf != NULL);
    if (!buf) return;

    /* 1. the BSP orchestrator scope */
    char *scope_aps = extract_scope(buf, "void arch_boot_aps(void)",
                                    "subsys_prepare_cpus();");
    assert_true(scope_aps != NULL);
    if (scope_aps) {
        assert_true(run_order_checks(scope_aps, k_core_aps_checks,
                                     sizeof(k_core_aps_checks) / sizeof(k_core_aps_checks[0]),
                                     "arch_boot_aps (core)"));
        free(scope_aps);
    }

    /* 2. the unified AP state machine scope */
    char *scope_entry = extract_scope(buf, "void smp_ap_common_entry(",
                                      "arch_smp_ap_enter_idle(cpu_id, cpu);");
    assert_true(scope_entry != NULL);
    if (scope_entry) {
        assert_true(run_order_checks(scope_entry, k_ap_common_checks,
                                     sizeof(k_ap_common_checks) / sizeof(k_ap_common_checks[0]),
                                     "smp_ap_common_entry"));
        free(scope_entry);
    }

    /* Substring sanity: the unified facade symbols live here. */
    assert_true(find_idx(buf, "smp_ap_common_entry") >= 0);
    assert_true(find_idx(buf, "[boot-1] FATAL cpu=%s stage=%s") >= 0);
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

    /* 2. arch_boot_runtime scope (the retired arch_boot_aps is gone:
     * its ordering moved to kernel/core/smp.c, asserted in
     * suite_core_smp_order). */
    char *scope_runtime = extract_scope(buf, "void arch_boot_runtime(void)",
                                        "aarch64_shootdown_probe();\n    }");
    assert_true(scope_runtime != NULL);
    if (scope_runtime) {
        assert_true(run_order_checks(scope_runtime, k_a64_runtime_checks,
                                     sizeof(k_a64_runtime_checks) / sizeof(k_a64_runtime_checks[0]),
                                     "arch_boot_runtime"));
        free(scope_runtime);
    }

    /* Substring sanity */
    assert_true(find_idx(buf, "ipi_ready_publish_and_count(0)") >= 0);
    assert_true(find_idx(buf, "smp_starting_enter()") >= 0);
    free(buf);
}

static void suite_ap_tail_order(void)
{
    TEST_SUITE("double_state_publish smp.c AP hook ordering");
    char path[512];
    snprintf(path, sizeof(path), "%s/kernel/arch/aarch64/smp/smp.c", OS01_KERNEL_SRC);
    char *buf = read_file(path);
    assert_true(buf != NULL);
    if (!buf) return;

    char *cpu_init = extract_scope(buf, "void arch_smp_ap_cpu_init(",
                                   "aarch64_page_table_ap_verify(cpu_id)");
    assert_true(cpu_init != NULL);
    if (cpu_init) {
        assert_true(run_order_checks(cpu_init, k_a64_cpu_init_checks,
                                     sizeof(k_a64_cpu_init_checks) / sizeof(k_a64_cpu_init_checks[0]),
                                     "arch_smp_ap_cpu_init (aarch64)"));
        free(cpu_init);
    }

    char *post_online = extract_scope(buf, "void arch_smp_ap_post_online(",
                                      "boot_go_get(cpu_id)");
    assert_true(post_online != NULL);
    if (post_online) {
        assert_true(run_order_checks(post_online, k_a64_post_online_checks,
                                     sizeof(k_a64_post_online_checks) / sizeof(k_a64_post_online_checks[0]),
                                     "arch_smp_ap_post_online (aarch64)"));
        free(post_online);
    }

    char *publish = extract_scope(buf, "void arch_smp_ap_publish_ipi_ready(",
                                  "ipi_ready_publish_and_count(cpu_id)");
    assert_true(publish != NULL);
    if (publish) {
        assert_true(run_order_checks(publish, k_a64_publish_checks,
                                     sizeof(k_a64_publish_checks) / sizeof(k_a64_publish_checks[0]),
                                     "arch_smp_ap_publish_ipi_ready (aarch64)"));
        free(publish);
    }

    /* The retired shapes must stay retired. */
    assert_true(find_idx(buf, "void secondary_idle") < 0);
    assert_true(find_idx(buf, "uint32_t smp_boot_aps(void)") < 0);
    assert_true(find_idx(buf, "status=DEGRADED") < 0);
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
        "    int rc = arch_smp_boot_aps(num_cpus, smp_ap_common_entry, percpu_data);\n"
        "    log_info(\"[boot-1] complete discovered=%s ready=%s\\n\", \"1\", \"1\");\n"
        "    arch_smp_post_boot_hook(num_cpus);\n"
        "    subsys_prepare_cpus();\n"
        "}\n"
        "void smp_ap_common_entry(uint32_t cpu_id, struct percpu *cpu) {\n"
        "    arch_smp_ap_cpu_init(cpu_id, cpu);\n"
        "    __atomic_store_n(&cpu->online, 1u, __ATOMIC_RELEASE);\n"
        "    arch_smp_ap_post_online(cpu_id, cpu);\n"
        "    arch_local_irq_enable();\n"
        "    arch_mb();\n"
        "    arch_smp_ap_publish_ipi_ready(cpu_id, cpu);\n"
        "    arch_smp_ap_enter_idle(cpu_id, cpu);\n"
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

    char *scope_aps = extract_scope(swapped_defs, "void arch_boot_aps(void)",
                                    "subsys_prepare_cpus();");
    assert_true(scope_aps != NULL);
    assert_true(run_order_checks(scope_aps, k_core_aps_checks,
                                 sizeof(k_core_aps_checks) / sizeof(k_core_aps_checks[0]), "swapped_aps"));
    free(scope_aps);

    char *scope_entry = extract_scope(swapped_defs, "void smp_ap_common_entry(",
                                      "arch_smp_ap_enter_idle(cpu_id, cpu);");
    assert_true(scope_entry != NULL);
    assert_true(run_order_checks(scope_entry, k_ap_common_checks,
                                 sizeof(k_ap_common_checks) / sizeof(k_ap_common_checks[0]), "swapped_entry"));
    free(scope_entry);

    char *scope_percpu = extract_scope(swapped_defs, "void arch_boot_percpu(void)", "smp_starting_enter();\n}");
    assert_true(scope_percpu != NULL);
    assert_true(run_order_checks(scope_percpu, k_percpu_checks,
                                 sizeof(k_percpu_checks) / sizeof(k_percpu_checks[0]), "swapped_percpu"));
    free(scope_percpu);
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
    TEST_SUITE("double_state_publish x86 smp.c AP hooks + BSP two-stage order");
    char path[512];
    snprintf(path, sizeof(path), "%s/kernel/arch/x86_64/smp/smp.c", OS01_KERNEL_SRC);
    char *buf = read_file(path);
    assert_true(buf != NULL);
    if (!buf) return;

    /* AP hook bodies. */
    char *cpu_init = extract_scope(buf, "void arch_smp_ap_cpu_init(",
                                   "boot1_fault_before_online(cpu_id)");
    assert_true(cpu_init != NULL);
    if (cpu_init) {
        assert_true(run_order_checks(cpu_init, k_x86_cpu_init_checks,
                                     sizeof(k_x86_cpu_init_checks) / sizeof(k_x86_cpu_init_checks[0]),
                                     "arch_smp_ap_cpu_init (x86)"));
        free(cpu_init);
    }

    char *post_online = extract_scope(buf, "void arch_smp_ap_post_online(",
                                      "&cpu->tsc_sampled, 1, __ATOMIC_RELEASE");
    assert_true(post_online != NULL);
    if (post_online) {
        assert_true(run_order_checks(post_online, k_x86_post_online_checks,
                                     sizeof(k_x86_post_online_checks) / sizeof(k_x86_post_online_checks[0]),
                                     "arch_smp_ap_post_online (x86)"));
        free(post_online);
    }

    char *publish = extract_scope(buf, "void arch_smp_ap_publish_ipi_ready(",
                                  "boot1_fault_after_ipi_ready(cpu_id)");
    assert_true(publish != NULL);
    if (publish) {
        assert_true(run_order_checks(publish, k_x86_publish_checks,
                                     sizeof(k_x86_publish_checks) / sizeof(k_x86_publish_checks[0]),
                                     "arch_smp_ap_publish_ipi_ready (x86)"));
        free(publish);
    }

    char *enter_idle = extract_scope(buf, "void arch_smp_ap_enter_idle(",
                                     "arch_cpu_halt()");
    assert_true(enter_idle != NULL);
    if (enter_idle) {
        assert_true(run_order_checks(enter_idle, k_x86_enter_idle_checks,
                                     sizeof(k_x86_enter_idle_checks) / sizeof(k_x86_enter_idle_checks[0]),
                                     "arch_smp_ap_enter_idle (x86)"));
        free(enter_idle);
    }

    /* BSP: invariant check → SIPI → online wait → go/sample →
     * scheduler wait → ready marker.  The trampoline fill is a single
     * site inside the loop ahead of the SIPI sequence, so the NEXT
     * CPU's fill happens only after this CPU's scheduler_ok wait. */
    char *scope_bsp = extract_scope(buf, "int arch_smp_boot_aps(",
                                    "[boot-1] ready cpu=");
    assert_true(scope_bsp != NULL);
    if (scope_bsp) {
        size_t n = sizeof(k_x86_bsp_checks) / sizeof(k_x86_bsp_checks[0]);
        assert_true(run_order_checks(scope_bsp, k_x86_bsp_checks, n,
                                     "arch_smp_boot_aps"));
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

    /* The retired x86 orchestrator shapes must stay retired. */
    assert_true(find_idx(buf, "void ap_entry(void)") < 0);
    assert_true(find_idx(buf, "void smp_boot_aps(void)") < 0);
    assert_true(find_idx(buf, "[boot-1] complete") < 0);
    free(buf);
}

/* BOOT-1 Task 2 consumer-acquire contract (final-review finding): the
 * sched consumers the plan names must read runtime online with acquire
 * — pointer-based accesses (dst->online / rq->online) included.  The
 * Task 2 grep audit missed exactly these, so the contract is pinned in
 * source text, not in a grep. */
static void suite_sched_consumer_acquire_contract(void)
{
    TEST_SUITE("double_state_publish sched consumer acquire contract");
    const char *checks[][3] = {
        /* file, function scope start, needle that must appear after it */
        { "kernel/sched/balance.c", "void sched_notify_remote_cpu(",
          "__atomic_load_n(&dst->online, __ATOMIC_ACQUIRE)" },
        { "kernel/sched/kthread.c", "int sched_alloc_and_publish_task(",
          "__atomic_load_n(&rq->online, __ATOMIC_ACQUIRE)" },
    };
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); ++i) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", OS01_KERNEL_SRC, checks[i][0]);
        char *buf = read_file(path);
        assert_true(buf != NULL);
        if (!buf) continue;
        long scope = find_idx(buf, checks[i][1]);
        long needle = find_idx(buf, checks[i][2]);
        assert_true(scope >= 0);
        assert_true(needle >= 0);
        if (scope >= 0 && needle >= 0)
            assert_true(needle > scope);
        free(buf);
    }
}

int main(void)
{
    suite_kernel_main_order();
    suite_core_smp_order();
    suite_aarch64_boot_order();
    suite_ap_tail_order();
    suite_sched_consumer_acquire_contract();
    suite_x86_boot_order();
    suite_x86_smp_order();
    suite_negative_scope_checks();
    printf("\ntest_double_state_publish: %d total, %d passed, %d failed\n",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
