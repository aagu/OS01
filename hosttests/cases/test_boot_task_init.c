/* hosttests/cases/test_boot_task_init.c — BOOT-3 Task 3 task_init startup barrier test.
 *
 * Verifies task_init() in x86 sched_boot.c:
 *   - Stdio failure halts immediately via boot_failure_stop("stdio", component, rc, -1, -1)
 *     and DOES NOT call spawn_user_task.
 *   - spawn_user_task negative return halts immediately via boot_failure_stop("init", "/bin/init", rc, -1, -1)
 *     and DOES NOT call probe_create, net_lwip_init, sched_boot_start, or thermal worker.
 *   - Success calls spawn_user_task(1) and proceeds through net/sched startup.
 */

#include "test_framework.h"
#include <sched/task.h>
#include <sched/boot.h>
#include <sched/boot_stdio.h>
#include <fs/vfs.h>
#include <fs/file.h>
#include <core/boot_failure.h>
#include <errno.h>
#include <string.h>
#include <setjmp.h>
#include <stdlib.h>

/* Trackers */
static int stdio_prepare_calls = 0;
static int spawn_user_task_calls = 0;
static int net_lwip_init_calls = 0;
static int sched_boot_start_calls = 0;
static int vfs_end_bootstrap_calls = 0;
static int thermal_worker_calls = 0;

enum {
    EV_NONE = 0,
    EV_STDIO_PREPARE = 1,
    EV_VFS_END_BOOTSTRAP = 2,
    EV_SPAWN_USER_TASK = 3,
    EV_NET_INIT = 4,
    EV_SCHED_BOOT_START = 5,
    EV_THERMAL = 6,
    EV_BOOT_STOP = 7,
};
static int recorded_events[32];
static int recorded_event_count = 0;
static void record_ev(int ev)
{
    if (recorded_event_count < 32)
        recorded_events[recorded_event_count++] = ev;
}

static int mock_stdio_rc = 0;
static const char *mock_stdio_component = "none";
static int64_t mock_spawn_rc = 1;

/* boot_failure interception */
static int stop_calls = 0;
static char last_stop_stage[32];
static char last_stop_component[64];
static int64_t last_stop_rc = 0;
static int last_stop_phase = 0;
static int last_stop_cpu = 0;
static jmp_buf test_exit_jmp;
static int jump_on_stop = 0;
static int jump_on_net = 0;

__attribute__((noreturn))
void boot_failure_stop(const char *stage, const char *component,
                       int64_t rc, int phase, int cpu)
{
    record_ev(EV_BOOT_STOP);
    stop_calls++;
    snprintf(last_stop_stage, sizeof(last_stop_stage), "%s", stage ? stage : "unknown");
    snprintf(last_stop_component, sizeof(last_stop_component), "%s", component ? component : "unknown");
    last_stop_rc = rc;
    last_stop_phase = phase;
    last_stop_cpu = cpu;

    if (jump_on_stop) {
        longjmp(test_exit_jmp, 1);
    }
    abort();
}

void boot_failure_warn(const char *stage, const char *component,
                       int64_t rc, int phase, int cpu)
{
    (void)stage; (void)component; (void)rc; (void)phase; (void)cpu;
}

/* Logging mocks */
int g_log_level = 0;
void _log_info_impl(const char *f, ...) { (void)f; }
void _log_warn_impl(const char *f, ...) { (void)f; }
void _log_err_impl(const char *f, ...) { (void)f; }
void _log_debug_impl(const char *f, ...) { (void)f; }

/* Sched & arch mocks */
union task_union init_task_union;
task_t *host_test_current = &init_task_union.task;
mm_t init_mm;

/* Memory & symbol stubs for sched_boot.c init_mm setup */
struct {
    uint64_t start_code, end_code;
    uint64_t end_data;
    uint64_t end_rodata;
    uint64_t start_brk;
} PMMngr;

char _data;
char _rodata;
uint64_t _stack_start;
void schedule(void) {}
void arch_local_irq_enable(void) {}

/* Temporary stubs for old stdio block before replacing with sched_boot_prepare_stdio */
files_t *files_alloc(void) { return NULL; }
uint64_t task_nofile_soft(task_t *tsk) { (void)tsk; return 64; }
vfs_node_t *vfs_lookup(const char *path) { (void)path; return NULL; }
file_t *file_alloc(void) { return NULL; }
vfs_node_t *vfs_node_get(vfs_node_t *n) { return n; }
int fd_alloc_limit(files_t *fs, file_t *f, uint32_t min, uint32_t lim) { (void)fs; (void)f; (void)min; (void)lim; return 0; }
void file_put(file_t *f) { (void)f; }

void arch_task_init_platform(void) {}
void sched_boot_publish_idle(void) {}

int sched_boot_prepare_stdio(task_t *idle, const char **failed_component)
{
    (void)idle;
    record_ev(EV_STDIO_PREPARE);
    stdio_prepare_calls++;
    if (failed_component) *failed_component = mock_stdio_component;
    return mock_stdio_rc;
}

int64_t spawn_user_task(const char *path, const char *const *argv)
{
    (void)path; (void)argv;
    record_ev(EV_SPAWN_USER_TASK);
    spawn_user_task_calls++;
    return mock_spawn_rc;
}

void sched_boot_probe_create(void) {}

void net_lwip_init(void)
{
    record_ev(EV_NET_INIT);
    net_lwip_init_calls++;
    if (jump_on_net) {
        longjmp(test_exit_jmp, 2);
    }
}

void sched_boot_start(void)
{
    record_ev(EV_SCHED_BOOT_START);
    sched_boot_start_calls++;
}

void vfs_end_bootstrap(void)
{
    record_ev(EV_VFS_END_BOOTSTRAP);
    vfs_end_bootstrap_calls++;
}

int arch_thermal_start_worker(void)
{
    record_ev(EV_THERMAL);
    thermal_worker_calls++;
    return 0;
}

/* Prototype of task_init */
void task_init(void);

static void reset_task_init_state(void)
{
    memset(&init_task_union, 0, sizeof(init_task_union));
    stdio_prepare_calls = 0;
    spawn_user_task_calls = 0;
    net_lwip_init_calls = 0;
    sched_boot_start_calls = 0;
    vfs_end_bootstrap_calls = 0;
    thermal_worker_calls = 0;
    recorded_event_count = 0;
    memset(recorded_events, 0, sizeof(recorded_events));
    mock_stdio_rc = 0;
    mock_stdio_component = "none";
    mock_spawn_rc = 1;
    stop_calls = 0;
    last_stop_stage[0] = '\0';
    last_stop_component[0] = '\0';
    last_stop_rc = 0;
    last_stop_phase = 0;
    last_stop_cpu = 0;
    jump_on_stop = 1;
    jump_on_net = 0;
}

static void test_task_init_stdio_failure(void)
{
    reset_task_init_state();
    mock_stdio_rc = -ENOMEM;
    mock_stdio_component = "files";

    if (setjmp(test_exit_jmp) == 0) {
        task_init();
        assert_true(false); /* Should not be reached */
    }

    assert_eq(1, stop_calls);
    assert_str_eq("stdio", last_stop_stage);
    assert_str_eq("files", last_stop_component);
    assert_eq(-ENOMEM, last_stop_rc);
    assert_eq(0, vfs_end_bootstrap_calls);
    assert_eq(0, spawn_user_task_calls);
    assert_eq(0, net_lwip_init_calls);
    assert_eq(0, sched_boot_start_calls);
}

static void test_task_init_spawn_negative_failure(void)
{
    reset_task_init_state();
    mock_stdio_rc = 0;
    mock_spawn_rc = -1; /* ELF missing */

    if (setjmp(test_exit_jmp) == 0) {
        task_init();
        assert_true(false); /* Should not be reached */
    }

    assert_eq(1, stop_calls);
    assert_str_eq("init", last_stop_stage);
    assert_str_eq("/bin/init", last_stop_component);
    assert_eq(-1, last_stop_rc);
    assert_eq(1, vfs_end_bootstrap_calls);
    assert_eq(1, spawn_user_task_calls);
    assert_eq(0, net_lwip_init_calls);
    assert_eq(0, sched_boot_start_calls);
    assert_eq(0, thermal_worker_calls);

    /* Assert sequence: stdio -> vfs_end_bootstrap -> spawn_user_task -> stop */
    assert_eq(EV_STDIO_PREPARE, recorded_events[0]);
    assert_eq(EV_VFS_END_BOOTSTRAP, recorded_events[1]);
    assert_eq(EV_SPAWN_USER_TASK, recorded_events[2]);
    assert_eq(EV_BOOT_STOP, recorded_events[3]);

    /* Test -ENOMEM */
    reset_task_init_state();
    mock_stdio_rc = 0;
    mock_spawn_rc = -ENOMEM;

    if (setjmp(test_exit_jmp) == 0) {
        task_init();
        assert_true(false); /* Should not be reached */
    }

    assert_eq(1, stop_calls);
    assert_str_eq("init", last_stop_stage);
    assert_str_eq("/bin/init", last_stop_component);
    assert_eq(-ENOMEM, last_stop_rc);
    assert_eq(1, vfs_end_bootstrap_calls);
    assert_eq(1, spawn_user_task_calls);
    assert_eq(0, net_lwip_init_calls);

    assert_eq(EV_STDIO_PREPARE, recorded_events[0]);
    assert_eq(EV_VFS_END_BOOTSTRAP, recorded_events[1]);
    assert_eq(EV_SPAWN_USER_TASK, recorded_events[2]);
    assert_eq(EV_BOOT_STOP, recorded_events[3]);
}

static void test_task_init_success_path(void)
{
    reset_task_init_state();
    mock_stdio_rc = 0;
    mock_spawn_rc = 1;
    jump_on_net = 1; /* Stop at net_lwip_init so we don't enter hlt idle loop */

    if (setjmp(test_exit_jmp) == 0) {
        task_init();
        assert_true(false);
    }

    assert_eq(0, stop_calls);
    assert_eq(1, stdio_prepare_calls);
    assert_eq(1, vfs_end_bootstrap_calls);
    assert_eq(1, spawn_user_task_calls);
    assert_eq(1, net_lwip_init_calls);

    /* Assert sequence: stdio -> vfs_end_bootstrap -> spawn_user_task -> net */
    assert_eq(EV_STDIO_PREPARE, recorded_events[0]);
    assert_eq(EV_VFS_END_BOOTSTRAP, recorded_events[1]);
    assert_eq(EV_SPAWN_USER_TASK, recorded_events[2]);
    assert_eq(EV_NET_INIT, recorded_events[3]);
}

int main(void)
{
    TEST_SUITE("BOOT-3 test_boot_task_init");

    test_task_init_stdio_failure();
    test_task_init_spawn_negative_failure();
    test_task_init_success_path();

    TEST_RESULTS();
    return 0;
}
