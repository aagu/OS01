/* @os01-test
 * id: test_smp_boot_unified
 * layer: host
 * subsystem: boot
 * timing: strict
 * tier: pr
 * link: smp-boot-unified
 * scenarios: []
 */
/* hosttests/cases/test_smp_boot_unified.c — Unified SMP boot (spec
 * 2026-11-19) host unit tests for the REAL kernel/core/smp.c.
 *
 * The production object is compiled from kernel/core/smp.c with the
 * preinclude mock smp_boot_unified_runtime.h (traceable
 * arch_local_irq_enable / arch_mb); every other machine-boundary symbol
 * is defined in this TU as a test double.  The orchestrator
 * (arch_boot_aps), the fail-closed policy and the AP state machine
 * (smp_ap_common_entry) under test are the production code — never a
 * reimplementation.
 */

#include "test_framework.h"
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>
#include <errno.h>

#include <arch/smp.h>
#include <arch/boot.h>
#include <percpu/percpu.h>
#include <core/boot_failure.h>
#include <log/log.h>

/* ── Definitions the production object expects from the environment ── */

int g_log_level = 6;
percpu_t percpu_data[NR_CPUS];
uint32_t num_cpus;

/* ── Log capture ─────────────────────────────────────────────── */

static char log_capture_buf[16384];
static size_t log_capture_len = 0;

static void log_capture_reset(void)
{
    log_capture_buf[0] = '\0';
    log_capture_len = 0;
}

void _log_info_impl(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    size_t l = strlen(buf);
    if (log_capture_len + l < sizeof(log_capture_buf) - 1) {
        memcpy(log_capture_buf + log_capture_len, buf, l);
        log_capture_len += l;
        log_capture_buf[log_capture_len] = '\0';
    }
}

void _log_err_impl(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    size_t l = strlen(buf);
    if (log_capture_len + l < sizeof(log_capture_buf) - 1) {
        memcpy(log_capture_buf + log_capture_len, buf, l);
        log_capture_len += l;
        log_capture_buf[log_capture_len] = '\0';
    }
}

void _log_warn_impl(const char *fmt, ...)
{
    (void)fmt;
}

/* ── boot_failure_stop capture ───────────────────────────────── */

static int stop_calls = 0;
static char last_stop_stage[32];
static char last_stop_component[64];
static int64_t last_stop_rc = 0;
static int last_stop_phase = 0;
static int last_stop_cpu = 0;
static jmp_buf stop_jmp;
static int jump_on_stop = 0;

__attribute__((noreturn))
void boot_failure_stop(const char *stage, const char *component,
                       int64_t rc, int phase, int cpu)
{
    stop_calls++;
    snprintf(last_stop_stage, sizeof(last_stop_stage), "%s", stage ? stage : "unknown");
    snprintf(last_stop_component, sizeof(last_stop_component), "%s", component ? component : "unknown");
    last_stop_rc = rc;
    last_stop_phase = phase;
    last_stop_cpu = cpu;

    if (jump_on_stop) {
        longjmp(stop_jmp, 1);
    }
    abort();
}

/* ── Architecture single-entry mock ──────────────────────────── */

static int mock_boot_calls = 0;
static uint32_t mock_boot_count_arg = 0;
static arch_smp_ap_entry_t mock_boot_entry_arg = 0;
static struct percpu *mock_boot_percpu_arg = 0;
static int mock_boot_rc = 0;
static int mock_status_null = 0;
static struct arch_smp_boot_status mock_status;

int arch_smp_boot_aps(uint32_t cpu_count, arch_smp_ap_entry_t entry,
                      struct percpu *per_cpu_data)
{
    mock_boot_calls++;
    mock_boot_count_arg = cpu_count;
    mock_boot_entry_arg = entry;
    mock_boot_percpu_arg = per_cpu_data;
    return mock_boot_rc;
}

const struct arch_smp_boot_status *arch_smp_last_status(void)
{
    if (mock_status_null) {
        return NULL;
    }
    return &mock_status;
}

/* ── AP lifecycle hook mocks (trace) ─────────────────────────── */

enum {
    TR_CPU_INIT = 0,
    TR_POST_ONLINE,
    TR_IRQ_ENABLE,
    TR_MB,
    TR_IPI_READY,
    TR_ENTER_IDLE,
    TR__COUNT
};

static int trace_buf[TR__COUNT * 4];
static int trace_online[TR__COUNT * 4];
static int trace_irq[TR__COUNT * 4];
static int trace_len = 0;
static int smp_test_irq_on = 0;

static void trace_push(int event, const struct percpu *cpu)
{
    if (trace_len >= TR__COUNT * 4) {
        assert_true(0);
        return;
    }
    trace_buf[trace_len] = event;
    trace_online[trace_len] = cpu ? (int)cpu->online : -1;
    trace_irq[trace_len] = smp_test_irq_on;
    trace_len++;
}

void arch_smp_ap_cpu_init(uint32_t cpu_id, struct percpu *cpu)
{
    (void)cpu_id;
    trace_push(TR_CPU_INIT, cpu);
}

void arch_smp_ap_post_online(uint32_t cpu_id, struct percpu *cpu)
{
    (void)cpu_id;
    trace_push(TR_POST_ONLINE, cpu);
}

void arch_local_irq_enable(void)
{
    trace_push(TR_IRQ_ENABLE, NULL);
    smp_test_irq_on = 1;
}

void smp_test_arch_mb(void)
{
    trace_push(TR_MB, NULL);
}

void arch_smp_ap_publish_ipi_ready(uint32_t cpu_id, struct percpu *cpu)
{
    (void)cpu_id;
    trace_push(TR_IPI_READY, cpu);
}

static jmp_buf idle_jmp;
static int idle_entered = 0;

void arch_smp_ap_enter_idle(uint32_t cpu_id, struct percpu *cpu)
{
    (void)cpu_id;
    trace_push(TR_ENTER_IDLE, cpu);
    idle_entered++;
    longjmp(idle_jmp, 1);
}

/* ── BSP post-boot sequence mocks ────────────────────────────── */

static int hook_calls = 0;
static uint32_t hook_arg = 0;
static int hook_seq = 0;
static int subsys_calls = 0;
static int subsys_seq = 0;
static int global_seq = 0;

__attribute__((noreturn))
static void smp_test_unreachable(const char *what)
{
    printf("  [FAIL] %s reached an unreachable path\n", what);
    abort();
}

void arch_smp_post_boot_hook(uint32_t online_cpus)
{
    hook_calls++;
    hook_arg = online_cpus;
    hook_seq = ++global_seq;
}

void subsys_prepare_cpus(void)
{
    subsys_calls++;
    subsys_seq = ++global_seq;
}

/* ── Reset ───────────────────────────────────────────────────── */

static void test_reset_all(void)
{
    memset(percpu_data, 0, sizeof(percpu_data));
    num_cpus = 4;
    log_capture_reset();
    stop_calls = 0;
    last_stop_stage[0] = '\0';
    last_stop_component[0] = '\0';
    last_stop_rc = 0;
    last_stop_phase = 0;
    last_stop_cpu = 0;
    jump_on_stop = 0;
    mock_boot_calls = 0;
    mock_boot_count_arg = 0;
    mock_boot_entry_arg = 0;
    mock_boot_percpu_arg = 0;
    mock_boot_rc = 0;
    mock_status_null = 0;
    memset(&mock_status, 0, sizeof(mock_status));
    trace_len = 0;
    smp_test_irq_on = 0;
    idle_entered = 0;
    hook_calls = 0;
    hook_arg = 0;
    hook_seq = 0;
    subsys_calls = 0;
    subsys_seq = 0;
    global_seq = 0;
}

/* Drive arch_boot_aps() with boot_failure_stop captured via longjmp. */
#define DRIVE_BOOT_APS()                                                   \
    do {                                                                   \
        jump_on_stop = 1;                                                  \
        if (setjmp(stop_jmp) == 0) {                                       \
            arch_boot_aps();                                               \
            smp_test_unreachable("arch_boot_aps returned");                \
        }                                                                  \
    } while (0)

/* ── 1. precondition: invalid num_cpus ───────────────────────── */

static void test_precondition_rejects_invalid_num_cpus(void)
{
    const uint32_t bad_counts[] = { 0u, (uint32_t)NR_CPUS + 1u };

    for (size_t k = 0; k < sizeof(bad_counts) / sizeof(bad_counts[0]); k++) {
        test_reset_all();
        num_cpus = bad_counts[k];
        DRIVE_BOOT_APS();

        assert_eq(1, stop_calls);
        assert_true(strcmp(last_stop_stage, "smp") == 0);
        assert_true(strcmp(last_stop_component, "invariant") == 0);
        assert_eq((long)-EINVAL, (long)last_stop_rc);
        assert_eq(-1, last_stop_phase);
        assert_eq(0, last_stop_cpu);
        assert_true(strstr(log_capture_buf,
                           "[boot-1] FATAL cpu=0 stage=invariant") != NULL);
        assert_eq(0, mock_boot_calls);
        assert_eq(0, hook_calls);
        assert_eq(0, subsys_calls);
    }
}

/* ── 2. precondition: BSP not online ─────────────────────────── */

static void test_precondition_rejects_bsp_offline(void)
{
    test_reset_all();
    num_cpus = 4;
    percpu_data[0].online = 0;
    DRIVE_BOOT_APS();

    assert_eq(1, stop_calls);
    assert_true(strcmp(last_stop_component, "invariant") == 0);
    assert_eq((long)-EINVAL, (long)last_stop_rc);
    assert_eq(0, last_stop_cpu);
    assert_true(strstr(log_capture_buf,
                       "[boot-1] FATAL cpu=0 stage=invariant") != NULL);
    assert_eq(0, mock_boot_calls);
}

/* ── 3. precondition: dirty AP residue ───────────────────────── */

static void test_precondition_rejects_dirty_ap(void)
{
    /* Residue: online == 1 on an AP that has not been brought up. */
    test_reset_all();
    num_cpus = 4;
    percpu_data[0].online = 1;
    percpu_data[2].online = 1;
    DRIVE_BOOT_APS();

    assert_eq(1, stop_calls);
    assert_true(strcmp(last_stop_component, "invariant") == 0);
    assert_eq((long)-EINVAL, (long)last_stop_rc);
    assert_eq(2, last_stop_cpu);
    assert_eq(0, mock_boot_calls);

    /* Residue: scheduler_ok == 1 on an AP. */
    test_reset_all();
    num_cpus = 4;
    percpu_data[0].online = 1;
    percpu_data[2].scheduler_ok = 1;
    DRIVE_BOOT_APS();

    assert_eq(1, stop_calls);
    assert_true(strcmp(last_stop_component, "invariant") == 0);
    assert_eq(2, last_stop_cpu);
    assert_eq(0, mock_boot_calls);
}

/* ── 4. single-CPU boot succeeds without touching the AP path ── */

static void test_single_cpu_boot_succeeds(void)
{
    test_reset_all();
    num_cpus = 1;
    percpu_data[0].online = 1;
    mock_boot_rc = 0;
    mock_status.requested = 1;
    mock_status.online = 1;
    mock_status.online_mask = 1u;

    arch_boot_aps();

    assert_eq(0, stop_calls);
    assert_eq(1, mock_boot_calls);
    assert_eq(1, (int)mock_boot_count_arg);
    assert_true(mock_boot_entry_arg == smp_ap_common_entry);
    assert_true(mock_boot_percpu_arg == percpu_data);
    assert_true(strstr(log_capture_buf,
                       "[boot-1] complete discovered=1 ready=1") != NULL);
    assert_eq(1, hook_calls);
    assert_eq(1, (int)hook_arg);
    assert_eq(1, subsys_calls);
    assert_true(hook_seq < subsys_seq);
}

/* ── 5. multicore orchestration: args and strict ordering ────── */

static void test_multicore_boot_order_and_args(void)
{
    test_reset_all();
    num_cpus = 4;
    percpu_data[0].online = 1;
    mock_boot_rc = 0;
    mock_status.requested = 4;
    mock_status.online = 4;
    mock_status.online_mask = 0xfu;

    arch_boot_aps();

    assert_eq(0, stop_calls);
    assert_eq(1, mock_boot_calls);
    assert_eq(4, (int)mock_boot_count_arg);
    assert_true(mock_boot_entry_arg == smp_ap_common_entry);
    assert_true(mock_boot_percpu_arg == percpu_data);
    assert_true(strstr(log_capture_buf,
                       "[boot-1] complete discovered=4 ready=4") != NULL);
    assert_eq(1, hook_calls);
    assert_eq(4, (int)hook_arg);
    assert_eq(1, subsys_calls);
    assert_true(hook_seq < subsys_seq);
}

/* ── 6. postcondition: NULL status or partial mask fails closed ─ */

static void test_postcondition_rejects_null_status_or_mask_mismatch(void)
{
    /* rc == 0 but arch_smp_last_status() == NULL. */
    test_reset_all();
    num_cpus = 4;
    percpu_data[0].online = 1;
    mock_boot_rc = 0;
    mock_status_null = 1;
    DRIVE_BOOT_APS();

    assert_eq(1, stop_calls);
    assert_true(strcmp(last_stop_component, "invariant") == 0);
    assert_eq((long)-EIO, (long)last_stop_rc);
    assert_eq(0, hook_calls);
    assert_eq(0, subsys_calls);

    /* rc == 0 but online under-reported. */
    test_reset_all();
    num_cpus = 4;
    percpu_data[0].online = 1;
    mock_boot_rc = 0;
    mock_status.requested = 4;
    mock_status.online = 3;
    mock_status.online_mask = 0xfu;
    DRIVE_BOOT_APS();

    assert_eq(1, stop_calls);
    assert_true(strcmp(last_stop_component, "invariant") == 0);
    assert_eq((long)-EIO, (long)last_stop_rc);
    assert_eq(0, hook_calls);
    assert_eq(0, subsys_calls);

    /* rc == 0 but online_mask missing a bit. */
    test_reset_all();
    num_cpus = 4;
    percpu_data[0].online = 1;
    mock_boot_rc = 0;
    mock_status.requested = 4;
    mock_status.online = 4;
    mock_status.online_mask = 0xdu;
    DRIVE_BOOT_APS();

    assert_eq(1, stop_calls);
    assert_true(strcmp(last_stop_component, "invariant") == 0);
    assert_eq((long)-EIO, (long)last_stop_rc);
    assert_eq(0, hook_calls);
    assert_eq(0, subsys_calls);
}

/* ── 7. fail-closed propagates every stage ───────────────────── */

static void test_fail_closed_propagates_all_stages(void)
{
    static const struct {
        enum smp_boot_stage stage;
        const char *name;
        int rc;
    } cases[] = {
        { SMP_STAGE_PREPARE,   "prepare",   -ENOMEM     },
        { SMP_STAGE_ACTIVATE,  "activate",  -ENODEV     },
        { SMP_STAGE_ONLINE,    "online",    -ETIMEDOUT  },
        { SMP_STAGE_SCHEDULER, "scheduler", -ETIMEDOUT  },
    };

    char expect_fatal[128];

    for (size_t k = 0; k < sizeof(cases) / sizeof(cases[0]); k++) {
        test_reset_all();
        num_cpus = 4;
        percpu_data[0].online = 1;
        mock_boot_rc = cases[k].rc;
        mock_status.requested = 4;
        mock_status.failed_cpu = 1;
        mock_status.failed_stage = cases[k].stage;
        mock_status.arch_rc = cases[k].rc;
        DRIVE_BOOT_APS();

        assert_eq(1, stop_calls);
        assert_true(strcmp(last_stop_stage, "smp") == 0);
        assert_true(strcmp(last_stop_component, cases[k].name) == 0);
        assert_eq((long)cases[k].rc, (long)last_stop_rc);
        assert_eq(-1, last_stop_phase);
        assert_eq(1, last_stop_cpu);

        snprintf(expect_fatal, sizeof(expect_fatal),
                 "[boot-1] FATAL cpu=1 stage=%s", cases[k].name);
        assert_true(strstr(log_capture_buf, expect_fatal) != NULL);

        assert_eq(0, hook_calls);
        assert_eq(0, subsys_calls);
    }
}

/* ── 8. AP common entry strict state machine order ───────────── */

static void test_ap_common_entry_strict_order(void)
{
    test_reset_all();

    if (setjmp(idle_jmp) == 0) {
        smp_ap_common_entry(1, &percpu_data[1]);
        smp_test_unreachable("smp_ap_common_entry returned");
    }

    assert_eq(1, idle_entered);
    assert_eq(6, trace_len);

    assert_eq(TR_CPU_INIT, trace_buf[0]);
    assert_eq(0, trace_online[0]);
    assert_eq(0, trace_irq[0]);

    assert_eq(TR_POST_ONLINE, trace_buf[1]);
    assert_eq(1, trace_online[1]);
    assert_eq(0, trace_irq[1]);

    assert_eq(TR_IRQ_ENABLE, trace_buf[2]);
    assert_eq(0, trace_irq[2]);

    assert_eq(TR_MB, trace_buf[3]);
    assert_eq(1, trace_irq[3]);

    assert_eq(TR_IPI_READY, trace_buf[4]);
    assert_eq(1, trace_online[4]);
    assert_eq(1, trace_irq[4]);

    assert_eq(TR_ENTER_IDLE, trace_buf[5]);

    /* The common entry is the only online publisher in the state machine. */
    assert_eq(1, (int)percpu_data[1].online);
}

int main(void)
{
    TEST_SUITE("unified-smp-boot test_smp_boot_unified");

    test_precondition_rejects_invalid_num_cpus();
    test_precondition_rejects_bsp_offline();
    test_precondition_rejects_dirty_ap();
    test_single_cpu_boot_succeeds();
    test_multicore_boot_order_and_args();
    test_postcondition_rejects_null_status_or_mask_mismatch();
    test_fail_closed_propagates_all_stages();
    test_ap_common_entry_strict_order();

    TEST_RESULTS();
    return 0;
}
