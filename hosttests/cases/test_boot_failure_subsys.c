/* hosttests/cases/test_boot_failure_subsys.c — BOOT-3 Task 1.
 *
 * Unit tests for subsystem failure barriers, registration integrity,
 * latching, iterator, and queries.
 */

#include "test_framework.h"
#include <subsys/subsys.h>
#include <core/boot_failure.h>
#include <percpu/percpu.h>
#include <errno.h>
#include <setjmp.h>
#include <string.h>
#include <stdio.h>

/* Number of CPUs for percpu tests */
uint32_t num_cpus = 2;

/* Global log level for log.h */
int g_log_level = 0;

/* Mock logger implementations */
void _log_info_impl(const char *fmt, ...) { (void)fmt; }
void _log_warn_impl(const char *fmt, ...) { (void)fmt; }
void _log_err_impl(const char *fmt, ...) { (void)fmt; }

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

static void reset_subsys_test(void)
{
    subsys_test_reset();
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
    num_cpus = 2;
}

/* Callbacks for testing */
static int init_ok(void) { return 0; }
static int init_fail_eio(void) { return -EIO; }
static int init_fail_enodev(void) { return -ENODEV; }
static int init_legacy_positive(void) { return 5; }

static int sentinel_ran = 0;
static int init_sentinel(void) { sentinel_ran = 1; return 0; }

static void test_required_failure_barrier(void)
{
    reset_subsys_test();
    sentinel_ran = 0;
    jump_on_stop = 1;

    register_subsys("mod_fail", init_fail_eio, SUBSYS_PHASE_3, 0);
    register_subsys("mod_sentinel", init_sentinel, SUBSYS_PHASE_3, 0);

    if (setjmp(stop_jmp) == 0) {
        subsys_init_phase(SUBSYS_PHASE_3);
    }

    assert_eq(1, stop_calls);
    assert_str_eq("subsys", last_stop_stage);
    assert_str_eq("mod_fail", last_stop_component);
    assert_eq(-EIO, last_stop_rc);
    assert_eq(3, last_stop_phase);
    assert_eq(-1, last_stop_cpu);
    /* Sentinel MUST NOT have run */
    assert_eq(0, sentinel_ran);
}

static void test_optional_failure_warn_and_continue(void)
{
    reset_subsys_test();
    sentinel_ran = 0;
    jump_on_stop = 0;

    register_subsys("mod_opt", init_fail_enodev, SUBSYS_PHASE_3, SUBSYS_FLAG_OPTIONAL);
    register_subsys("mod_sentinel", init_sentinel, SUBSYS_PHASE_3, 0);

    subsys_init_phase(SUBSYS_PHASE_3);

    assert_eq(0, stop_calls);
    assert_eq(1, warn_calls);
    assert_str_eq("subsys", last_warn_stage);
    assert_str_eq("mod_opt", last_warn_component);
    assert_eq(-ENODEV, last_warn_rc);
    assert_eq(3, last_warn_phase);
    assert_eq(-1, last_warn_cpu);

    /* Sentinel DID run */
    assert_eq(1, sentinel_ran);
    assert_eq(-1, subsys_status("mod_opt"));
    assert_eq(-ENODEV, subsys_result("mod_opt"));
    assert_eq(1, subsys_status("mod_sentinel"));
    assert_eq(0, subsys_result("mod_sentinel"));
}

/* Percpu callbacks */
static int percpu_fail_on_cpu1(int cpu_id)
{
    if (cpu_id == 1) return -EIO;
    return 0;
}

static int percpu_fail_on_cpu0(int cpu_id)
{
    if (cpu_id == 0) return -EBUSY;
    return 0;
}

static void test_percpu_required_failure(void)
{
    reset_subsys_test();
    jump_on_stop = 1;

    register_subsys_percpu("percpu_req", percpu_fail_on_cpu0, 0);

    if (setjmp(stop_jmp) == 0) {
        subsys_init_percpu();
    }

    assert_eq(1, stop_calls);
    assert_str_eq("percpu", last_stop_stage);
    assert_str_eq("percpu_req", last_stop_component);
    assert_eq(-EBUSY, last_stop_rc);
    assert_eq(-1, last_stop_phase);
    assert_eq(0, last_stop_cpu);
}

static void test_percpu_optional_failure(void)
{
    reset_subsys_test();
    jump_on_stop = 0;

    register_subsys_percpu("percpu_opt", percpu_fail_on_cpu1, SUBSYS_FLAG_OPTIONAL);

    subsys_init_percpu();

    assert_eq(0, stop_calls);
    assert_eq(1, warn_calls);
    assert_str_eq("percpu", last_warn_stage);
    assert_str_eq("percpu_opt", last_warn_component);
    assert_eq(-EIO, last_warn_rc);
    assert_eq(-1, last_warn_phase);
    assert_eq(1, last_warn_cpu);

    uint64_t mask = 0;
    int res = subsys_percpu_result("percpu_opt", &mask);
    assert_eq(-EIO, res);
    assert_eq((1ULL << 1), mask);

    /* Must NOT be marked 1 (success) when a cpu failed */
    int st = subsys_status("percpu_opt");
    assert_true(st < 0);
}

static void test_registration_validation(void)
{
    reset_subsys_test();

    /* NULL / empty name */
    assert_eq(-EINVAL, register_subsys(NULL, init_ok, 3, 0));
    assert_eq(-EINVAL, register_subsys("", init_ok, 3, 0));
    /* NULL callback */
    assert_eq(-EINVAL, register_subsys("test", NULL, 3, 0));
    /* Bad phase */
    assert_eq(-EINVAL, register_subsys("test", init_ok, 2, 0));
    assert_eq(-EINVAL, register_subsys("test", init_ok, 7, 0));
    /* Bad flags */
    assert_eq(-EINVAL, register_subsys("test", init_ok, 3, 0x10));

    /* Duplicate in BSP table */
    reset_subsys_test();
    assert_eq(0, register_subsys("dup_name", init_ok, 3, 0));
    assert_eq(-EEXIST, register_subsys("dup_name", init_ok, 4, 0));

    /* Table full (64 entries) */
    reset_subsys_test();
    char names[65][16];
    for (int i = 0; i < 64; i++) {
        snprintf(names[i], sizeof(names[i]), "sub_%d", i);
        assert_eq(0, register_subsys(names[i], init_ok, 3, 0));
    }
    snprintf(names[64], sizeof(names[64]), "sub_64");
    assert_eq(-ENOSPC, register_subsys(names[64], init_ok, 3, 0));

    /* Percpu validation */
    reset_subsys_test();
    assert_eq(-EINVAL, register_subsys_percpu(NULL, percpu_fail_on_cpu1, 0));
    assert_eq(-EINVAL, register_subsys_percpu("", percpu_fail_on_cpu1, 0));
    assert_eq(-EINVAL, register_subsys_percpu("p1", NULL, 0));
    assert_eq(-EINVAL, register_subsys_percpu("p1", percpu_fail_on_cpu1, 0x20));

    reset_subsys_test();
    assert_eq(0, register_subsys_percpu("p_dup", percpu_fail_on_cpu1, 0));
    assert_eq(-EEXIST, register_subsys_percpu("p_dup", percpu_fail_on_cpu1, 0));

    /* Percpu table full (16 entries) */
    reset_subsys_test();
    for (int i = 0; i < 16; i++) {
        snprintf(names[i], sizeof(names[i]), "psub_%d", i);
        assert_eq(0, register_subsys_percpu(names[i], percpu_fail_on_cpu1, 0));
    }
    snprintf(names[16], sizeof(names[16]), "psub_16");
    assert_eq(-ENOSPC, register_subsys_percpu(names[16], percpu_fail_on_cpu1, 0));
}

static int initcall_ok1(void) { return register_subsys("ic1", init_ok, 3, 0); }
static int initcall_ok2(void) { return register_subsys("ic2", init_ok, 3, 0); }
static int initcall_dup(void) { return register_subsys("ic1", init_ok, 4, 0); }
static int initcall_nonzero(void) { return -EIO; }
static int initcall_sentinel_ran = 0;
static int initcall_sentinel(void) { initcall_sentinel_ran = 1; return 0; }

static void test_initcall_iterator_and_latch(void)
{
    reset_subsys_test();
    jump_on_stop = 1;
    initcall_sentinel_ran = 0;

    /* An initcall that triggers a duplicate registration */
    subsys_initcall_t calls[] = {
        initcall_ok1,
        initcall_dup,      /* returns -EEXIST from register_subsys */
        initcall_sentinel
    };

    if (setjmp(stop_jmp) == 0) {
        subsys_register_initcalls(calls, calls + 3);
    }

    assert_eq(1, stop_calls);
    assert_str_eq("register", last_stop_stage);
    /* Should report the component that caused the registration error */
    assert_str_eq("ic1", last_stop_component);
    assert_eq(-EEXIST, last_stop_rc);
    assert_eq(0, initcall_sentinel_ran);

    /* An initcall that returns a non-zero error without latching */
    reset_subsys_test();
    jump_on_stop = 1;
    initcall_sentinel_ran = 0;

    subsys_initcall_t calls2[] = {
        initcall_ok2,
        initcall_nonzero,
        initcall_sentinel
    };

    if (setjmp(stop_jmp) == 0) {
        subsys_register_initcalls(calls2, calls2 + 3);
    }

    assert_eq(1, stop_calls);
    assert_str_eq("register", last_stop_stage);
    assert_eq(-EIO, last_stop_rc);
    assert_eq(0, initcall_sentinel_ran);
}

static void test_sealing_table(void)
{
    reset_subsys_test();
    jump_on_stop = 1;

    register_subsys("m1", init_ok, SUBSYS_PHASE_3, 0);
    subsys_init_phase(SUBSYS_PHASE_3);

    /* Late registration after phase 3 has run must fail */
    int rc = register_subsys("late", init_ok, SUBSYS_PHASE_4, 0);
    assert_true(rc < 0);

    /* Next phase execution catches the latch */
    if (setjmp(stop_jmp) == 0) {
        subsys_init_phase(SUBSYS_PHASE_4);
    }
    assert_eq(1, stop_calls);
    assert_str_eq("register", last_stop_stage);
}

static void test_queries(void)
{
    reset_subsys_test();

    assert_eq(-ENOENT, subsys_result("nonexistent"));
    assert_eq(-ENOENT, subsys_require_ready("nonexistent"));

    register_subsys("q_unrun", init_ok, 3, 0);
    assert_eq(-EAGAIN, subsys_result("q_unrun"));
    assert_eq(-EAGAIN, subsys_require_ready("q_unrun"));

    register_subsys("q_leg", init_legacy_positive, 3, SUBSYS_FLAG_OPTIONAL);
    subsys_init_phase(3);

    assert_eq(0, subsys_result("q_unrun"));
    assert_eq(0, subsys_require_ready("q_unrun"));

    /* Legacy positive rc is kept in raw result but mapped to -EIO in require_ready */
    assert_eq(5, subsys_result("q_leg"));
    assert_eq(-EIO, subsys_require_ready("q_leg"));
}

int main(void)
{
    TEST_SUITE("BOOT-3 boot_failure_subsys");
    test_required_failure_barrier();
    test_optional_failure_warn_and_continue();
    test_percpu_required_failure();
    test_percpu_optional_failure();
    test_registration_validation();
    test_initcall_iterator_and_latch();
    test_sealing_table();
    test_queries();
    TEST_RESULTS();
    return 0;
}
