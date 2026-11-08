/* hosttests/cases/test_boot_contract_subsys.c — BOOT-4 Task 1.
 *
 * Unit tests for named boot phases, bounded dependency resolution,
 * cycle detection, preflight validation, and production wrapper ordering.
 */

#include "test_framework.h"
#include <subsys/subsys.h>
#include <core/boot_failure.h>
#include <errno.h>
#include <setjmp.h>
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

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

static void reset_test_state(void)
{
    subsys_test_reset();
    stop_calls = 0;
    warn_calls = 0;
    memset(last_stop_stage, 0, sizeof(last_stop_stage));
    memset(last_stop_component, 0, sizeof(last_stop_component));
    last_stop_rc = 0;
    last_stop_phase = 0;
    last_stop_cpu = 0;
    memset(last_warn_stage, 0, sizeof(last_warn_stage));
    memset(last_warn_component, 0, sizeof(last_warn_component));
    last_warn_rc = 0;
    last_warn_phase = 0;
    last_warn_cpu = 0;
    jump_on_stop = 0;
}

/* Trace helpers */
static char exec_trace[32][32];
static int exec_count = 0;

static void trace_exec(const char *name)
{
    if (exec_count < 32) {
        snprintf(exec_trace[exec_count], sizeof(exec_trace[exec_count]), "%s", name);
        exec_count++;
    }
}

/* Dummy callbacks */
static int init_ok(void) { return 0; }
static int init_fail(void) { return -EIO; }

static int init_cb_producer(void) { trace_exec("producer"); return 0; }
static int init_cb_consumer(void) { trace_exec("consumer"); return 0; }
static int init_cb_indep(void)    { trace_exec("independent"); return 0; }

static int init_cb_A(void) { trace_exec("A"); return 0; }
static int init_cb_B(void) { trace_exec("B"); return 0; }
static int init_cb_C(void) { trace_exec("C"); return 0; }

/* ── Test cases ─────────────────────────────────────────── */

/* 1. Consumer registered before producer; producer must execute first */
static void test_subsys_deps_reorders_consumer_and_producer(void)
{
    reset_test_state();
    exec_count = 0;

    static const char *const cons_deps[] = {"producer"};
    /* Consumer registered first */
    int r1 = register_subsys_deps("consumer", init_cb_consumer,
                                  SUBSYS_PHASE_CONTROLLERS, 0, cons_deps, 1);
    assert_eq(0, r1);

    /* Producer registered second */
    int r2 = register_subsys_deps("producer", init_cb_producer,
                                  SUBSYS_PHASE_CONTROLLERS, 0, NULL, 0);
    assert_eq(0, r2);

    subsys_init_phase(SUBSYS_PHASE_CONTROLLERS);

    assert_eq(2, exec_count);
    assert_str_eq("producer", exec_trace[0]);
    assert_str_eq("consumer", exec_trace[1]);
    assert_eq(1, subsys_status("producer"));
    assert_eq(1, subsys_status("consumer"));
}

/* 2. Chain resolution: A -> B -> C registered in order A, B, C; must execute C, B, A */
static void test_subsys_deps_chain_resolution(void)
{
    reset_test_state();
    exec_count = 0;

    static const char *const a_deps[] = {"B"};
    static const char *const b_deps[] = {"C"};

    assert_eq(0, register_subsys_deps("A", init_cb_A, SUBSYS_PHASE_DEVICE_IRQ, 0, a_deps, 1));
    assert_eq(0, register_subsys_deps("B", init_cb_B, SUBSYS_PHASE_DEVICE_IRQ, 0, b_deps, 1));
    assert_eq(0, register_subsys_deps("C", init_cb_C, SUBSYS_PHASE_DEVICE_IRQ, 0, NULL, 0));

    subsys_init_phase(SUBSYS_PHASE_DEVICE_IRQ);

    assert_eq(3, exec_count);
    assert_str_eq("C", exec_trace[0]);
    assert_str_eq("B", exec_trace[1]);
    assert_str_eq("A", exec_trace[2]);
}

/* 3. Independent items not blocked by preceding pending items */
static void test_subsys_deps_independent_items_not_blocked(void)
{
    reset_test_state();
    exec_count = 0;

    static const char *const cons_deps[] = {"producer"};
    /* Order: Consumer (waits for producer), Independent (no deps), Producer (no deps) */
    assert_eq(0, register_subsys_deps("consumer", init_cb_consumer, SUBSYS_PHASE_TIME, 0, cons_deps, 1));
    assert_eq(0, register_subsys_deps("independent", init_cb_indep, SUBSYS_PHASE_TIME, 0, NULL, 0));
    assert_eq(0, register_subsys_deps("producer", init_cb_producer, SUBSYS_PHASE_TIME, 0, NULL, 0));

    subsys_init_phase(SUBSYS_PHASE_TIME);

    assert_eq(3, exec_count);
    /* In round 1, independent and producer are ready (order: independent, then producer) */
    assert_str_eq("independent", exec_trace[0]);
    assert_str_eq("producer", exec_trace[1]);
    /* In round 2, consumer is ready */
    assert_str_eq("consumer", exec_trace[2]);
}

/* 4. Earlier phase not run yields -EAGAIN without executing consumer */
static void test_subsys_deps_earlier_phase_not_run_yields_eagain(void)
{
    reset_test_state();
    exec_count = 0;

    static const char *const cons_deps[] = {"producer_p4"};
    /* Producer in phase TIME (4) */
    assert_eq(0, register_subsys_deps("producer_p4", init_cb_producer, SUBSYS_PHASE_TIME, 0, NULL, 0));
    /* Consumer in phase DEVICE_IRQ (5) requires producer_p4 */
    assert_eq(0, register_subsys_deps("consumer_p5", init_cb_consumer, SUBSYS_PHASE_DEVICE_IRQ, 0, cons_deps, 1));

    /* Dispatch phase 5 directly without phase 4 */
    jump_on_stop = 1;
    if (setjmp(stop_jmp) == 0) {
        subsys_init_phase(SUBSYS_PHASE_DEVICE_IRQ);
        assert_true(false); /* Should not reach */
    }

    assert_eq(1, stop_calls);
    assert_str_eq("subsys", last_stop_stage);
    assert_str_eq("consumer_p5", last_stop_component);
    assert_eq(-EAGAIN, last_stop_rc);
    assert_eq((int)SUBSYS_PHASE_DEVICE_IRQ, last_stop_phase);
    assert_eq(0, exec_count); /* Neither ran */
}

/* 5. Optional producer failed -> required consumer fatal -ENODEV */
static void test_subsys_deps_optional_producer_failed_required_consumer(void)
{
    reset_test_state();
    exec_count = 0;

    static const char *const cons_deps[] = {"opt_prod"};
    assert_eq(0, register_subsys_deps("opt_prod", init_fail, SUBSYS_PHASE_TIME, SUBSYS_FLAG_OPTIONAL, NULL, 0));
    assert_eq(0, register_subsys_deps("req_cons", init_cb_consumer, SUBSYS_PHASE_TIME, 0, cons_deps, 1));

    jump_on_stop = 1;
    if (setjmp(stop_jmp) == 0) {
        subsys_init_phase(SUBSYS_PHASE_TIME);
        assert_true(false);
    }

    assert_eq(1, warn_calls);
    assert_str_eq("subsys", last_warn_stage);
    assert_str_eq("opt_prod", last_warn_component);

    assert_eq(1, stop_calls);
    assert_str_eq("subsys", last_stop_stage);
    assert_str_eq("req_cons", last_stop_component);
    assert_eq(-ENODEV, last_stop_rc);
    assert_eq(0, exec_count); /* Consumer did not execute */
}

/* 6. Optional producer failed -> optional consumer skipped with -ENODEV & WARN, independent continues */
static void test_subsys_deps_optional_producer_failed_optional_consumer(void)
{
    reset_test_state();
    exec_count = 0;

    static const char *const cons_deps[] = {"opt_prod"};
    assert_eq(0, register_subsys_deps("opt_prod", init_fail, SUBSYS_PHASE_STORAGE, SUBSYS_FLAG_OPTIONAL, NULL, 0));
    assert_eq(0, register_subsys_deps("opt_cons", init_cb_consumer, SUBSYS_PHASE_STORAGE, SUBSYS_FLAG_OPTIONAL, cons_deps, 1));
    assert_eq(0, register_subsys_deps("indep", init_cb_indep, SUBSYS_PHASE_STORAGE, 0, NULL, 0));

    subsys_init_phase(SUBSYS_PHASE_STORAGE);

    assert_eq(0, stop_calls);
    assert_eq(2, warn_calls); /* opt_prod warned, opt_cons warned */
    assert_str_eq("subsys", last_warn_stage);
    assert_str_eq("opt_cons", last_warn_component);
    assert_eq(-ENODEV, last_warn_rc);

    assert_eq(-1, subsys_status("opt_cons"));
    assert_eq(-ENODEV, subsys_result("opt_cons"));

    assert_eq(1, exec_count);
    assert_str_eq("independent", exec_trace[0]);
    assert_eq(1, subsys_status("indep"));
}

/* 7. Preflight RED: Missing dependency -> -ENOENT fatal before any callback runs */
static void test_subsys_deps_missing_dependency(void)
{
    reset_test_state();
    exec_count = 0;

    static const char *const bad_deps[] = {"ghost"};
    assert_eq(0, register_subsys_deps("indep", init_cb_indep, SUBSYS_PHASE_CONTROLLERS, 0, NULL, 0));
    assert_eq(0, register_subsys_deps("consumer", init_cb_consumer, SUBSYS_PHASE_CONTROLLERS, 0, bad_deps, 1));

    jump_on_stop = 1;
    if (setjmp(stop_jmp) == 0) {
        subsys_init_phase(SUBSYS_PHASE_CONTROLLERS);
        assert_true(false);
    }

    assert_eq(1, stop_calls);
    assert_str_eq("register", last_stop_stage);
    assert_str_eq("consumer", last_stop_component);
    assert_eq(-ENOENT, last_stop_rc);
    assert_eq(0, exec_count); /* Zero callbacks executed */
}

/* 8. Preflight RED: Future phase dependency -> -EINVAL fatal before any callback runs */
static void test_subsys_deps_future_phase(void)
{
    reset_test_state();
    exec_count = 0;

    static const char *const fut_deps[] = {"storage_prod"};
    /* Consumer in phase 4 requires producer in phase 6 */
    assert_eq(0, register_subsys_deps("time_cons", init_cb_consumer, SUBSYS_PHASE_TIME, 0, fut_deps, 1));
    assert_eq(0, register_subsys_deps("storage_prod", init_cb_producer, SUBSYS_PHASE_STORAGE, 0, NULL, 0));

    jump_on_stop = 1;
    if (setjmp(stop_jmp) == 0) {
        subsys_init_phase(SUBSYS_PHASE_TIME);
        assert_true(false);
    }

    assert_eq(1, stop_calls);
    assert_str_eq("register", last_stop_stage);
    assert_str_eq("time_cons", last_stop_component);
    assert_eq(-EINVAL, last_stop_rc);
    assert_eq(0, exec_count);
}

/* 9. Registration validation: Self-dependency returns -EINVAL and latches */
static void test_subsys_deps_self_dependency(void)
{
    reset_test_state();

    static const char *const self_deps[] = {"myself"};
    int r = register_subsys_deps("myself", init_ok, SUBSYS_PHASE_CONTROLLERS, 0, self_deps, 1);
    assert_eq(-EINVAL, r);

    /* Second register rejected because latch has error */
    int r2 = register_subsys("second", init_ok, SUBSYS_PHASE_CONTROLLERS, 0);
    assert_eq(-EINVAL, r2);
}

/* 10. Registration validation: Duplicate dependency in requires returns -EINVAL */
static void test_subsys_deps_duplicate_dependency(void)
{
    reset_test_state();

    static const char *const dup_deps[] = {"dep1", "dep1"};
    int r = register_subsys_deps("client", init_ok, SUBSYS_PHASE_CONTROLLERS, 0, dup_deps, 2);
    assert_eq(-EINVAL, r);
}

/* 11. Registration validation: count > 8 returns -EINVAL */
static void test_subsys_deps_count_over_limit(void)
{
    reset_test_state();

    static const char *const many_deps[] = {
        "d1", "d2", "d3", "d4", "d5", "d6", "d7", "d8", "d9"
    };
    int r = register_subsys_deps("client", init_ok, SUBSYS_PHASE_CONTROLLERS, 0, many_deps, 9);
    assert_eq(-EINVAL, r);
}

/* 12. Registration validation: NULL or empty requires element returns -EINVAL */
static void test_subsys_deps_null_or_empty_requires_element(void)
{
    reset_test_state();
    static const char *const null_deps[] = {"valid", NULL};
    int r1 = register_subsys_deps("c1", init_ok, SUBSYS_PHASE_CONTROLLERS, 0, null_deps, 2);
    assert_eq(-EINVAL, r1);

    reset_test_state();
    static const char *const empty_deps[] = {"valid", ""};
    int r2 = register_subsys_deps("c2", init_ok, SUBSYS_PHASE_CONTROLLERS, 0, empty_deps, 2);
    assert_eq(-EINVAL, r2);
}

/* 13. Registration validation: count == 8 is valid */
static void test_subsys_deps_max_8_valid(void)
{
    reset_test_state();

    static const char *const eight_deps[] = {
        "d1", "d2", "d3", "d4", "d5", "d6", "d7", "d8"
    };
    /* Register 8 producers first */
    for (int i = 0; i < 8; i++) {
        assert_eq(0, register_subsys_deps(eight_deps[i], init_ok, SUBSYS_PHASE_CONTROLLERS, 0, NULL, 0));
    }
    int r = register_subsys_deps("consumer8", init_ok, SUBSYS_PHASE_CONTROLLERS, 0, eight_deps, 8);
    assert_eq(0, r);
}

/* 14. Cycle detection: A -> B, B -> C, C -> B registered in order A, B, C reports B (earliest actual cycle member)! */
static void test_subsys_deps_cycle_detection(void)
{
    reset_test_state();
    exec_count = 0;

    static const char *const a_deps[] = {"B"};
    static const char *const b_deps[] = {"C"};
    static const char *const c_deps[] = {"B"};

    /* Registration order: A, then B, then C */
    assert_eq(0, register_subsys_deps("A", init_cb_A, SUBSYS_PHASE_CONTROLLERS, 0, a_deps, 1));
    assert_eq(0, register_subsys_deps("B", init_cb_B, SUBSYS_PHASE_CONTROLLERS, 0, b_deps, 1));
    assert_eq(0, register_subsys_deps("C", init_cb_C, SUBSYS_PHASE_CONTROLLERS, 0, c_deps, 1));

    jump_on_stop = 1;
    if (setjmp(stop_jmp) == 0) {
        subsys_init_phase(SUBSYS_PHASE_CONTROLLERS);
        assert_true(false);
    }

    assert_eq(1, stop_calls);
    assert_str_eq("register", last_stop_stage);
    /* Crucial: A is NOT in the cycle! B and C are in the cycle. B was registered before C. Diagnosis must report B! */
    assert_str_eq("B", last_stop_component);
    assert_eq(-ELOOP, last_stop_rc);
    assert_eq(0, exec_count);
}

/* 15. Multiple cycles: reports the registration-order earliest actual cycle member */
static void test_subsys_deps_multiple_cycles(void)
{
    reset_test_state();

    static const char *const x1_deps[] = {"X2"};
    static const char *const x2_deps[] = {"X1"};
    static const char *const y1_deps[] = {"Y2"};
    static const char *const y2_deps[] = {"Y1"};

    /* Cycle 1: X1 <-> X2; Cycle 2: Y1 <-> Y2 */
    assert_eq(0, register_subsys_deps("X1", init_ok, SUBSYS_PHASE_TIME, 0, x1_deps, 1));
    assert_eq(0, register_subsys_deps("X2", init_ok, SUBSYS_PHASE_TIME, 0, x2_deps, 1));
    assert_eq(0, register_subsys_deps("Y1", init_ok, SUBSYS_PHASE_TIME, 0, y1_deps, 1));
    assert_eq(0, register_subsys_deps("Y2", init_ok, SUBSYS_PHASE_TIME, 0, y2_deps, 1));

    jump_on_stop = 1;
    if (setjmp(stop_jmp) == 0) {
        subsys_init_phase(SUBSYS_PHASE_TIME);
        assert_true(false);
    }

    assert_eq(1, stop_calls);
    assert_str_eq("register", last_stop_stage);
    assert_str_eq("X1", last_stop_component);
    assert_eq(-ELOOP, last_stop_rc);
}

/* 16. Invalid phase dispatch yields -EINVAL fatal */
static void test_subsys_deps_invalid_phase_dispatch(void)
{
    reset_test_state();

    jump_on_stop = 1;
    if (setjmp(stop_jmp) == 0) {
        subsys_init_phase(2);
        assert_true(false);
    }

    assert_eq(1, stop_calls);
    assert_str_eq("subsys", last_stop_stage);
    assert_str_eq("phase", last_stop_component);
    assert_eq(-EINVAL, last_stop_rc);

    reset_test_state();
    jump_on_stop = 1;
    if (setjmp(stop_jmp) == 0) {
        subsys_init_phase(7);
        assert_true(false);
    }

    assert_eq(1, stop_calls);
    assert_str_eq("subsys", last_stop_stage);
    assert_str_eq("phase", last_stop_component);
    assert_eq(-EINVAL, last_stop_rc);
}

/* 17. PIC & APIC production wrapper ordering: PIC registered first, APIC registered second -> APIC runs first */
static bool mock_ioapic_ready = false;
static int mock_apic_init(void)
{
    trace_exec("apic");
    mock_ioapic_ready = true;
    return 0;
}
static int mock_pic_init(void)
{
    trace_exec("pic");
    /* PIC asserts that IOAPIC is already available */
    if (!mock_ioapic_ready) {
        return -EIO;
    }
    return 0;
}

static void test_subsys_deps_pic_apic_production_wrappers(void)
{
    reset_test_state();
    exec_count = 0;
    mock_ioapic_ready = false;

    static const char *const pic_deps[] = {"apic"};
    /* PIC registered first */
    assert_eq(0, register_subsys_deps("pic", mock_pic_init, SUBSYS_PHASE_CONTROLLERS, SUBSYS_FLAG_OPTIONAL, pic_deps, 1));
    /* APIC registered second */
    assert_eq(0, register_subsys_deps("apic", mock_apic_init, SUBSYS_PHASE_CONTROLLERS, 0, NULL, 0));

    subsys_init_phase(SUBSYS_PHASE_CONTROLLERS);

    assert_eq(2, exec_count);
    assert_str_eq("apic", exec_trace[0]);
    assert_str_eq("pic", exec_trace[1]);
    assert_eq(1, subsys_status("apic"));
    assert_eq(1, subsys_status("pic"));
}

int main(void)
{
    TEST_SUITE("BOOT-4 test_boot_contract_subsys");

    test_subsys_deps_reorders_consumer_and_producer();
    test_subsys_deps_chain_resolution();
    test_subsys_deps_independent_items_not_blocked();
    test_subsys_deps_earlier_phase_not_run_yields_eagain();
    test_subsys_deps_optional_producer_failed_required_consumer();
    test_subsys_deps_optional_producer_failed_optional_consumer();
    test_subsys_deps_missing_dependency();
    test_subsys_deps_future_phase();
    test_subsys_deps_self_dependency();
    test_subsys_deps_duplicate_dependency();
    test_subsys_deps_count_over_limit();
    test_subsys_deps_null_or_empty_requires_element();
    test_subsys_deps_max_8_valid();
    test_subsys_deps_cycle_detection();
    test_subsys_deps_multiple_cycles();
    test_subsys_deps_invalid_phase_dispatch();
    test_subsys_deps_pic_apic_production_wrappers();

    TEST_RESULTS();
    return 0;
}
