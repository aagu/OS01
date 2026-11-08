/* hosttests/cases/test_boot_contract_cpu_prepare.c — BOOT-4 Task 2.
 *
 * Unit tests for BSP CPU preparation semantics, deprecated wrapper compatibility,
 * idempotence, namespace coexistence, and source/symbol contract checks.
 */

#include "test_framework.h"
#include <subsys/subsys.h>
#include <core/boot_failure.h>
#include <errno.h>
#include <setjmp.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>

#ifndef OS01_KERNEL_SRC
#define OS01_KERNEL_SRC "."
#endif

/* Number of CPUs for tests */
uint32_t num_cpus = 4;

/* Global log level for log.h */
int g_log_level = 6;

/* Log capture buffer */
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

void _log_warn_impl(const char *fmt, ...)
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
    (void)fmt;
}

/* Mock boot_failure_stop & warn */
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

static int warn_calls = 0;
static char last_warn_stage[32];
static char last_warn_component[64];
static int64_t last_warn_rc = 0;
static int last_warn_phase = 0;
static int last_warn_cpu = 0;

void boot_failure_warn(const char *stage, const char *component,
                       int64_t rc, int phase, int cpu)
{
    warn_calls++;
    snprintf(last_warn_stage, sizeof(last_warn_stage), "%s", stage ? stage : "unknown");
    snprintf(last_warn_component, sizeof(last_warn_component), "%s", component ? component : "unknown");
    last_warn_rc = rc;
    last_warn_phase = phase;
    last_warn_cpu = cpu;
}

static void test_reset_all(void)
{
    subsys_test_reset();
    log_capture_reset();
    stop_calls = 0;
    warn_calls = 0;
    last_stop_stage[0] = '\0';
    last_stop_component[0] = '\0';
    last_stop_rc = 0;
    last_stop_phase = 0;
    last_stop_cpu = 0;
    last_warn_stage[0] = '\0';
    last_warn_component[0] = '\0';
    last_warn_rc = 0;
    last_warn_phase = 0;
    last_warn_cpu = 0;
    jump_on_stop = 0;
    num_cpus = 4;
}

/* Tracking callbacks */
static int cb1_called_targets[8];
static int cb1_call_count = 0;
static int cb1_executor[8];

static int mock_prep_cb1(int target_cpu)
{
    if (cb1_call_count < 8) {
        cb1_called_targets[cb1_call_count] = target_cpu;
        cb1_executor[cb1_call_count] = 0; /* BSP is always executor 0 */
        cb1_call_count++;
    }
    return 0;
}

/* Test 1: Executor is BSP and offline target is still called */
static void test_cpu_prepare_executor_and_targets(void)
{
    test_reset_all();
    cb1_call_count = 0;
    memset(cb1_called_targets, -1, sizeof(cb1_called_targets));

    /* Target 2 offline still passed to prepare_for_cpu */
    num_cpus = 4;
    int rc = register_subsys_cpu_prepare("prep_bsp", mock_prep_cb1, 0);
    assert_eq(0, rc);

    subsys_prepare_cpus();

    assert_eq(4, cb1_call_count);
    for (int i = 0; i < 4; i++) {
        assert_eq(i, cb1_called_targets[i]);
        assert_eq(0, cb1_executor[i]);
    }

    /* Check logs: cpu-prepare prep_bsp target=%u executor=bsp */
    assert_true(strstr(log_capture_buf, "cpu-prepare prep_bsp target=0 executor=bsp") != NULL);
    assert_true(strstr(log_capture_buf, "cpu-prepare prep_bsp target=1 executor=bsp") != NULL);
    assert_true(strstr(log_capture_buf, "cpu-prepare prep_bsp target=2 executor=bsp") != NULL);
    assert_true(strstr(log_capture_buf, "cpu-prepare prep_bsp target=3 executor=bsp") != NULL);

    /* Must NOT contain fake evidence like executor=target or lapic-timer-start */
    assert_true(strstr(log_capture_buf, "executor=target") == NULL);
    assert_true(strstr(log_capture_buf, "lapic-timer-start") == NULL);
}

/* Test 2: Required failure halts immediately with stage cpu-prepare */
static int fail_on_target_1(int target_cpu)
{
    if (target_cpu == 1) return -EIO;
    return 0;
}

static void test_cpu_prepare_required_failure_halts(void)
{
    test_reset_all();
    num_cpus = 4;

    int rc = register_subsys_cpu_prepare("prep_req", fail_on_target_1, 0);
    assert_eq(0, rc);

    jump_on_stop = 1;
    if (setjmp(stop_jmp) == 0) {
        subsys_prepare_cpus();
        assert_true(0); /* should not reach */
    }

    assert_eq(1, stop_calls);
    assert_str_eq("percpu", last_stop_stage);
    assert_str_eq("prep_req", last_stop_component);
    assert_eq(-EIO, last_stop_rc);
    assert_eq(-1, last_stop_phase);
    assert_eq(1, last_stop_cpu);
}

/* Test 3: Optional failure continues, sets failed_mask and records first rc */
static int fail_optional_targets(int target_cpu)
{
    if (target_cpu == 1) return -EIO;
    if (target_cpu == 2) return -EINVAL;
    return 0;
}

static void test_cpu_prepare_optional_failure_continues(void)
{
    test_reset_all();
    num_cpus = 4;

    int rc = register_subsys_cpu_prepare("prep_opt", fail_optional_targets, SUBSYS_FLAG_OPTIONAL);
    assert_eq(0, rc);

    subsys_prepare_cpus();

    assert_eq(0, stop_calls);
    assert_eq(2, warn_calls);
    assert_str_eq("percpu", last_warn_stage);
    assert_str_eq("prep_opt", last_warn_component);
    assert_eq(-EINVAL, last_warn_rc);
    assert_eq(-1, last_warn_phase);
    assert_eq(2, last_warn_cpu);

    uint64_t failed_mask = 0;
    int res = subsys_cpu_prepare_result("prep_opt", &failed_mask);
    assert_eq(-EIO, res);
    assert_eq((1ULL << 1) | (1ULL << 2), failed_mask);
    assert_eq(-1, subsys_status("prep_opt"));
}

/* Test 4: Deprecated wrappers operate on same table and do not rerun */
static int deprecation_counter = 0;
static int mock_counted_cb(int target_cpu)
{
    (void)target_cpu;
    deprecation_counter++;
    return 0;
}

static void test_cpu_prepare_deprecated_api_compatibility_and_idempotence(void)
{
    test_reset_all();
    num_cpus = 2;
    deprecation_counter = 0;

    /* Register old API */
    int rc = register_subsys_percpu("compat_entry", mock_counted_cb, 0);
    assert_eq(0, rc);

    /* Dispatch with new API */
    subsys_prepare_cpus();
    assert_eq(2, deprecation_counter);

    /* Query with old API */
    uint64_t mask_old = 0xFF;
    int res_old = subsys_percpu_result("compat_entry", &mask_old);
    assert_eq(0, res_old);
    assert_eq(0ULL, mask_old);

    /* Query with new API */
    uint64_t mask_new = 0xFF;
    int res_new = subsys_cpu_prepare_result("compat_entry", &mask_new);
    assert_eq(0, res_new);
    assert_eq(0ULL, mask_new);

    /* Dispatch again with old API: must NOT rerun */
    subsys_init_percpu();
    assert_eq(2, deprecation_counter);

    /* Dispatch again with new API: must NOT rerun */
    subsys_prepare_cpus();
    assert_eq(2, deprecation_counter);
}

/* Test 5: Capacity limit 16 entries and late-register seal */
static void test_cpu_prepare_capacity_and_seal(void)
{
    test_reset_all();
    num_cpus = 1;

    char names[17][16];
    for (int i = 0; i < 16; i++) {
        snprintf(names[i], sizeof(names[i]), "cpup_%d", i);
        int rc = register_subsys_cpu_prepare(names[i], mock_prep_cb1, 0);
        assert_eq(0, rc);
    }

    /* 17th entry fails with -ENOSPC */
    snprintf(names[16], sizeof(names[16]), "cpup_16");
    int rc17 = register_subsys_cpu_prepare(names[16], mock_prep_cb1, 0);
    assert_eq(-ENOSPC, rc17);

    test_reset_all();
    /* Register 1 entry, dispatch, then late register fails with -EINVAL */
    assert_eq(0, register_subsys_cpu_prepare("cpup_seal", mock_prep_cb1, 0));
    subsys_prepare_cpus();

    int rc_late = register_subsys_cpu_prepare("cpup_late", mock_prep_cb1, 0);
    assert_eq(-EINVAL, rc_late);
}

/* Test 6: Same namespace coexistence with BSP table, subsys_status BSP priority */
static int bsp_init_cb(void) { return 0; }

static void test_cpu_prepare_namespace_and_status(void)
{
    test_reset_all();
    num_cpus = 2;

    /* Register same name in BSP table and CPU prepare table */
    assert_eq(0, register_subsys("shared_name", bsp_init_cb, SUBSYS_PHASE_TIME, 0));
    assert_eq(0, register_subsys_cpu_prepare("shared_name", mock_prep_cb1, 0));

    /* Before any dispatch: status is 0 (pending) */
    assert_eq(0, subsys_status("shared_name"));

    /* Dispatch CPU prepare table only */
    subsys_prepare_cpus();

    /* Status should still check BSP table first: BSP is still 0! */
    assert_eq(0, subsys_status("shared_name"));

    /* Dispatch BSP table */
    subsys_init_phase(SUBSYS_PHASE_TIME);

    /* Now BSP table is 1 */
    assert_eq(1, subsys_status("shared_name"));
}

/* Test 7: Corner cases for subsys_cpu_prepare_result */
static void test_cpu_prepare_result_corner_cases(void)
{
    test_reset_all();
    num_cpus = 2;

    uint64_t mask = 0xAA;
    assert_eq(-ENOENT, subsys_cpu_prepare_result(NULL, &mask));
    assert_eq(0ULL, mask);

    mask = 0xAA;
    assert_eq(-ENOENT, subsys_cpu_prepare_result("unknown", &mask));
    assert_eq(0ULL, mask);

    assert_eq(0, register_subsys_cpu_prepare("unrun", mock_prep_cb1, 0));
    mask = 0xAA;
    assert_eq(0, subsys_cpu_prepare_result("unrun", &mask));
    assert_eq(0ULL, mask);

    subsys_prepare_cpus();

    /* NULL failed_mask pointer handled gracefully */
    assert_eq(0, subsys_cpu_prepare_result("unrun", NULL));
}

/* Test 8: Source/Symbol contract check (Step 2) */
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

static void test_cpu_prepare_source_contract(void)
{
    /* 1. kernel/arch/x86_64/smp/subsys_percpu.c must NOT exist */
    FILE *f_old = open_kernel_file("kernel/arch/x86_64/smp/subsys_percpu.c");
    assert_true(f_old == NULL);
    if (f_old) fclose(f_old);

    /* 2. kernel/arch/x86_64/smp/boot.c must NOT contain arch_register_subsys_percpu */
    char *boot_src = slurp_file("kernel/arch/x86_64/smp/boot.c");
    assert_true(boot_src != NULL);
    if (boot_src) {
        assert_true(strstr(boot_src, "arch_register_subsys_percpu") == NULL);
        assert_true(strstr(boot_src, "_lapic_timer_start_percpu") == NULL);
        assert_true(strstr(boot_src, "subsys_prepare_cpus") != NULL);
        free(boot_src);
    }

    /* 3. kernel/include/arch/subsys.h must NOT contain arch_register_subsys_percpu */
    char *hdr_src = slurp_file("kernel/include/arch/subsys.h");
    assert_true(hdr_src != NULL);
    if (hdr_src) {
        assert_true(strstr(hdr_src, "arch_register_subsys_percpu") == NULL);
        free(hdr_src);
    }
}

int main(void)
{
    TEST_SUITE("BOOT-4 test_boot_contract_cpu_prepare");

    test_cpu_prepare_executor_and_targets();
    test_cpu_prepare_required_failure_halts();
    test_cpu_prepare_optional_failure_continues();
    test_cpu_prepare_deprecated_api_compatibility_and_idempotence();
    test_cpu_prepare_capacity_and_seal();
    test_cpu_prepare_namespace_and_status();
    test_cpu_prepare_result_corner_cases();
    test_cpu_prepare_source_contract();

    TEST_RESULTS();
    return 0;
}
