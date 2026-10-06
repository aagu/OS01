/*
 * test_pty_winsize.c — PTY master window resize notifications and SIGWINCH
 *
 * QEMU Resolution Switcher Task 6 (Spec §6.3):
 *   - Common pty_ioctl for TIOCGWINSZ, TIOCSWINSZ, TIOCGPGRP, TIOCSPGRP
 *   - PTY master handles TIOCSWINSZ / TIOCGWINSZ / TIOCSPGRP / TIOCGPGRP
 *   - Four-field winsize (ws_row, ws_col, ws_xpixel, ws_ypixel) atomic snapshot
 *   - SIGWINCH sent only when at least one field changes and pgrp > 0
 *   - signal_pgrp called strictly outside pty->state_lock
 *   - User memory fault safety (-EFAULT on invalid user pointers)
 *   - Regression: PTY master preserves TCGETS and rejects unknown ioctls with -ENOTTY
 *   - Concurrent resize reads consistent four-field states without tearing
 */

#include "test_framework.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <fs/file.h>
#include <fs/devfs.h>
#include <tty/pty.h>
#include <sys/ioctl.h>

/* Host pthread declarations without relying on host/libc header mix */
typedef unsigned long host_pthread_t;
extern int pthread_create(host_pthread_t *thread, const void *attr, void *(*start_routine)(void *), void *arg);
extern int pthread_join(host_pthread_t thread, void **retval);

/* ── Task + files plumbing (declared in gfx_test_runtime.h) ── */
static task_t   test_task;
task_t *gfx_test_current = &test_task;
#define current gfx_test_current

uint64_t volatile jiffies;

/* ── Global mock state ── */
static pty_t *g_active_pty = NULL;
static int g_signal_call_count = 0;
static pid_t g_signal_last_target = 0;
static int g_signal_last_sig = 0;
static int g_signal_state_lock_held_at_call = -1;

static bool g_user_range_valid = true;
static bool g_user_writable_ok = true;
static bool g_inject_copy_from_fault = false;
static bool g_inject_copy_to_fault = false;

/* ── signal_pgrp host mock ── */
int signal_pgrp(pid_t target, int sig)
{
    g_signal_call_count++;
    g_signal_last_target = target;
    g_signal_last_sig = sig;
    if (g_active_pty) {
        /* Under OS01 convention: 1 = unlocked, 0 = locked.
         * If lock was held during signal delivery, lock == 0.
         * We set g_signal_state_lock_held_at_call to 1 if held, 0 if unlocked. */
        g_signal_state_lock_held_at_call = (g_active_pty->state_lock.lock == 0L) ? 1 : 0;
    } else {
        g_signal_state_lock_held_at_call = -1;
    }
    return 0;
}

/* ── Uaccess stubs ── */
bool syscall_check_user_range(uint64_t addr, uint64_t len, bool writable)
{
    (void)len;
    if (addr == 0) return false;
    if (!g_user_range_valid) return false;
    if (writable && !g_user_writable_ok) return false;
    return true;
}

ssize_t copy_from_user_ft_res(void *dst, const void *src, size_t n,
                              void (*on_fault)(void *), void *arg)
{
    (void)on_fault; (void)arg;
    if (g_inject_copy_from_fault) return -EFAULT;
    memcpy(dst, src, n);
    return (ssize_t)n;
}

ssize_t copy_to_user_ft_res(void *dst, const void *src, size_t n,
                            void (*on_fault)(void *), void *arg)
{
    (void)on_fault; (void)arg;
    if (g_inject_copy_to_fault) return -EFAULT;
    memcpy(dst, src, n);
    return (ssize_t)n;
}

/* ── Minimal kernel stubs for file.c / pty.c link ── */
void *kmalloc(size_t size) { return malloc(size ? size : 1); }
size_t kfree(void *ptr) { free(ptr); return 0; }
struct vfs_node *vfs_node_get(struct vfs_node *n) { if (n) n->refcount++; return n; }
void vfs_node_put(struct vfs_node *n) { if (!n) return; n->refcount--; if (n->refcount == 0) free(n); }
int vfs_mount(const char *p, block_device_t *d, vfs_ops_t *o, void *f) { (void)p; (void)d; (void)o; (void)f; return -ENOSYS; }
struct vfs_node *vfs_lookup(const char *p) { (void)p; return NULL; }
struct vfs_node *vfs_lookup_from(const char *p, const char *c) { (void)p; (void)c; return NULL; }
int vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer) { return node->ops->read(node, offset, size, buffer); }
int strnlen_user(const void *p, size_t m) { (void)p; (void)m; return 0; }
int user_write_range_begin(uint64_t a, size_t l) { (void)a; (void)l; return 0; }
void user_write_range_end(void) {}
int user_read_range_begin(uint64_t a, size_t l) { (void)a; (void)l; return 0; }
void user_read_range_end(void) {}
void get_random_bytes(void *b, size_t n) { (void)b; (void)n; }
void random_add_entropy(const void *d, size_t n) { (void)d; (void)n; }
void *keyboard_get_tty(void) { return NULL; }
char read_serial(void) { return 0; }
void write_serial(char c) { (void)c; }
void write_serial_unlocked(unsigned char c) { (void)c; }
void schedule(void) {}
void task_wake(task_t *t) { (void)t; }
list_t *task_list_next(list_t *pos) { (void)pos; return NULL; }
int color_printk(unsigned int f, unsigned int b, const char *fmt, ...) { (void)f; (void)b; (void)fmt; return 0; }
int serial_printk(const char *fmt, ...) { (void)fmt; return 0; }
void log_debug(const char *fmt, ...) { (void)fmt; }
void frame_buffer_init(void) {}
void frame_buffer_early_init(void) {}
int devfs_register_chrdev(const char *n, void *p, const struct devfs_ops *o) { (void)n; (void)p; (void)o; return 0; }
int devfs_ioctl_file(file_t *f, int cmd, void *arg) { (void)f; (void)cmd; (void)arg; return -ENOTTY; }
void wait_queue_init(wait_queue_t *wq) { (void)wq; }
void wait_queue_sleep(wait_queue_t *wq) { (void)wq; }
void wait_queue_wake_one(wait_queue_t *wq) { (void)wq; }
void wait_queue_wake_all(wait_queue_t *wq) { (void)wq; }

/* ── Test Helpers ── */
static void reset_test_state(void)
{
    g_signal_call_count = 0;
    g_signal_last_target = 0;
    g_signal_last_sig = 0;
    g_signal_state_lock_held_at_call = -1;
    g_user_range_valid = true;
    g_user_writable_ok = true;
    g_inject_copy_from_fault = false;
    g_inject_copy_to_fault = false;
}

static void init_test_files(pty_t *pty, file_t *fm, file_t *fs)
{
    memset(fm, 0, sizeof(*fm));
    fm->type = FD_PTY_MASTER;
    fm->pty = pty;
    fm->flags = O_RDWR;
    fm->refcount = 1;

    memset(fs, 0, sizeof(*fs));
    fs->type = FD_PTY_SLAVE;
    fs->pty = pty;
    fs->flags = O_RDWR;
    fs->refcount = 1;

    g_active_pty = pty;
}

/* ═══════════════════════════════════════════════════════════════
 *  TEST 1: test_master_slave_winsize
 *  Master writes (30, 100, 800, 600), slave reads all 4 fields identical,
 *  master reads all 4 fields identical, and signal_pgrp is called once.
 * ═══════════════════════════════════════════════════════════════ */
TEST_FUNC(test_master_slave_winsize)
{
    reset_test_state();
    pty_t *pty = pty_alloc();
    assert_true(pty != NULL);

    file_t fm, fs;
    init_test_files(pty, &fm, &fs);

    /* Set foreground process group to 100 via master */
    pid_t pgrp = 100;
    int rc = fd_ioctl(&fm, TIOCSPGRP, &pgrp);
    assert_eq(0, rc);

    /* Master sets winsize to (30, 100, 800, 600) */
    struct winsize ws_in = {
        .ws_row = 30,
        .ws_col = 100,
        .ws_xpixel = 800,
        .ws_ypixel = 600,
    };
    rc = fd_ioctl(&fm, TIOCSWINSZ, &ws_in);
    assert_eq(0, rc);

    /* Slave reads 4 fields — must match */
    struct winsize ws_out_slave = {0};
    rc = fd_ioctl(&fs, TIOCGWINSZ, &ws_out_slave);
    assert_eq(0, rc);
    assert_eq(30, ws_out_slave.ws_row);
    assert_eq(100, ws_out_slave.ws_col);
    assert_eq(800, ws_out_slave.ws_xpixel);
    assert_eq(600, ws_out_slave.ws_ypixel);

    /* Master reads 4 fields — must match */
    struct winsize ws_out_master = {0};
    rc = fd_ioctl(&fm, TIOCGWINSZ, &ws_out_master);
    assert_eq(0, rc);
    assert_eq(30, ws_out_master.ws_row);
    assert_eq(100, ws_out_master.ws_col);
    assert_eq(800, ws_out_master.ws_xpixel);
    assert_eq(600, ws_out_master.ws_ypixel);

    /* Exactly 1 signal_pgrp call for pgrp=100 with SIGWINCH (28) */
    assert_eq(1, g_signal_call_count);
    assert_eq(100, g_signal_last_target);
    assert_eq(28, g_signal_last_sig);
    /* Crucial: lock must NOT be held during signal delivery */
    assert_eq(0, g_signal_state_lock_held_at_call);
}

/* ═══════════════════════════════════════════════════════════════
 *  TEST 2: test_winsize_no_signal_when_unchanged
 *  Setting identical dimensions sends 0 signals.
 * ═══════════════════════════════════════════════════════════════ */
TEST_FUNC(test_winsize_no_signal_when_unchanged)
{
    reset_test_state();
    pty_t *pty = pty_alloc();
    assert_true(pty != NULL);

    file_t fm, fs;
    init_test_files(pty, &fm, &fs);

    pid_t pgrp = 200;
    assert_eq(0, fd_ioctl(&fm, TIOCSPGRP, &pgrp));

    struct winsize ws = { .ws_row = 25, .ws_col = 80, .ws_xpixel = 640, .ws_ypixel = 480 };
    assert_eq(0, fd_ioctl(&fm, TIOCSWINSZ, &ws));
    assert_eq(1, g_signal_call_count);

    /* Reset counter, write exact same dimensions again */
    g_signal_call_count = 0;
    int rc = fd_ioctl(&fm, TIOCSWINSZ, &ws);
    assert_eq(0, rc);
    assert_eq(0, g_signal_call_count);

    /* Also try via slave — exact same dimensions, 0 signals */
    rc = fd_ioctl(&fs, TIOCSWINSZ, &ws);
    assert_eq(0, rc);
    assert_eq(0, g_signal_call_count);
}

/* ═══════════════════════════════════════════════════════════════
 *  TEST 3: test_winsize_signal_on_each_field_change
 *  Changing any ONE of the 4 fields triggers SIGWINCH exactly once.
 * ═══════════════════════════════════════════════════════════════ */
TEST_FUNC(test_winsize_signal_on_each_field_change)
{
    reset_test_state();
    pty_t *pty = pty_alloc();
    assert_true(pty != NULL);

    file_t fm, fs;
    init_test_files(pty, &fm, &fs);

    pid_t pgrp = 300;
    assert_eq(0, fd_ioctl(&fm, TIOCSPGRP, &pgrp));

    /* Base dimensions */
    struct winsize ws = { .ws_row = 25, .ws_col = 80, .ws_xpixel = 640, .ws_ypixel = 480 };
    assert_eq(0, fd_ioctl(&fm, TIOCSWINSZ, &ws));
    assert_eq(1, g_signal_call_count);

    /* 1. Change ws_row only */
    ws.ws_row = 26;
    assert_eq(0, fd_ioctl(&fm, TIOCSWINSZ, &ws));
    assert_eq(2, g_signal_call_count);
    assert_eq(300, g_signal_last_target);

    /* 2. Change ws_col only */
    ws.ws_col = 81;
    assert_eq(0, fd_ioctl(&fm, TIOCSWINSZ, &ws));
    assert_eq(3, g_signal_call_count);

    /* 3. Change ws_xpixel only */
    ws.ws_xpixel = 641;
    assert_eq(0, fd_ioctl(&fm, TIOCSWINSZ, &ws));
    assert_eq(4, g_signal_call_count);

    /* 4. Change ws_ypixel only */
    ws.ws_ypixel = 481;
    assert_eq(0, fd_ioctl(&fm, TIOCSWINSZ, &ws));
    assert_eq(5, g_signal_call_count);
}

/* ═══════════════════════════════════════════════════════════════
 *  TEST 4: test_winsize_pgrp_zero_no_signal
 *  When pgrp == 0, dimensions are updated but no signal is sent.
 * ═══════════════════════════════════════════════════════════════ */
TEST_FUNC(test_winsize_pgrp_zero_no_signal)
{
    reset_test_state();
    pty_t *pty = pty_alloc();
    assert_true(pty != NULL);

    file_t fm, fs;
    init_test_files(pty, &fm, &fs);

    /* Explicitly set pgrp to 0 */
    pid_t pgrp = 0;
    assert_eq(0, fd_ioctl(&fm, TIOCSPGRP, &pgrp));
    g_signal_call_count = 0;

    struct winsize ws = { .ws_row = 40, .ws_col = 120, .ws_xpixel = 1024, .ws_ypixel = 768 };
    int rc = fd_ioctl(&fm, TIOCSWINSZ, &ws);
    assert_eq(0, rc);

    /* Dimensions must be updated */
    struct winsize ws_out = {0};
    assert_eq(0, fd_ioctl(&fs, TIOCGWINSZ, &ws_out));
    assert_eq(40, ws_out.ws_row);
    assert_eq(120, ws_out.ws_col);
    assert_eq(1024, ws_out.ws_xpixel);
    assert_eq(768, ws_out.ws_ypixel);

    /* But NO signal sent */
    assert_eq(0, g_signal_call_count);
}

/* ═══════════════════════════════════════════════════════════════
 *  TEST 5: test_winsize_user_fault
 *  User memory fault returns -EFAULT, does not modify state, and does not signal.
 * ═══════════════════════════════════════════════════════════════ */
TEST_FUNC(test_winsize_user_fault)
{
    reset_test_state();
    pty_t *pty = pty_alloc();
    assert_true(pty != NULL);

    file_t fm, fs;
    init_test_files(pty, &fm, &fs);

    pid_t pgrp = 400;
    assert_eq(0, fd_ioctl(&fm, TIOCSPGRP, &pgrp));
    g_signal_call_count = 0;

    /* Baseline: (25, 80, 0, 0) from pty_alloc */

    /* 1. NULL pointer on TIOCSWINSZ */
    int rc = fd_ioctl(&fm, TIOCSWINSZ, NULL);
    assert_eq(-EFAULT, rc);
    assert_eq(0, g_signal_call_count);

    /* 2. Range check failure on TIOCSWINSZ */
    struct winsize ws_bad = { .ws_row = 50, .ws_col = 100, .ws_xpixel = 800, .ws_ypixel = 600 };
    g_user_range_valid = false;
    rc = fd_ioctl(&fm, TIOCSWINSZ, &ws_bad);
    assert_eq(-EFAULT, rc);
    assert_eq(0, g_signal_call_count);
    g_user_range_valid = true;

    /* 3. copy_from_user fault on TIOCSWINSZ */
    g_inject_copy_from_fault = true;
    rc = fd_ioctl(&fm, TIOCSWINSZ, &ws_bad);
    assert_eq(-EFAULT, rc);
    assert_eq(0, g_signal_call_count);
    g_inject_copy_from_fault = false;

    /* Verify dimensions remain pristine default (25, 80, 0, 0) */
    struct winsize ws_check = {0};
    assert_eq(0, fd_ioctl(&fs, TIOCGWINSZ, &ws_check));
    assert_eq(25, ws_check.ws_row);
    assert_eq(80, ws_check.ws_col);
    assert_eq(0, ws_check.ws_xpixel);
    assert_eq(0, ws_check.ws_ypixel);

    /* 4. NULL pointer on TIOCGWINSZ */
    rc = fd_ioctl(&fm, TIOCGWINSZ, NULL);
    assert_eq(-EFAULT, rc);

    /* 5. Range check failure on TIOCGWINSZ (writable check) */
    g_user_writable_ok = false;
    rc = fd_ioctl(&fm, TIOCGWINSZ, &ws_check);
    assert_eq(-EFAULT, rc);
    g_user_writable_ok = true;

    /* 6. copy_to_user fault on TIOCGWINSZ */
    g_inject_copy_to_fault = true;
    rc = fd_ioctl(&fm, TIOCGWINSZ, &ws_check);
    assert_eq(-EFAULT, rc);
    g_inject_copy_to_fault = false;
}

/* ═══════════════════════════════════════════════════════════════
 *  TEST 6: test_master_pgrp_support
 *  TIOCSPGRP and TIOCGPGRP on master and slave, with fault checking.
 * ═══════════════════════════════════════════════════════════════ */
TEST_FUNC(test_master_pgrp_support)
{
    reset_test_state();
    pty_t *pty = pty_alloc();
    assert_true(pty != NULL);

    file_t fm, fs;
    init_test_files(pty, &fm, &fs);

    /* Initial pgrp is 0 */
    pid_t pg_out = -1;
    int rc = fd_ioctl(&fm, TIOCGPGRP, &pg_out);
    assert_eq(0, rc);
    assert_eq(0, pg_out);

    /* Master sets pgrp to 555 */
    pid_t pg_in = 555;
    rc = fd_ioctl(&fm, TIOCSPGRP, &pg_in);
    assert_eq(0, rc);

    /* Read back via master and slave */
    pg_out = 0;
    assert_eq(0, fd_ioctl(&fm, TIOCGPGRP, &pg_out));
    assert_eq(555, pg_out);

    pg_out = 0;
    assert_eq(0, fd_ioctl(&fs, TIOCGPGRP, &pg_out));
    assert_eq(555, pg_out);

    /* Slave sets pgrp to 777 */
    pg_in = 777;
    rc = fd_ioctl(&fs, TIOCSPGRP, &pg_in);
    assert_eq(0, rc);

    /* Read back via master */
    pg_out = 0;
    assert_eq(0, fd_ioctl(&fm, TIOCGPGRP, &pg_out));
    assert_eq(777, pg_out);

    /* Fault tests on master TIOCSPGRP */
    assert_eq(-EFAULT, fd_ioctl(&fm, TIOCSPGRP, NULL));
    g_user_range_valid = false;
    assert_eq(-EFAULT, fd_ioctl(&fm, TIOCSPGRP, &pg_in));
    g_user_range_valid = true;
    g_inject_copy_from_fault = true;
    assert_eq(-EFAULT, fd_ioctl(&fm, TIOCSPGRP, &pg_in));
    g_inject_copy_from_fault = false;

    /* Fault tests on master TIOCGPGRP */
    assert_eq(-EFAULT, fd_ioctl(&fm, TIOCGPGRP, NULL));
    g_user_writable_ok = false;
    assert_eq(-EFAULT, fd_ioctl(&fm, TIOCGPGRP, &pg_out));
    g_user_writable_ok = true;
    g_inject_copy_to_fault = true;
    assert_eq(-EFAULT, fd_ioctl(&fm, TIOCGPGRP, &pg_out));
    g_inject_copy_to_fault = false;
}

/* ═══════════════════════════════════════════════════════════════
 *  TEST 7: test_master_tcgets_regression
 *  Master retains TCGETS and rejects unknown ioctls with -ENOTTY.
 * ═══════════════════════════════════════════════════════════════ */
TEST_FUNC(test_master_tcgets_regression)
{
    reset_test_state();
    pty_t *pty = pty_alloc();
    assert_true(pty != NULL);

    file_t fm, fs;
    init_test_files(pty, &fm, &fs);

    /* Master TCGETS */
    struct termios tio = {0};
    int rc = fd_ioctl(&fm, TCGETS, &tio);
    assert_eq(0, rc);
    assert_eq(ICRNL, tio.c_iflag);
    assert_eq((tcflag_t)(OPOST | ONLCR), tio.c_oflag);

    /* Unknown ioctl on master -> -ENOTTY */
    rc = fd_ioctl(&fm, 0x9876, &tio);
    assert_eq(-ENOTTY, rc);

    /* Unknown ioctl on slave -> -ENOTTY */
    rc = fd_ioctl(&fs, 0x9876, &tio);
    assert_eq(-ENOTTY, rc);
}

/* ═══════════════════════════════════════════════════════════════
 *  TEST 8: test_concurrent_resize_consistency
 *  Concurrent reader / writer threads running TIOCSWINSZ / TIOCGWINSZ
 *  never see a torn quad of (row, col, xpixel, ypixel).
 * ═══════════════════════════════════════════════════════════════ */
#define CONCURRENT_ITERS 20000

typedef struct {
    file_t *fm;
    file_t *fs;
    volatile bool stop;
    volatile int torn_count;
} resize_thread_arg_t;

static void *resize_writer_thread(void *arg)
{
    resize_thread_arg_t *ctx = (resize_thread_arg_t *)arg;
    struct winsize mode_a = { .ws_row = 25, .ws_col = 80, .ws_xpixel = 640, .ws_ypixel = 480 };
    struct winsize mode_b = { .ws_row = 50, .ws_col = 160, .ws_xpixel = 1280, .ws_ypixel = 960 };

    for (int i = 0; i < CONCURRENT_ITERS && !ctx->stop; i++) {
        struct winsize *ws = (i & 1) ? &mode_a : &mode_b;
        fd_ioctl(ctx->fm, TIOCSWINSZ, ws);
    }
    return NULL;
}

static void *resize_reader_thread(void *arg)
{
    resize_thread_arg_t *ctx = (resize_thread_arg_t *)arg;
    for (int i = 0; i < CONCURRENT_ITERS && !ctx->stop; i++) {
        struct winsize ws = {0};
        fd_ioctl(ctx->fs, TIOCGWINSZ, &ws);

        bool is_a = (ws.ws_row == 25 && ws.ws_col == 80 && ws.ws_xpixel == 640 && ws.ws_ypixel == 480);
        bool is_b = (ws.ws_row == 50 && ws.ws_col == 160 && ws.ws_xpixel == 1280 && ws.ws_ypixel == 960);
        if (!is_a && !is_b) {
            ctx->torn_count++;
            ctx->stop = true;
            break;
        }
    }
    return NULL;
}

TEST_FUNC(test_concurrent_resize_consistency)
{
    reset_test_state();
    pty_t *pty = pty_alloc();
    assert_true(pty != NULL);

    file_t fm, fs;
    init_test_files(pty, &fm, &fs);

    resize_thread_arg_t ctx = {
        .fm = &fm,
        .fs = &fs,
        .stop = false,
        .torn_count = 0,
    };

    host_pthread_t th_writer, th_reader;
    pthread_create(&th_writer, NULL, resize_writer_thread, &ctx);
    pthread_create(&th_reader, NULL, resize_reader_thread, &ctx);

    pthread_join(th_writer, NULL);
    ctx.stop = true;
    pthread_join(th_reader, NULL);

    assert_eq(0, ctx.torn_count);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_master_slave_winsize),
    TEST_ENTRY(test_winsize_no_signal_when_unchanged),
    TEST_ENTRY(test_winsize_signal_on_each_field_change),
    TEST_ENTRY(test_winsize_pgrp_zero_no_signal),
    TEST_ENTRY(test_winsize_user_fault),
    TEST_ENTRY(test_master_pgrp_support),
    TEST_ENTRY(test_master_tcgets_regression),
    TEST_ENTRY(test_concurrent_resize_consistency),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
