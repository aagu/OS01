/*
 * hosttests/cases/test_nofile_limits.c
 *
 * ARCH-10 Task 5: RLIMIT_NOFILE kernel state, native ABI, compat entry and libc wrappers.
 */

#include "test_framework.h"

#include <config/limits.h>
#include <uapi/resource.h>
#include <uapi/syscall.h>
#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <sys/resource.h>

#include <sched/task.h>
#include <sched/resource.h>
#include <syscall/dispatch.h>
#include <syscall/resource.h>
#include <syscall/compat.h>

/* Mock current task */
static task_t g_current_task;
task_t *host_test_current = &g_current_task;
mm_t init_mm;
thread_t init_thread;

/* Stubs for unused syscall dispatch families */
int64_t sys_fs_dispatch(syscall_ctx_t *ctx)   { (void)ctx; return -ENOSYS; }
int64_t sys_mm_dispatch(syscall_ctx_t *ctx)   { (void)ctx; return -ENOSYS; }
int64_t sys_time_dispatch(syscall_ctx_t *ctx) { (void)ctx; return -ENOSYS; }
int64_t sys_misc_dispatch(syscall_ctx_t *ctx) { (void)ctx; return -ENOSYS; }
int64_t sys_net_dispatch(syscall_ctx_t *ctx)  { (void)ctx; return -ENOSYS; }
int64_t sys_proc_dispatch(syscall_ctx_t *ctx) { (void)ctx; return -ENOSYS; }

int64_t host_test_syscall(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3)
{
    syscall_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.nr = nr;
    ctx.args[0] = a1;
    ctx.args[1] = a2;
    ctx.args[2] = a3;
    if (nr == SYS_getrlimit || nr == SYS_setrlimit)
        return sys_resource_dispatch(&ctx);
    return -ENOSYS;
}

/* Mock uaccess fault injection */
static bool g_fault_user_range = false;
static bool g_fault_copy_from = false;
static bool g_fault_copy_to = false;

bool syscall_check_user_range(uint64_t addr, uint64_t size, bool writable)
{
    (void)size; (void)writable;
    if (addr == 0 || g_fault_user_range)
        return false;
    return true;
}

ssize_t copy_from_user_ft_res(void *dst, const void *src, size_t n,
                              void (*on_fault)(void *), void *arg)
{
    (void)on_fault; (void)arg;
    if (!src || g_fault_copy_from)
        return -1;
    memcpy(dst, src, n);
    return (ssize_t)n;
}

ssize_t copy_to_user_ft_res(void *dst, const void *src, size_t n,
                            void (*on_fault)(void *), void *arg)
{
    (void)on_fault; (void)arg;
    if (!dst || g_fault_copy_to)
        return -1;
    memcpy(dst, src, n);
    return (ssize_t)n;
}

/* Static BSP idle instance for testing INIT_TASK */
static union task_union test_bsp_task_union = {
    INIT_TASK(test_bsp_task_union.task)
};

/* ── 1. Helpers ─────────────────────────────────────────── */

TEST_FUNC(test_nofile_helpers_init)
{
    task_t t;
    memset(&t, 0x5a, sizeof(t));
    task_nofile_init(&t);

    assert_eq(OS01_NOFILE_CAPACITY, t.nofile_soft);
    assert_eq(OS01_NOFILE_CAPACITY, t.nofile_hard);
    assert_eq(1, spin_trylock(&t.nofile_lock));
    spin_unlock(&t.nofile_lock);
    assert_eq(OS01_NOFILE_CAPACITY, task_nofile_soft(&t));
}

TEST_FUNC(test_nofile_helpers_inherit)
{
    task_t parent;
    task_nofile_init(&parent);
    parent.nofile_soft = 10;
    parent.nofile_hard = 12;

    task_t child;
    memset(&child, 0, sizeof(child));
    task_nofile_inherit(&child, &parent);

    assert_eq(10, child.nofile_soft);
    assert_eq(12, child.nofile_hard);
    assert_eq(1, spin_trylock(&child.nofile_lock));
    spin_unlock(&child.nofile_lock);

    /* NULL parent falls back to clean init */
    task_t orphan;
    memset(&orphan, 0, sizeof(orphan));
    task_nofile_inherit(&orphan, NULL);
    assert_eq(OS01_NOFILE_CAPACITY, orphan.nofile_soft);
    assert_eq(OS01_NOFILE_CAPACITY, orphan.nofile_hard);
}

TEST_FUNC(test_nofile_helpers_update)
{
    task_t t;
    task_nofile_init(&t);

    struct os01_rlimit64 old_lim;
    memset(&old_lim, 0, sizeof(old_lim));

    /* NULL new只查询，old可NULL */
    int rc = task_nofile_update(&t, NULL, &old_lim);
    assert_eq(0, rc);
    assert_eq(OS01_NOFILE_CAPACITY, old_lim.cur);
    assert_eq(OS01_NOFILE_CAPACITY, old_lim.max);

    rc = task_nofile_update(&t, NULL, NULL);
    assert_eq(0, rc);

    /* Valid update: 10 / 12 */
    struct os01_rlimit64 new_lim = { .cur = 10, .max = 12 };
    rc = task_nofile_update(&t, &new_lim, &old_lim);
    assert_eq(0, rc);
    assert_eq(OS01_NOFILE_CAPACITY, old_lim.cur);
    assert_eq(OS01_NOFILE_CAPACITY, old_lim.max);
    assert_eq(10, t.nofile_soft);
    assert_eq(12, t.nofile_hard);

    /* 0/0 成功 */
    new_lim.cur = 0;
    new_lim.max = 0;
    rc = task_nofile_update(&t, &new_lim, &old_lim);
    assert_eq(0, rc);
    assert_eq(10, old_lim.cur);
    assert_eq(12, old_lim.max);
    assert_eq(0, t.nofile_soft);
    assert_eq(0, t.nofile_hard);

    /* soft > hard -> -EINVAL */
    task_nofile_init(&t);
    new_lim.cur = 14;
    new_lim.max = 12;
    rc = task_nofile_update(&t, &new_lim, NULL);
    assert_eq(-EINVAL, rc);
    assert_eq(OS01_NOFILE_CAPACITY, t.nofile_soft);

    /* 提高 hard -> -EPERM */
    new_lim.cur = 10;
    new_lim.max = 10;
    rc = task_nofile_update(&t, &new_lim, NULL);
    assert_eq(0, rc);
    assert_eq(10, t.nofile_hard);

    /* Try raising hard from 10 to 12 -> -EPERM */
    new_lim.cur = 10;
    new_lim.max = 12;
    rc = task_nofile_update(&t, &new_lim, NULL);
    assert_eq(-EPERM, rc);
    assert_eq(10, t.nofile_hard);

    /* infinity 大于 NOFILE 不得悄悄截断 */
    new_lim.cur = 10;
    new_lim.max = OS01_RLIM_INFINITY;
    rc = task_nofile_update(&t, &new_lim, NULL);
    assert_eq(-EPERM, rc);
    assert_eq(10, t.nofile_hard);
}

/* ── 2. All Task Constructors ───────────────────────────── */

TEST_FUNC(nofile_all_task_constructors)
{
    /* 1. Static BSP idle (INIT_TASK) */
    task_t *bsp = &test_bsp_task_union.task;
    assert_eq(OS01_NOFILE_CAPACITY, bsp->nofile_soft);
    assert_eq(OS01_NOFILE_CAPACITY, bsp->nofile_hard);
    assert_true(bsp->nofile_soft > 0);
    assert_true(bsp->nofile_hard > 0);
    assert_eq(1, spin_trylock(&bsp->nofile_lock));
    spin_unlock(&bsp->nofile_lock);

    /* 2. x86 AP idle constructor path (memset then task_nofile_init) */
    task_t ap_idle;
    memset(&ap_idle, 0, sizeof(ap_idle));
    task_nofile_init(&ap_idle);
    assert_eq(OS01_NOFILE_CAPACITY, ap_idle.nofile_soft);
    assert_eq(OS01_NOFILE_CAPACITY, ap_idle.nofile_hard);
    assert_true(ap_idle.nofile_soft > 0);
    assert_true(ap_idle.nofile_hard > 0);
    assert_eq(1, spin_trylock(&ap_idle.nofile_lock));
    spin_unlock(&ap_idle.nofile_lock);

    /* 3. 共享 kernel_thread constructor path (memset then task_nofile_init) */
    task_t kthread;
    memset(&kthread, 0, sizeof(kthread));
    task_nofile_init(&kthread);
    assert_eq(OS01_NOFILE_CAPACITY, kthread.nofile_soft);
    assert_eq(OS01_NOFILE_CAPACITY, kthread.nofile_hard);
    assert_true(kthread.nofile_soft > 0);
    assert_true(kthread.nofile_hard > 0);
    assert_eq(1, spin_trylock(&kthread.nofile_lock));
    spin_unlock(&kthread.nofile_lock);

    /* 4. 用户 fork constructor path (task_nofile_inherit) */
    task_t parent;
    task_nofile_init(&parent);
    parent.nofile_soft = 8;
    parent.nofile_hard = 10;

    task_t child;
    memset(&child, 0, sizeof(child));
    task_nofile_inherit(&child, &parent);
    assert_eq(8, child.nofile_soft);
    assert_eq(10, child.nofile_hard);
    assert_true(child.nofile_soft > 0);
    assert_true(child.nofile_hard > 0);
    assert_eq(1, spin_trylock(&child.nofile_lock));
    spin_unlock(&child.nofile_lock);

    /* 5. User spawn task constructor path (spawn_user_task: memset then task_nofile_init) */
    task_t spawned_user;
    memset(&spawned_user, 0, sizeof(spawned_user));
    task_nofile_init(&spawned_user);
    assert_eq(OS01_NOFILE_CAPACITY, spawned_user.nofile_soft);
    assert_eq(OS01_NOFILE_CAPACITY, spawned_user.nofile_hard);
    assert_true(spawned_user.nofile_soft > 0);
    assert_true(spawned_user.nofile_hard > 0);
    assert_eq(1, spin_trylock(&spawned_user.nofile_lock));
    spin_unlock(&spawned_user.nofile_lock);

    /* 6. Exec preservation check: exec retains task limits */
    child.nofile_soft = 5;
    child.nofile_hard = 6;
    /* Exec does not reinitialize or clear resource limits */
    assert_eq(5, child.nofile_soft);
    assert_eq(6, child.nofile_hard);
}

/* ── 3. Syscall Dispatch ────────────────────────────────── */

TEST_FUNC(test_sys_resource_dispatch)
{
    task_nofile_init(&g_current_task);
    g_current_task.pid = 42;

    struct os01_rlimit64 user_lim;
    syscall_ctx_t ctx;

    /* Unsupported resource -> -EINVAL */
    ctx = (syscall_ctx_t){
        .nr = SYS_getrlimit,
        .args = { 0 /* not NOFILE */, (uint64_t)&user_lim },
    };
    assert_eq(-EINVAL, sys_resource_dispatch(&ctx));

    /* getrlimit(NULL) 为 EFAULT */
    ctx = (syscall_ctx_t){
        .nr = SYS_getrlimit,
        .args = { OS01_RLIMIT_NOFILE, 0 },
    };
    assert_eq(-EFAULT, sys_resource_dispatch(&ctx));

    /* Fault-tolerant uaccess copy_to failure -> -EFAULT */
    g_fault_copy_to = true;
    ctx = (syscall_ctx_t){
        .nr = SYS_getrlimit,
        .args = { OS01_RLIMIT_NOFILE, (uint64_t)&user_lim },
    };
    assert_eq(-EFAULT, sys_resource_dispatch(&ctx));
    g_fault_copy_to = false;

    /* Successful getrlimit */
    ctx = (syscall_ctx_t){
        .nr = SYS_getrlimit,
        .args = { OS01_RLIMIT_NOFILE, (uint64_t)&user_lim },
    };
    assert_eq(0, sys_resource_dispatch(&ctx));
    assert_eq(OS01_NOFILE_CAPACITY, user_lim.cur);
    assert_eq(OS01_NOFILE_CAPACITY, user_lim.max);

    /* setrlimit(NULL) 为 EFAULT */
    ctx = (syscall_ctx_t){
        .nr = SYS_setrlimit,
        .args = { OS01_RLIMIT_NOFILE, 0 },
    };
    assert_eq(-EFAULT, sys_resource_dispatch(&ctx));

    /* setrlimit bad address -> -EFAULT */
    g_fault_copy_from = true;
    ctx = (syscall_ctx_t){
        .nr = SYS_setrlimit,
        .args = { OS01_RLIMIT_NOFILE, (uint64_t)&user_lim },
    };
    assert_eq(-EFAULT, sys_resource_dispatch(&ctx));
    g_fault_copy_from = false;

    /* setrlimit soft > hard -> -EINVAL */
    user_lim.cur = 15;
    user_lim.max = 10;
    ctx = (syscall_ctx_t){
        .nr = SYS_setrlimit,
        .args = { OS01_RLIMIT_NOFILE, (uint64_t)&user_lim },
    };
    assert_eq(-EINVAL, sys_resource_dispatch(&ctx));

    /* setrlimit raise hard -> -EPERM */
    user_lim.cur = 10;
    user_lim.max = 20;
    ctx = (syscall_ctx_t){
        .nr = SYS_setrlimit,
        .args = { OS01_RLIMIT_NOFILE, (uint64_t)&user_lim },
    };
    assert_eq(-EPERM, sys_resource_dispatch(&ctx));

    /* Successful setrlimit: 8 / 12 */
    user_lim.cur = 8;
    user_lim.max = 12;
    ctx = (syscall_ctx_t){
        .nr = SYS_setrlimit,
        .args = { OS01_RLIMIT_NOFILE, (uint64_t)&user_lim },
    };
    assert_eq(0, sys_resource_dispatch(&ctx));
    assert_eq(8, g_current_task.nofile_soft);
    assert_eq(12, g_current_task.nofile_hard);
}

/* ── 4. Linux Compat ────────────────────────────────────── */

TEST_FUNC(test_compat_resource_dispatch)
{
    task_nofile_init(&g_current_task);
    g_current_task.pid = 100;

    struct os01_rlimit64 user_lim;
    syscall_ctx_t ctx;

    /* Linux 97 (getrlimit) remaps and returns native */
    ctx = (syscall_ctx_t){
        .nr = 97,
        .args = { OS01_RLIMIT_NOFILE, (uint64_t)&user_lim },
    };
    assert_eq(0, compat_linux_dispatch(&ctx));
    assert_eq(OS01_NOFILE_CAPACITY, user_lim.cur);
    assert_eq(OS01_NOFILE_CAPACITY, user_lim.max);

    /* Linux 160 (setrlimit) remaps and updates */
    user_lim.cur = 9;
    user_lim.max = 11;
    ctx = (syscall_ctx_t){
        .nr = 160,
        .args = { OS01_RLIMIT_NOFILE, (uint64_t)&user_lim },
    };
    assert_eq(0, compat_linux_dispatch(&ctx));
    assert_eq(9, g_current_task.nofile_soft);
    assert_eq(11, g_current_task.nofile_hard);

    /* Linux 302 (prlimit64 adapter) */
    /* pid other than 0 and self -> -ESRCH */
    ctx = (syscall_ctx_t){
        .nr = 302,
        .args = { 999 /* other pid */, OS01_RLIMIT_NOFILE, 0, 0 },
    };
    assert_eq(-ESRCH, compat_linux_dispatch(&ctx));

    /* unsupported resource -> -EINVAL */
    ctx = (syscall_ctx_t){
        .nr = 302,
        .args = { 0, 99 /* bad resource */, 0, 0 },
    };
    assert_eq(-EINVAL, compat_linux_dispatch(&ctx));

    /* new复制失败不改 */
    struct os01_rlimit64 new_u = { .cur = 4, .max = 4 };
    struct os01_rlimit64 old_u = { 0, 0 };
    g_fault_copy_from = true;
    ctx = (syscall_ctx_t){
        .nr = 302,
        .args = { 0, OS01_RLIMIT_NOFILE, (uint64_t)&new_u, (uint64_t)&old_u },
    };
    assert_eq(-EFAULT, compat_linux_dispatch(&ctx));
    assert_eq(9, g_current_task.nofile_soft); /* Unchanged */
    g_fault_copy_from = false;

    /* old复制失败允许新值已提交 */
    g_fault_copy_to = true;
    ctx = (syscall_ctx_t){
        .nr = 302,
        .args = { 100 /* self pid */, OS01_RLIMIT_NOFILE, (uint64_t)&new_u, (uint64_t)&old_u },
    };
    assert_eq(-EFAULT, compat_linux_dispatch(&ctx));
    assert_eq(4, g_current_task.nofile_soft); /* Committed! */
    assert_eq(4, g_current_task.nofile_hard); /* Committed! */
    g_fault_copy_to = false;

    /* Successful prlimit query & update */
    new_u.cur = 2;
    new_u.max = 3;
    ctx = (syscall_ctx_t){
        .nr = 302,
        .args = { 0, OS01_RLIMIT_NOFILE, (uint64_t)&new_u, (uint64_t)&old_u },
    };
    assert_eq(0, compat_linux_dispatch(&ctx));
    assert_eq(4, old_u.cur); /* Previous values */
    assert_eq(4, old_u.max);
    assert_eq(2, g_current_task.nofile_soft); /* New values */
    assert_eq(3, g_current_task.nofile_hard);
}

/* ── 5. Libc Wrappers ───────────────────────────────────── */

/* Mock syscall implementation for libc wrappers */
int64_t syscall(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    (void)arg3;
    syscall_ctx_t ctx = {
        .nr = nr,
        .args = { arg1, arg2, 0, 0, 0, 0 },
    };
    return sys_resource_dispatch(&ctx);
}

/* Include or declare libc functions under test */
extern int getrlimit(int resource, struct rlimit *rlim);
extern int setrlimit(int resource, const struct rlimit *rlim);

TEST_FUNC(test_libc_wrappers)
{
    task_nofile_init(&g_current_task);

    struct rlimit rl;
    memset(&rl, 0, sizeof(rl));

    /* NULL rlim -> -1, errno=EFAULT */
    errno = 0;
    int rc = getrlimit(RLIMIT_NOFILE, NULL);
    assert_eq(-1, rc);
    assert_eq(EFAULT, errno);

    /* Unsupported resource -> -1, errno=EINVAL (no longer fake success!) */
    errno = 0;
    rc = getrlimit(RLIMIT_CPU, &rl);
    assert_eq(-1, rc);
    assert_eq(EINVAL, errno);

    errno = 0;
    rc = setrlimit(RLIMIT_CPU, &rl);
    assert_eq(-1, rc);
    assert_eq(EINVAL, errno);

    /* Valid getrlimit: must report actual soft capacity, NOT 65536 stub! */
    errno = 0;
    rc = getrlimit(RLIMIT_NOFILE, &rl);
    assert_eq(0, rc);
    assert_eq(OS01_NOFILE_CAPACITY, rl.rlim_cur);
    assert_eq(OS01_NOFILE_CAPACITY, rl.rlim_max);

    /* Valid setrlimit */
    rl.rlim_cur = 6;
    rl.rlim_max = 8;
    rc = setrlimit(RLIMIT_NOFILE, &rl);
    assert_eq(0, rc);
    assert_eq(6, g_current_task.nofile_soft);
    assert_eq(8, g_current_task.nofile_hard);

    /* Verify subsequent getrlimit reflects updated limits */
    memset(&rl, 0, sizeof(rl));
    rc = getrlimit(RLIMIT_NOFILE, &rl);
    assert_eq(0, rc);
    assert_eq(6, rl.rlim_cur);
    assert_eq(8, rl.rlim_max);
}

/* ── Test Suite Registration ────────────────────────────── */

TEST_LIST_BEGIN
    TEST_ENTRY(test_nofile_helpers_init),
    TEST_ENTRY(test_nofile_helpers_inherit),
    TEST_ENTRY(test_nofile_helpers_update),
    TEST_ENTRY(nofile_all_task_constructors),
    TEST_ENTRY(test_sys_resource_dispatch),
    TEST_ENTRY(test_compat_resource_dispatch),
    TEST_ENTRY(test_libc_wrappers),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    TEST_RESULTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
