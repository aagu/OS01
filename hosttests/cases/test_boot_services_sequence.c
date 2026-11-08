/* hosttests/cases/test_boot_services_sequence.c — BOOT-5 Task 3 sequence test.
 *
 * Verifies kernel_start_userspace() in kernel/init/userspace.c:
 *   - Normal execution sequence:
 *     begin -> futex -> console -> archprepare -> init_mm -> publishidle ->
 *     stdio -> end -> spawn(pid1) -> probe -> net -> schedstart -> thermal -> late -> archidle
 *   - Verifies net runs in open window (scheduler_ok == 0)
 *   - Verifies thermal runs in closed window (scheduler_ok == 1)
 *   - stdio failure stops without end or spawn
 *   - spawn failure (negative) stops without downstream services
 *   - spawn failure (!= 1) stops without unpublishing
 */

#include "test_framework.h"
#include <init/userspace.h>
#include <init/services.h>
#include <selftest/boot_late.h>
#include <sched/task.h>
#include <sched/boot.h>
#include <memory/memory.h>
#include <memory/pmm.h>
#include <percpu/percpu.h>
#include <core/boot_failure.h>
#include <setjmp.h>
#include <string.h>

#define MAX_SEQ_TRACE 32
static char seq_trace[MAX_SEQ_TRACE][32];
static int seq_trace_count = 0;

static void record_seq(const char *name)
{
    if (seq_trace_count < MAX_SEQ_TRACE) {
        snprintf(seq_trace[seq_trace_count++], 32, "%s", name);
    }
}

static int find_seq(const char *name)
{
    for (int i = 0; i < seq_trace_count; i++) {
        if (strcmp(seq_trace[i], name) == 0)
            return i;
    }
    return -1;
}

static jmp_buf g_seq_jmp;
static char last_fail_stage[32];
static char last_fail_comp[32];
static int64_t last_fail_rc = 0;

void boot_failure_stop(const char *stage, const char *comp, int64_t rc, int phase, int cpu)
{
    (void)phase; (void)cpu;
    snprintf(last_fail_stage, sizeof(last_fail_stage), "%s", stage ? stage : "");
    snprintf(last_fail_comp, sizeof(last_fail_comp), "%s", comp ? comp : "");
    last_fail_rc = rc;
    record_seq("boot_failure_stop");
    longjmp(g_seq_jmp, 1);
}

/* Stubs & flags */
static int mock_stdio_rc = 0;
static int64_t mock_spawn_rc = 1;
static bool net_saw_window_open = false;
static bool net_saw_scheduler_ok = false;
static bool thermal_saw_window_closed = false;
static bool thermal_saw_scheduler_ok = false;

/* Mock globals */
union task_union init_task_union;
mm_t init_mm;
struct Physical_Memory_Manager PMMngr = {
    .start_code = 0x100000,
    .end_code = 0x200000,
    .end_data = 0x300000,
    .end_rodata = 0x400000,
    .start_brk = 0x500000,
};
char _data = 0;
char _rodata = 0;
uint64_t _stack_start = 0x600000;

percpu_t percpu_data[NR_CPUS];
task_t *host_test_current = &init_task_union.task;

/* Stubs */
void sched_boot_creation_begin(void) { record_seq("creation_begin"); }
bool sched_boot_creation_open(void) { return true; }
void futex_init(void) { record_seq("futex"); }
void console_init(void) { record_seq("console"); }
void arch_sched_boot_prepare(void) { record_seq("archprepare"); }
void sched_boot_publish_idle(void) { record_seq("publishidle"); }

int sched_boot_prepare_stdio(task_t *t, const char **failed_comp)
{
    (void)t;
    record_seq("stdio");
    if (mock_stdio_rc != 0) {
        *failed_comp = "dev_tty";
        return mock_stdio_rc;
    }
    return 0;
}
void boot3_test_sentinel(const char *name) { (void)name; }
void vfs_end_bootstrap(void) { record_seq("end"); }

void boot3_test_spawn_enter(void) {}
const char *boot3_test_init_path(void) { return "/bin/init"; }
int64_t spawn_user_task(const char *path, const char *const *argv)
{
    (void)path; (void)argv;
    record_seq("spawn");
    return mock_spawn_rc;
}
void boot3_test_spawn_leave(void) {}

void selftest_boot_creation_probe(void) { record_seq("probe"); }

void boot_services_prepare(void)
{
    record_seq("net");
    net_saw_window_open = sched_boot_creation_open();
    net_saw_scheduler_ok = percpu_data[0].scheduler_ok;
}

void sched_boot_start(void)
{
    record_seq("schedstart");
    percpu_data[0].scheduler_ok = 1;
}

void boot_services_start(void)
{
    record_seq("thermal");
    thermal_saw_window_closed = true;
    thermal_saw_scheduler_ok = percpu_data[0].scheduler_ok;
}

void selftest_boot_run_late(void) { record_seq("late"); }

void arch_sched_boot_idle(void)
{
    record_seq("archidle");
    longjmp(g_seq_jmp, 2);
}

int g_log_level = 0;
void _log_info_impl(const char *f, ...) { (void)f; }
void _log_warn_impl(const char *f, ...) { (void)f; }
void _log_err_impl(const char *f, ...) { (void)f; }
void _log_debug_impl(const char *f, ...) { (void)f; }
void serial_printk(const char *f, ...) { (void)f; }
void printk(const char *f, ...) { (void)f; }

static void reset_sequence_state(void)
{
    seq_trace_count = 0;
    mock_stdio_rc = 0;
    mock_spawn_rc = 1;
    net_saw_window_open = false;
    net_saw_scheduler_ok = false;
    thermal_saw_window_closed = false;
    thermal_saw_scheduler_ok = false;
    memset(last_fail_stage, 0, sizeof(last_fail_stage));
    memset(last_fail_comp, 0, sizeof(last_fail_comp));
    last_fail_rc = 0;
    memset(&percpu_data, 0, sizeof(percpu_data));
    memset(&init_task_union, 0, sizeof(init_task_union));
    memset(&init_mm, 0, sizeof(init_mm));
}

static void test_normal_sequence(void)
{
    reset_sequence_state();

    int val = setjmp(g_seq_jmp);
    if (val == 0) {
        kernel_start_userspace();
    }

    assert_eq(2, val); /* Reached archidle */

    int idx_begin = find_seq("creation_begin");
    int idx_futex = find_seq("futex");
    int idx_console = find_seq("console");
    int idx_archprep = find_seq("archprepare");
    int idx_pubidle = find_seq("publishidle");
    int idx_stdio = find_seq("stdio");
    int idx_end = find_seq("end");
    int idx_spawn = find_seq("spawn");
    int idx_probe = find_seq("probe");
    int idx_net = find_seq("net");
    int idx_schedstart = find_seq("schedstart");
    int idx_thermal = find_seq("thermal");
    int idx_late = find_seq("late");
    int idx_idle = find_seq("archidle");

    assert_true(idx_begin >= 0);
    assert_true(idx_futex > idx_begin);
    assert_true(idx_console > idx_futex);
    assert_true(idx_archprep > idx_console);
    assert_true(idx_pubidle > idx_archprep);
    assert_true(idx_stdio > idx_pubidle);
    assert_true(idx_end > idx_stdio);
    assert_true(idx_spawn > idx_end);
    assert_true(idx_probe > idx_spawn);
    assert_true(idx_net > idx_probe);
    assert_true(idx_schedstart > idx_net);
    assert_true(idx_thermal > idx_schedstart);
    assert_true(idx_late > idx_thermal);
    assert_true(idx_idle > idx_late);

    assert_true(net_saw_window_open);
    assert_false(net_saw_scheduler_ok);
    assert_true(thermal_saw_window_closed);
    assert_true(thermal_saw_scheduler_ok);

    /* Verify init_mm segments */
    assert_eq(PMMngr.start_code, init_mm.start_code);
    assert_eq(PMMngr.end_code, init_mm.end_code);
    assert_eq((uint64_t)&_data, init_mm.start_data);
    assert_eq(PMMngr.end_data, init_mm.end_data);
    assert_eq((uint64_t)&_rodata, init_mm.start_rodata);
    assert_eq(PMMngr.end_rodata, init_mm.end_rodata);
    assert_eq(PMMngr.start_brk, init_mm.end_brk);
    assert_eq(0x600000, init_mm.start_stack);
}

static void test_stdio_failure_stops(void)
{
    reset_sequence_state();
    mock_stdio_rc = -5;

    int val = setjmp(g_seq_jmp);
    if (val == 0) {
        kernel_start_userspace();
    }

    assert_eq(1, val);
    assert_str_eq("stdio", last_fail_stage);
    assert_str_eq("dev_tty", last_fail_comp);
    assert_eq(-5, last_fail_rc);

    assert_true(find_seq("stdio") >= 0);
    assert_eq(-1, find_seq("end"));
    assert_eq(-1, find_seq("spawn"));
}

static void test_spawn_failure_negative_stops(void)
{
    reset_sequence_state();
    mock_spawn_rc = -11; /* -EAGAIN */

    int val = setjmp(g_seq_jmp);
    if (val == 0) {
        kernel_start_userspace();
    }

    assert_eq(1, val);
    assert_str_eq("init", last_fail_stage);
    assert_str_eq("/bin/init", last_fail_comp);
    assert_eq(-11, last_fail_rc);

    assert_true(find_seq("end") >= 0);
    assert_true(find_seq("spawn") >= 0);
    assert_eq(-1, find_seq("probe"));
    assert_eq(-1, find_seq("net"));
    assert_eq(-1, find_seq("schedstart"));
}

static void test_spawn_failure_positive_stops(void)
{
    reset_sequence_state();
    mock_spawn_rc = 2; /* not PID 1 */

    int val = setjmp(g_seq_jmp);
    if (val == 0) {
        kernel_start_userspace();
    }

    assert_eq(1, val);
    assert_str_eq("init", last_fail_stage);
    assert_str_eq("/bin/init", last_fail_comp);
    assert_eq(2, last_fail_rc);

    assert_true(find_seq("spawn") >= 0);
    assert_eq(-1, find_seq("probe"));
    assert_eq(-1, find_seq("net"));
}

int main(void)
{
    TEST_SUITE("BOOT-5 test_boot_services_sequence");

    test_normal_sequence();
    test_stdio_failure_stops();
    test_spawn_failure_negative_stops();
    test_spawn_failure_positive_stops();

    TEST_RESULTS();
    return 0;
}
