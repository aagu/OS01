#include "test_framework.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sched/task.h>
#include <sched/internal.h>
#include <memory/uaccess.h>
#include <syscall/dispatch.h>
#include <arch/syscall.h>
#include <arch/clocksource.h>
#include <uapi/syscall.h>

/* ── Controlled host-test runtime for real wait.c + sys_native.c ── */

union task_union init_task_union;
spinlock_T task_list_lock;
static task_t g_parent;
task_t *host_test_current = &g_parent;

list_t *task_list_next(list_t *pos)
{
    return pos ? pos->next : NULL;
}

static int g_prepare_rc = 0;
static int g_prepare_calls = 0;
static uint64_t g_prepare_last_addr = 0;
static uint64_t g_prepare_last_len = 0;

int syscall_prepare_user_write(uint64_t addr, uint64_t len)
{
    g_prepare_calls++;
    g_prepare_last_addr = addr;
    g_prepare_last_len = len;
    return g_prepare_rc;
}

static int g_copy_to_user_calls = 0;
static ssize_t g_copy_to_user_rc = 0; /* 0 = normal copy */

ssize_t copy_to_user_ft_res(void *dst, const void *src, size_t n,
                            void (*on_fault)(void *), void *arg)
{
    (void)on_fault;
    (void)arg;
    g_copy_to_user_calls++;
    if (g_copy_to_user_rc < 0)
        return g_copy_to_user_rc;
    if (dst && src)
        memcpy(dst, src, n);
    return (ssize_t)n;
}

ssize_t copy_from_user_ft_res(void *dst, const void *src, size_t n,
                              void (*on_fault)(void *), void *arg)
{
    (void)on_fault;
    (void)arg;
    if (dst && src)
        memcpy(dst, src, n);
    return (ssize_t)n;
}

static int g_free_calls = 0;
static task_t *g_freed_tasks[8];

void sched_free_reaped_task(task_t *child)
{
    if (g_free_calls < 8)
        g_freed_tasks[g_free_calls] = child;
    g_free_calls++;
}

static int g_blocker_calls = 0;
static int g_blocker_last_type = -1;
static bool g_blocker_last_sig = false;
static void (*g_blocker_hook)(int call_idx, blocker_check_t check) = NULL;
static int g_blocker_rcs[8];

int blocker_wait(blocker_check_t check, int type, bool signal_can_wake)
{
    int idx = g_blocker_calls++;
    g_blocker_last_type = type;
    g_blocker_last_sig = signal_can_wake;
    if (g_blocker_hook)
        g_blocker_hook(idx, check);
    return g_blocker_rcs[idx];
}

/* Linker stubs for unused sys_native.c / log references. */
__attribute__((noreturn)) void arch_syscall_exit(uint64_t status)
{
    (void)status;
    abort();
}
int64_t arch_syscall_fork(void *arch_frame) { (void)arch_frame; return -ENOSYS; }
uint64_t arch_clocksource_read_ns(void) { return 0; }
int g_log_level = 0;
void _log_err_impl(const char *fmt, ...) { (void)fmt; }
void _log_info_impl(const char *fmt, ...) { (void)fmt; }

void *kmalloc(size_t size) { return malloc(size); }
size_t kfree(void *address) { free(address); return 0; }

static bool g_strnlen_force_fault = false;
static uint64_t g_strnlen_fault_addr = 0;

int syscall_strnlen_user(uint64_t addr, size_t max)
{
    if (g_strnlen_force_fault || addr == 0 || addr == g_strnlen_fault_addr)
        return -EFAULT;
    if (max == 0)
        return 0;
    const char *s = (const char *)(uintptr_t)addr;
    size_t n = 0;
    while (n < max && s[n] != '\0')
        n++;
    return (int)n;
}

static int g_exec_calls = 0;
static int64_t g_exec_rc = 0;
static void *g_exec_frame = NULL;
static char g_exec_path[256];
static int g_exec_argc = 0;
static char g_exec_argv[8][64];
static int g_exec_envc = 0;
static char g_exec_envp[8][64];

int64_t arch_syscall_exec(void *arch_frame, const char *path,
                          const char *const *argv, const char *const *envp)
{
    g_exec_calls++;
    g_exec_frame = arch_frame;
    if (path)
        snprintf(g_exec_path, sizeof(g_exec_path), "%s", path);
    else
        g_exec_path[0] = '\0';
    g_exec_argc = 0;
    if (argv) {
        while (argv[g_exec_argc] && g_exec_argc < 8) {
            snprintf(g_exec_argv[g_exec_argc], sizeof(g_exec_argv[0]), "%s",
                     argv[g_exec_argc]);
            g_exec_argc++;
        }
    }
    g_exec_envc = 0;
    if (envp) {
        while (envp[g_exec_envc] && g_exec_envc < 8) {
            snprintf(g_exec_envp[g_exec_envc], sizeof(g_exec_envp[0]), "%s",
                     envp[g_exec_envc]);
            g_exec_envc++;
        }
    }
    return g_exec_rc;
}

extern int64_t sys_native_waitpid(syscall_ctx_t *ctx);
extern int64_t sys_native_exec(syscall_ctx_t *ctx);

static void reset_env(void)
{
    memset(&init_task_union, 0, sizeof(init_task_union));
    list_init(&init_task_union.task.list);
    spin_init(&task_list_lock);

    memset(&g_parent, 0, sizeof(g_parent));
    g_parent.pid = 10;
    g_parent.state = TASK_RUNNING;
    g_parent.flags = PF_PROCESS;
    list_add_to_before(&init_task_union.task.list, &g_parent.list);

    g_prepare_rc = 0;
    g_prepare_calls = 0;
    g_prepare_last_addr = 0;
    g_prepare_last_len = 0;

    g_copy_to_user_calls = 0;
    g_copy_to_user_rc = 0;

    g_free_calls = 0;
    memset(g_freed_tasks, 0, sizeof(g_freed_tasks));

    g_blocker_calls = 0;
    g_blocker_last_type = -1;
    g_blocker_last_sig = false;
    g_blocker_hook = NULL;
    memset(g_blocker_rcs, 0, sizeof(g_blocker_rcs));

    g_strnlen_force_fault = false;
    g_strnlen_fault_addr = 0;
    g_exec_calls = 0;
    g_exec_rc = 0;
    g_exec_frame = NULL;
    memset(g_exec_path, 0, sizeof(g_exec_path));
    g_exec_argc = 0;
    memset(g_exec_argv, 0, sizeof(g_exec_argv));
    g_exec_envc = 0;
    memset(g_exec_envp, 0, sizeof(g_exec_envp));
}

static void init_child(task_t *c, int64_t pid, int64_t exit_code, int64_t state,
                       uint32_t on_cpu)
{
    memset(c, 0, sizeof(*c));
    c->pid = pid;
    c->parent = &g_parent;
    c->exit_code = exit_code;
    c->state = state;
    c->on_cpu = on_cpu;
    c->flags = PF_PROCESS;
    list_add_to_before(&init_task_union.task.list, &c->list);
}

static void test_waitpid_reap_specific_and_any(void)
{
    TEST_SUITE("waitpid reaps specific pid and sign-extended -1");
    reset_env();

    task_t c1, c2;
    init_child(&c1, 101, 0x2a << 8, TASK_ZOMBIE, 0);
    init_child(&c2, 102, 0x3b << 8, TASK_ZOMBIE, 0);

    int status = 0;
    syscall_ctx_t ctx = {
        .nr = SYS_waitpid,
        .args = { 102, (uint64_t)(uintptr_t)&status, 0 }
    };
    assert_eq(102, sys_native_waitpid(&ctx));
    assert_eq(1, g_prepare_calls);
    assert_eq((uint64_t)(uintptr_t)&status, g_prepare_last_addr);
    assert_eq(sizeof(int), g_prepare_last_len);
    assert_eq(1, g_copy_to_user_calls);
    assert_eq(0x3b << 8, status);
    assert_eq(1, g_free_calls);
    assert_true(g_freed_tasks[0] == &c2);
    assert_null(c2.list.next);
    assert_null(c2.list.prev);

    /* 32-bit -1 (0xffffffffULL) must sign-extend to -1 in sys_native_waitpid. */
    status = 0;
    ctx = (syscall_ctx_t){
        .nr = SYS_waitpid,
        .args = { 0xffffffffULL, (uint64_t)(uintptr_t)&status, 0 }
    };
    assert_eq(101, sys_native_waitpid(&ctx));
    assert_eq(0x2a << 8, status);
    assert_eq(2, g_free_calls);
    assert_true(g_freed_tasks[1] == &c1);
}

static void test_waitpid_null_status(void)
{
    TEST_SUITE("waitpid with NULL user_status skips prepare and copy");
    reset_env();

    task_t c1;
    init_child(&c1, 103, 0x11 << 8, TASK_ZOMBIE, 0);

    syscall_ctx_t ctx = {
        .nr = SYS_waitpid,
        .args = { 103, 0, 0 }
    };
    assert_eq(103, sys_native_waitpid(&ctx));
    assert_eq(0, g_prepare_calls);
    assert_eq(0, g_copy_to_user_calls);
    assert_eq(1, g_free_calls);
    assert_true(g_freed_tasks[0] == &c1);
}

static void test_waitpid_wnohang_and_on_cpu_guard(void)
{
    TEST_SUITE("waitpid WNOHANG returns 0 when running or zombie on_cpu==1");
    reset_env();

    task_t c_run, c_oncpu;
    init_child(&c_run, 104, 0, TASK_RUNNING, 0);
    init_child(&c_oncpu, 105, 0x77 << 8, TASK_ZOMBIE, 1);

    int status = -1;
    assert_eq(0, do_waitpid(104, &status, WNOHANG));
    assert_eq(0, do_waitpid(105, &status, WNOHANG));
    assert_eq(0, g_free_calls);
    assert_eq(-1, status);
    assert_not_null(c_run.list.next);
    assert_not_null(c_oncpu.list.next);

    /* Once on_cpu clears, WNOHANG reaps c_oncpu. */
    c_oncpu.on_cpu = 0;
    assert_eq(105, do_waitpid(105, &status, WNOHANG));
    assert_eq(0x77 << 8, status);
    assert_eq(1, g_free_calls);
}

static task_t *g_hook_child = NULL;
static bool g_unblock_on_call0 = false;
static bool g_unblock_on_call1 = false;

static void blocking_hook(int call_idx, blocker_check_t check)
{
    if (call_idx == 0) {
        g_unblock_on_call0 = check(&g_parent);
    } else if (call_idx == 1 && g_hook_child) {
        g_hook_child->state = TASK_ZOMBIE;
        g_hook_child->on_cpu = 0;
        g_hook_child->exit_code = 0x42 << 8;
        g_unblock_on_call1 = check(&g_parent);
    }
}

static void test_waitpid_blocking_wakeup_and_eintr_retry(void)
{
    TEST_SUITE("waitpid blocks via blocker_wait, retries on -EINTR, and reaps");
    reset_env();

    task_t c1;
    init_child(&c1, 106, 0, TASK_RUNNING, 1);
    g_hook_child = &c1;
    g_unblock_on_call0 = true;
    g_unblock_on_call1 = false;
    g_blocker_rcs[0] = -EINTR;
    g_blocker_rcs[1] = 0;
    g_blocker_hook = blocking_hook;

    int status = 0;
    assert_eq(106, do_waitpid(106, &status, 0));
    assert_eq(2, g_blocker_calls);
    assert_eq(BLOCKER_WAITPID, g_blocker_last_type);
    assert_true(g_blocker_last_sig);
    assert_false(g_unblock_on_call0);
    assert_true(g_unblock_on_call1);
    assert_eq(0x42 << 8, status);
    assert_eq(1, g_free_calls);
}

static void test_waitpid_echild_cases(void)
{
    TEST_SUITE("waitpid returns -ECHILD for missing/non-child/pgrp/reaped");
    reset_env();

    assert_eq(-ECHILD, do_waitpid(-1, NULL, 0));
    assert_eq(-ECHILD, do_waitpid(0, NULL, 0));
    assert_eq(-ECHILD, do_waitpid(-2, NULL, 0));

    task_t other_parent, foreign_child, own_child;
    memset(&other_parent, 0, sizeof(other_parent));
    other_parent.pid = 99;
    init_child(&foreign_child, 201, 0, TASK_ZOMBIE, 0);
    foreign_child.parent = &other_parent;

    assert_eq(-ECHILD, do_waitpid(201, NULL, 0));
    assert_eq(-ECHILD, do_waitpid(-1, NULL, 0));

    init_child(&own_child, 202, 0x05 << 8, TASK_ZOMBIE, 0);
    assert_eq(202, do_waitpid(202, NULL, 0));
    assert_eq(-ECHILD, do_waitpid(202, NULL, 0));
}

static void test_waitpid_bad_status_preserves_zombie_and_rejects_without_children(void)
{
    TEST_SUITE("sys_native_waitpid rejects bad status before reaping or -ECHILD");
    reset_env();

    /* No children + bad status -> -EFAULT / -ENOMEM takes precedence over -ECHILD. */
    g_prepare_rc = -EFAULT;
    syscall_ctx_t ctx = {
        .nr = SYS_waitpid,
        .args = { (uint64_t)-1, 0x1, 0 }
    };
    assert_eq(-EFAULT, sys_native_waitpid(&ctx));

    g_prepare_rc = -ENOMEM;
    assert_eq(-ENOMEM, sys_native_waitpid(&ctx));

    /* With a ZOMBIE child, bad status must NOT detach or free the child. */
    task_t c1;
    init_child(&c1, 107, 0x55 << 8, TASK_ZOMBIE, 0);
    g_prepare_rc = -EFAULT;
    ctx.args[0] = 107;
    assert_eq(-EFAULT, sys_native_waitpid(&ctx));
    assert_eq(0, g_free_calls);
    assert_not_null(c1.list.next);

    /* Retrying with a valid status pointer still reaps the preserved zombie. */
    int status = 0;
    g_prepare_rc = 0;
    ctx.args[1] = (uint64_t)(uintptr_t)&status;
    assert_eq(107, sys_native_waitpid(&ctx));
    assert_eq(0x55 << 8, status);
    assert_eq(1, g_free_calls);
    assert_true(g_freed_tasks[0] == &c1);
}

static void test_sys_native_exec(void)
{
    TEST_SUITE("sys_native_exec marshals path/argv/envp and sets suppress_writeback");
    reset_env();

    /* NULL / bad path returns -EFAULT without suppress_writeback. */
    syscall_ctx_t ctx = {
        .nr = SYS_exec,
        .args = { 0, 0, 0 },
        .arch_frame = &ctx,
        .suppress_writeback = false,
    };
    assert_eq(-EFAULT, sys_native_exec(&ctx));
    assert_false(ctx.suppress_writeback);
    assert_eq(0, g_exec_calls);

    g_strnlen_fault_addr = 0xdeadbeefULL;
    ctx.args[0] = 0xdeadbeefULL;
    assert_eq(-EFAULT, sys_native_exec(&ctx));
    assert_false(ctx.suppress_writeback);
    assert_eq(0, g_exec_calls);
    g_strnlen_fault_addr = 0;

    /* Path >= PATH_MAX (256) returns -ENAMETOOLONG. */
    char long_path[PATH_MAX + 8];
    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    ctx.args[0] = (uint64_t)(uintptr_t)long_path;
    assert_eq(-ENAMETOOLONG, sys_native_exec(&ctx));
    assert_false(ctx.suppress_writeback);
    assert_eq(0, g_exec_calls);

    /* Valid path + argv + envp -> deep copied and passed to arch_syscall_exec. */
    const char *argv[] = { "/bin/exec_child", "cow_arg", "tail", NULL };
    const char *envp[] = { "K=V", NULL };
    ctx = (syscall_ctx_t){
        .nr = SYS_exec,
        .args = {
            (uint64_t)(uintptr_t)"/bin/exec_child",
            (uint64_t)(uintptr_t)argv,
            (uint64_t)(uintptr_t)envp,
        },
        .arch_frame = &ctx,
        .suppress_writeback = false,
    };
    g_exec_rc = 0;
    assert_eq(0, sys_native_exec(&ctx));
    assert_true(ctx.suppress_writeback);
    assert_eq(1, g_exec_calls);
    assert_true(g_exec_frame == &ctx);
    assert_true(strcmp(g_exec_path, "/bin/exec_child") == 0);
    assert_eq(3, g_exec_argc);
    assert_true(strcmp(g_exec_argv[0], "/bin/exec_child") == 0);
    assert_true(strcmp(g_exec_argv[1], "cow_arg") == 0);
    assert_true(strcmp(g_exec_argv[2], "tail") == 0);
    assert_eq(1, g_exec_envc);
    assert_true(strcmp(g_exec_envp[0], "K=V") == 0);

    /* Failed arch_syscall_exec (-ENOENT) must NOT set suppress_writeback. */
    ctx.suppress_writeback = false;
    g_exec_rc = -ENOENT;
    assert_eq(-ENOENT, sys_native_exec(&ctx));
    assert_false(ctx.suppress_writeback);
    assert_eq(2, g_exec_calls);
}

int main(void)
{
    test_waitpid_reap_specific_and_any();
    test_waitpid_null_status();
    test_waitpid_wnohang_and_on_cpu_guard();
    test_waitpid_blocking_wakeup_and_eintr_retry();
    test_waitpid_echild_cases();
    test_waitpid_bad_status_preserves_zombie_and_rejects_without_children();
    test_sys_native_exec();
    printf("\n%s: %d total, %d passed, %d failed\n",
           "test_waitpid",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
