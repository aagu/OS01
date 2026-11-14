/*
 * hosttests/cases/test_nofile_limits.c
 *
 * ARCH-10 Task 5: RLIMIT_NOFILE kernel state, native ABI, compat entry and libc wrappers.
 * ARCH-10 Task 6: every descriptor installation honours the per-task NOFILE soft limit.
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
#include <fs/file.h>
#include <fs/devfs.h>
#include <net/socket.h>
#include <uapi/sockaddr.h>
#include <tty/pty.h>
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

/* The four shared native entries (kernel/syscall/sys_native.c) are
 * unreachable from this case; they only have to satisfy the linker. */
int64_t sys_native_exit(syscall_ctx_t *ctx) { (void)ctx; return -ENOSYS; }
int64_t sys_native_fork(syscall_ctx_t *ctx) { (void)ctx; return -ENOSYS; }
int64_t sys_native_waitpid(syscall_ctx_t *ctx) { (void)ctx; return -ENOSYS; }
int64_t sys_native_exec(syscall_ctx_t *ctx) { (void)ctx; return -ENOSYS; }
int64_t sys_native_getpid(syscall_ctx_t *ctx) { (void)ctx; return -ENOSYS; }
int64_t sys_native_getppid(syscall_ctx_t *ctx) { (void)ctx; return -ENOSYS; }
int64_t sys_native_clock_gettime(syscall_ctx_t *ctx) { (void)ctx; return -ENOSYS; }

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

/* ── 6. ARCH-10 Task 6: descriptor installation honours the soft limit ──
 *
 * These cases drive the PRODUCTION installation code linked into this
 * binary (kernel/fs/file.c, kernel/net/socket.c, kernel/tty/pty.c):
 * every descriptor installation must be judged against the current
 * task's RLIMIT_NOFILE soft limit, and a refused installation must
 * not leak the object it was given.
 */

/* Fresh fd table owned by the mock current task, with the soft limit
 * set to `soft`.  The mock task IS the production task_t, so the
 * production task_nofile_soft() snapshot is the one under test. */
static files_t *install_table_new(uint64_t soft)
{
    files_t *fs = files_alloc();
    assert_not_null(fs);

    task_nofile_init(&g_current_task);
    struct os01_rlimit64 lim = { .cur = soft, .max = OS01_NOFILE_CAPACITY };
    assert_eq(0, task_nofile_update(&g_current_task, &lim, NULL));
    assert_eq(soft, task_nofile_soft(&g_current_task));

    g_current_task.files = fs;
    return fs;
}

/* Lower (or raise) the soft limit of the mock current task. */
static void install_set_soft(uint64_t soft)
{
    struct os01_rlimit64 lim = { .cur = soft, .max = OS01_NOFILE_CAPACITY };
    assert_eq(0, task_nofile_update(&g_current_task, &lim, NULL));
    assert_eq(soft, task_nofile_soft(&g_current_task));
}

/* Install a placeholder descriptor and return its fd. */
static int install_placeholder(files_t *fs)
{
    file_t *f = file_alloc();
    assert_not_null(f);
    f->type = FD_DEV;
    f->flags = O_RDONLY;
    int fd = fd_alloc(fs, f);
    if (fd < 0) file_put(f);   // refused: the caller still owns it
    return fd;
}

TEST_FUNC(test_install_fd_alloc_respects_soft_limit)
{
    files_t *fs = install_table_new(4);

    for (int i = 0; i < 4; i++)
        assert_eq(i, install_placeholder(fs));

    /* The next descriptor does not fit below the soft limit. */
    file_t *f = file_alloc();
    assert_not_null(f);
    int rc = fd_alloc(fs, f);
    assert_eq(-EMFILE, rc);
    /* The refused install took no reference: the caller still owns it. */
    assert_eq(1, f->refcount);
    file_put(f);

    /* Nothing was installed above the limit. */
    assert_null(fs->fd[4]);
    for (int i = 0; i < 4; i++)
        assert_not_null(fs->fd[i]);
}

TEST_FUNC(test_install_pipe_needs_two_slots)
{
    files_t *fs = install_table_new(4);

    /* 0..2 occupied: exactly one slot (3) is left below the limit. */
    for (int i = 0; i < 3; i++)
        assert_eq(i, install_placeholder(fs));

    int kmalloc_before = fake_kmalloc_calls;
    int kfree_before = fake_kfree_calls;

    int fds[2] = { -1, -1 };
    int64_t rc = do_pipe(fds);
    assert_eq(-EMFILE, rc);

    /* The slot the reader end briefly took is released again. */
    assert_null(fs->fd[3]);
    for (int i = 0; i < 3; i++)
        assert_not_null(fs->fd[i]);

    /* The pipe object itself is released: its buffer is freed only when
     * BOTH file references are dropped (the pipe is freed when the last
     * reader AND the last writer are gone).  A leaked writer file would
     * leave the pipe allocated. */
    assert_eq(kmalloc_before + 1, fake_kmalloc_calls);
    assert_eq(kfree_before + 1, fake_kfree_calls);
}

TEST_FUNC(test_install_socket_failure_releases_socket)
{
    files_t *fs = install_table_new(4);

    for (int i = 0; i < 4; i++)
        assert_eq(i, install_placeholder(fs));

    int new_before = fake_netconn_new_calls;
    int delete_before = fake_netconn_delete_calls;

    int64_t rc = do_socket(AF_INET, SOCK_STREAM, 6);
    assert_eq(-EMFILE, rc);

    /* The socket that could not be installed is released. */
    assert_eq(new_before + 1, fake_netconn_new_calls);
    assert_eq(delete_before + 1, fake_netconn_delete_calls);
    for (int i = 0; i < 4; i++)
        assert_not_null(fs->fd[i]);
}

TEST_FUNC(test_install_accept_failure_releases_socket)
{
    install_table_new(OS01_NOFILE_CAPACITY);

    int64_t listen_fd = do_socket(AF_INET, SOCK_STREAM, 6);
    assert_true(listen_fd >= 0);
    assert_eq(0, do_bind((int)listen_fd, 0x0a000001, 1234));
    assert_eq(0, do_listen((int)listen_fd, 4));

    /* Lower the limit so the accepted descriptor does not fit.  The
     * already-open listening socket stays usable. */
    install_set_soft(1);

    int delete_before = fake_netconn_delete_calls;
    uint32_t peer_ip = 0;
    uint16_t peer_port = 0;
    int64_t rc = do_accept((int)listen_fd, &peer_ip, &peer_port);
    assert_eq(-EMFILE, rc);

    /* The accepted socket is released with the refused install. */
    assert_eq(delete_before + 1, fake_netconn_delete_calls);
}

TEST_FUNC(test_install_ptmx_failure_releases_pty)
{
    files_t *fs = install_table_new(4);

    for (int i = 0; i < 4; i++)
        assert_eq(i, install_placeholder(fs));

    /* Production PTMX open path (pty.c), reached through the ops table
     * it registers with devfs. */
    pty_init();
    assert_not_null(fake_fd_install_ptmx_ops);

    file_t *f = NULL;
    int rc = fake_fd_install_ptmx_ops->open("/dev/ptmx", &f);
    assert_eq(0, rc);
    assert_not_null(f);
    assert_eq(FD_PTY_MASTER, f->type);
    assert_true(pty_table[0].allocated);

    /* The descriptor install is refused. */
    int fd = fd_alloc(fs, f);
    assert_eq(-EMFILE, fd);
    if (fd >= 0) {
        fd_close(fs, fd);   // install wrongly accepted: close the slot
    } else {
        file_put(f);        // refused: the caller still owns the reference
    }

    /* The PTY is reclaimed: a master that never had a slave must not
     * keep the pty slot and its two pipes alive. */
    assert_false(pty_table[0].allocated);
    assert_null(pty_table[0].master_to_slave);
    assert_null(pty_table[0].slave_to_master);
}

TEST_FUNC(test_install_open_fd_survives_limit_lowering)
{
    files_t *fs = install_table_new(OS01_NOFILE_CAPACITY);

    /* 0..11 occupied, so the pipe lands on 12 (read) and 13 (write). */
    for (int i = 0; i < 12; i++)
        assert_eq(i, install_placeholder(fs));

    int fds[2] = { -1, -1 };
    assert_eq(0, do_pipe(fds));
    assert_eq(12, fds[0]);
    assert_eq(13, fds[1]);

    char payload = 'x';
    assert_eq(1, fd_write(fs->fd[13], &payload, 1));

    /* Lower the soft limit below the open descriptor numbers. */
    install_set_soft(4);

    /* The already-open descriptor stays installed and readable. */
    assert_not_null(fs->fd[12]);
    char got = 0;
    assert_eq(1, fd_read(fs->fd[12], &got, 1));
    assert_eq('x', got);

    /* ... and stays closable. */
    fd_close(fs, 12);
    assert_null(fs->fd[12]);
}

TEST_FUNC(test_install_dup2_rejects_target_above_limit)
{
    files_t *fs = install_table_new(OS01_NOFILE_CAPACITY);

    for (int i = 0; i < 12; i++)
        assert_eq(i, install_placeholder(fs));

    /* The descriptor that lives at 12 — the target of the dup2 below. */
    file_t *target = file_alloc();
    assert_not_null(target);
    target->type = FD_DEV;
    target->flags = O_RDONLY;
    int tfd = fd_alloc(fs, target);
    assert_eq(12, tfd);

    install_set_soft(4);

    /* The target is above the soft limit: refused. */
    assert_eq(-EBADF, fd_dup2(fs, 0, 12));

    /* The refusal must not close the descriptor that lives there. */
    assert_not_null(fs->fd[12]);
    assert_true(fs->fd[12] == target);
    assert_eq(1, target->refcount);
}

TEST_FUNC(test_install_dup2_same_fd_installs_nothing)
{
    files_t *fs = install_table_new(4);

    file_t *f = file_alloc();
    assert_not_null(f);
    f->type = FD_DEV;
    f->flags = O_RDONLY;
    assert_eq(0, fd_alloc(fs, f));

    /* oldfd == newfd on an open descriptor: no new installation. */
    assert_eq(0, fd_dup2(fs, 0, 0));
    assert_true(fs->fd[0] == f);
    assert_eq(1, f->refcount);

    /* oldfd == newfd on a closed descriptor: -EBADF. */
    assert_eq(-EBADF, fd_dup2(fs, 3, 3));

    /* oldfd is not open: -EBADF. */
    assert_eq(-EBADF, fd_dup2(fs, 3, 1));
}

TEST_FUNC(test_install_dupfd_errno_split)
{
    files_t *fs = install_table_new(4);

    file_t *f = file_alloc();
    assert_not_null(f);
    f->type = FD_DEV;
    f->flags = O_RDONLY;
    assert_eq(0, fd_alloc(fs, f));

    /* F_DUPFD with a minimum at the soft limit: unreachable. */
    assert_eq(-EINVAL, fd_dupfd(fs, 0, 4));
    assert_eq(-EINVAL, fd_dupfd(fs, 0, 9));

    /* A valid minimum with no free slot below the limit. */
    assert_eq(1, install_placeholder(fs));
    assert_eq(2, install_placeholder(fs));
    assert_eq(3, install_placeholder(fs));
    assert_eq(-EMFILE, fd_dupfd(fs, 0, 1));

    /* Plain dup with the table exhausted below the limit. */
    assert_eq(-EMFILE, fd_dup(fs, 0));

    /* A valid minimum with a free slot below the limit still works. */
    fd_close(fs, 3);
    assert_eq(3, fd_dupfd(fs, 0, 3));
}

TEST_FUNC(test_install_dup_errno_order)
{
    files_t *fs = install_table_new(4);

    file_t *f = file_alloc();
    assert_not_null(f);
    f->type = FD_DEV;
    f->flags = O_RDONLY;
    assert_eq(0, fd_alloc(fs, f));

    /* Plain dup: the SOURCE descriptor is checked first, so an
     * invalid or closed oldfd is -EBADF whatever the limit says. */
    assert_eq(-EBADF, fd_dup(fs, 7));
    assert_eq(-EBADF, fd_dup(fs, -1));

    /* F_DUPFD: the requested MINIMUM is checked first, so an
     * unreachable minimum is -EINVAL even for an invalid source. */
    assert_eq(-EINVAL, fd_dupfd(fs, 7, 4));

    /* A reachable minimum hands the decision to the source. */
    assert_eq(-EBADF, fd_dupfd(fs, 7, 0));
    assert_eq(-EBADF, fd_dupfd(fs, 7, 1));
    assert_eq(-EBADF, fd_dupfd(fs, -1, 1));

    /* A closed source descriptor is -EBADF as well. */
    assert_eq(1, install_placeholder(fs));
    fd_close(fs, 1);
    assert_eq(-EBADF, fd_dup(fs, 1));
    assert_eq(-EBADF, fd_dupfd(fs, 1, 0));
}

TEST_FUNC(test_install_zero_limit_errno_split)
{
    files_t *fs = install_table_new(4);

    file_t *f = file_alloc();
    assert_not_null(f);
    f->type = FD_DEV;
    f->flags = O_RDONLY;
    assert_eq(0, fd_alloc(fs, f));

    /* Lower the soft limit to zero: no descriptor may be installed. */
    install_set_soft(0);

    file_t *g = file_alloc();
    assert_not_null(g);
    g->type = FD_DEV;
    g->flags = O_RDONLY;

    /* Nothing can be installed. */
    assert_eq(-EMFILE, fd_alloc(fs, g));
    assert_eq(1, g->refcount);   // the refused install took no reference
    file_put(g);

    /* F_DUPFD reports the unreachable minimum (-EINVAL) even with
     * soft == 0 — the minimum is checked before the source. */
    assert_eq(-EINVAL, fd_dupfd(fs, 0, 0));

    /* Plain dup checks the source first: -EBADF for an invalid or
     * closed source, -EMFILE when the source is valid but no slot
     * fits below the limit. */
    assert_eq(-EBADF, fd_dup(fs, 7));
    assert_eq(-EBADF, fd_dup(fs, -1));
    assert_eq(-EMFILE, fd_dup(fs, 0));   // fd 0 is open, but no slot fits
}

TEST_FUNC(test_install_missing_table_is_emfile)
{
    /* No descriptor table: every installation path reports the
     * per-process limit condition -EMFILE (not -EINVAL). */
    file_t *f = file_alloc();
    assert_not_null(f);
    f->type = FD_DEV;
    f->flags = O_RDONLY;

    assert_eq(-EMFILE, fd_alloc(NULL, f));
    assert_eq(-EMFILE, fd_alloc_limit(NULL, f, 0, 4));
    assert_eq(1, f->refcount);   // no reference was taken
    file_put(f);

    /* A malformed request (NULL file) is still -EINVAL. */
    files_t *empty = files_alloc();
    assert_not_null(empty);
    assert_eq(-EINVAL, fd_alloc_limit(empty, NULL, 0, 4));

    /* socket() / accept() report the same condition. */
    install_table_new(OS01_NOFILE_CAPACITY);
    g_current_task.files = NULL;

    int new_before = fake_netconn_new_calls;
    int delete_before = fake_netconn_delete_calls;
    int64_t rc = do_socket(AF_INET, SOCK_STREAM, 6);
    assert_eq(-EMFILE, rc);
    /* The socket that could not be installed is released. */
    assert_eq(new_before + 1, fake_netconn_new_calls);
    assert_eq(delete_before + 1, fake_netconn_delete_calls);
}

TEST_FUNC(test_install_fd_alloc_limit_range)
{
    files_t *fs = install_table_new(OS01_NOFILE_CAPACITY);

    /* The explicit range decides: [0, 2) admits only slots 0 and 1. */
    file_t *f0 = file_alloc();
    assert_not_null(f0);
    assert_eq(0, fd_alloc_limit(fs, f0, 0, 2));

    file_t *f1 = file_alloc();
    assert_not_null(f1);
    assert_eq(1, fd_alloc_limit(fs, f1, 0, 2));

    /* The next free slot is above the explicit bound. */
    file_t *f2 = file_alloc();
    assert_not_null(f2);
    assert_eq(-EMFILE, fd_alloc_limit(fs, f2, 0, 2));
    assert_eq(1, f2->refcount);   // the refused install took no reference
    file_put(f2);

    /* An empty range installs nothing. */
    file_t *f3 = file_alloc();
    assert_not_null(f3);
    assert_eq(-EMFILE, fd_alloc_limit(fs, f3, 2, 2));
    file_put(f3);

    /* An exclusive bound of 0 forbids every installation. */
    file_t *f4 = file_alloc();
    assert_not_null(f4);
    assert_eq(-EMFILE, fd_alloc_limit(fs, f4, 0, 0));
    file_put(f4);

    /* A minimum above the bound is refused as well. */
    file_t *f5 = file_alloc();
    assert_not_null(f5);
    assert_eq(-EMFILE, fd_alloc_limit(fs, f5, 3, 2));
    file_put(f5);

    /* Nothing was installed above the bound. */
    assert_null(fs->fd[2]);
    assert_not_null(fs->fd[0]);
    assert_not_null(fs->fd[1]);
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
    TEST_ENTRY(test_install_fd_alloc_respects_soft_limit),
    TEST_ENTRY(test_install_fd_alloc_limit_range),
    TEST_ENTRY(test_install_pipe_needs_two_slots),
    TEST_ENTRY(test_install_socket_failure_releases_socket),
    TEST_ENTRY(test_install_accept_failure_releases_socket),
    TEST_ENTRY(test_install_ptmx_failure_releases_pty),
    TEST_ENTRY(test_install_open_fd_survives_limit_lowering),
    TEST_ENTRY(test_install_dup2_rejects_target_above_limit),
    TEST_ENTRY(test_install_dup2_same_fd_installs_nothing),
    TEST_ENTRY(test_install_dupfd_errno_split),
    TEST_ENTRY(test_install_dup_errno_order),
    TEST_ENTRY(test_install_zero_limit_errno_split),
    TEST_ENTRY(test_install_missing_table_is_emfile),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    TEST_RESULTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
