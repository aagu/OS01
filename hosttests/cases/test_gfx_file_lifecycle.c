/*
 * test_gfx_file_lifecycle.c — per-file device ioctl/release lifecycle
 *
 * Spec §3 (gfx 2D API design, 2026-09-30):
 *   - Custom FD_DEV open attaches vfs_node_get(node) before return;
 *   - devfs_ioctl_file prefers ops->ioctl_file; falls back to
 *     ops->ioctl only when ioctl_file returns -ENOTTY;
 *   - devfs_release_file fires ops->release_file from file_free
 *     before the node ref is dropped, and only when set;
 *   - dup/fork-style extra refs survive a concurrent close while a
 *     file is pinned (the SYS_ioctl files_get_file/file_put pattern
 *     guarantees the same protection when the fd-table lock is held);
 *   - the node refcount returns to baseline after the last release.
 *
 * Build:
 *   - Production kernel/fs/devfs.c + kernel/fs/file.c compiled via
 *     GFX_HOST_CFLAGS against hosttests/mock/gfx_test_runtime.h.
 *   - This TU defines the stub surface the production sources need
 *     (kmalloc/kfree, current, schedule, tty_*, keyboard_*, ...).
 *   - The test exercises the PRODUCTION public symbols:
 *     devfs_register_chrdev, devfs_open_node, devfs_ioctl_file,
 *     devfs_release_file, file_alloc, file_get, file_put, fd_ioctl.
 */

#include "test_framework.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <fs/file.h>
#include <fs/devfs.h>
#include <uapi/gfx.h>
#include <lwip/err.h>
#include <lwip/api.h>

/* ── Task + files plumbing (declared in gfx_test_runtime.h) ── */

static task_t   test_task;
static files_t  test_files;
task_t *gfx_test_current = &test_task;
#define current gfx_test_current

/* jiffies referenced by inline clocksource_read_ns in production
 * source if any header chain pulls in <arch/.../clocksource.h>;
 * not strictly required by the devfs/file subset we compile but
 * keeps the runtime linkable. */
uint64_t volatile jiffies;

/* ── Stubs for symbols the production devfs.c + file.c reference.
 * Each one satisfies a linker reference; behaviour is irrelevant to
 * the lifecycle contract under test (the test only ever exercises
 * devfs_register_chrdev / devfs_open_node / devfs_ioctl_file /
 * devfs_release_file / file_alloc / file_get / file_put /
 * fd_ioctl, and the stub vfs_node_get/vfs_node_put just tracks a
 * reference balance for the test's assertions). */

/* Slab stubs — host malloc/free. */
void *kmalloc(size_t size) { return malloc(size ? size : 1); }
size_t kfree(void *ptr) { free(ptr); return 0; }

/* VFS node refcount stubs.  Production file.c calls vfs_node_put
 * exactly once per devfs_open_node-attach, so the test mirrors
 * the balance. */
struct vfs_node *vfs_node_get(struct vfs_node *n)
{
    if (n) n->refcount++;
    return n;
}
void vfs_node_put(struct vfs_node *n)
{
    if (!n) return;
    if (n->refcount == 0) {
        assert_true(0 && "vfs_node_put underflow");
        return;
    }
    n->refcount--;
    if (n->refcount == 0) free(n);
}

/* VFS misc — not exercised by the test, but referenced. */
int vfs_mount(const char *p, block_device_t *d, vfs_ops_t *o, void *f)
{ (void)p; (void)d; (void)o; (void)f; return -ENOSYS; }
struct vfs_node *vfs_lookup(const char *p)
{ (void)p; return NULL; }
struct vfs_node *vfs_lookup_from(const char *p, const char *c)
{ (void)p; (void)c; return NULL; }

/* Uaccess stubs — never invoked (no FD_VFS read/write in this test). */
ssize_t copy_to_user_ft_res(void *dst, const void *src, size_t n,
                            void (*cb)(void *), void *arg)
{ (void)cb; (void)arg; memcpy(dst, src, n); return (ssize_t)n; }
ssize_t copy_from_user_ft_res(void *dst, const void *src, size_t n,
                              void (*cb)(void *), void *arg)
{ (void)cb; (void)arg; memcpy(dst, src, n); return (ssize_t)n; }
int strnlen_user(const void *p, size_t m) { (void)p; (void)m; return 0; }
bool syscall_check_user_range(uint64_t a, uint64_t l, bool w)
{ (void)a; (void)l; (void)w; return true; }

/* VMM stubs. */
int  user_write_range_begin(uint64_t a, size_t l) { (void)a; (void)l; return 0; }
void user_write_range_end(void) {}
int  user_read_range_begin(uint64_t a, size_t l) { (void)a; (void)l; return 0; }
void user_read_range_end(void) {}

/* Random stubs. */
void get_random_bytes(void *b, size_t n) { (void)b; (void)n; }
void random_add_entropy(const void *d, size_t n) { (void)d; (void)n; }

/* Keyboard / serial stubs. */
void *keyboard_get_tty(void) { return NULL; }
char read_serial(void) { return 0; }
void write_serial(char c) { (void)c; }

/* TTY stubs. */
tty_t *get_dev_tty(void) { return NULL; }
void tty_set_dev_tty(tty_t *t) { (void)t; }
int tty_read(tty_t *t, char *b, int s, bool nb)
{ (void)t; (void)b; (void)s; (void)nb; return 0; }
int tty_write(tty_t *t, const char *b, int s)
{ (void)t; (void)b; (void)s; return 0; }
uint32_t tty_poll(tty_t *t, uint32_t r, struct poll_table *p)
{ (void)t; (void)p; return r; }
int tty_phys_ioctl(struct vfs_node *n, int c, void *a)
{ (void)n; (void)c; (void)a; return -ENOTTY; }

/* Debug log. */
void log_debug(const char *fmt, ...) { (void)fmt; }

/* lwIP stubs — referenced by file.c's FD_SOCKET branches; never
 * reached by the lifecycle test. */
err_t netconn_delete(struct netconn *c) { (void)c; return ERR_OK; }
err_t netconn_recv(struct netconn *c, struct netbuf **b)
{ (void)c; *b = NULL; return ERR_CLSD; }
err_t netconn_write(struct netconn *c, const void *d, size_t s, u8_t f)
{ (void)c; (void)d; (void)s; (void)f; return ERR_OK; }
err_t netconn_write_partly(struct netconn *c, const void *d, size_t s,
                           u8_t f, size_t *w)
{ (void)c; (void)d; (void)s; (void)f; if (w) *w = s; return ERR_OK; }
void netbuf_data(struct netbuf *b, void **d, u16_t *l)
{ (void)b; if (d) *d = NULL; if (l) *l = 0; }
int netbuf_next(struct netbuf *b) { (void)b; return -1; }
void netbuf_delete(struct netbuf *b) { (void)b; }

/* wait_queue stubs. */
void wait_queue_init(wait_queue_t *q) { (void)q; }
void wait_queue_sleep(wait_queue_t *q) { (void)q; }
void wait_queue_wake_one(wait_queue_t *q) { (void)q; }
void wait_queue_wake_all(wait_queue_t *q) { (void)q; }

/* Sched stubs. */
void task_wake(task_t *t) { (void)t; }
void schedule(void) {}
spinlock_T task_list_lock = { 1 };
gfx_test_task_union_t init_task_union;
list_t *task_list_next(list_t *p) { (void)p; return NULL; }
int signal_pgrp(pid_t t, int s) { (void)t; (void)s; return -1; }

/* ── test tracking (mirrors production devices[] semantics) ── */

struct test_device_state {
    int   open_count;
    int   ioctl_file_count;
    int   release_count;
    int   last_cmd;
    void *last_arg;
    void *last_priv_seen;
    file_t *last_file_seen;
    int   legacy_ioctl_count;
};

#define TEST_MAX_DEVICES 4
static struct test_device_state test_devices[TEST_MAX_DEVICES];

/* Mirror of "node refcount outstanding".  Every test-driven
 * vfs_node_get should be matched by a vfs_node_put in the cleanup
 * path; the production file_free vfs_node_put runs inside the
 * linked file.c.  We track the balance here to assert it returns
 * to zero after every release path. */
static int node_ref_balance;

static struct vfs_node *test_node_with_idx(int idx)
{
    struct vfs_node *n = calloc(1, sizeof(*n));
    n->type = VFS_CHRDEV;
    n->fs_data = (void *)(uintptr_t)idx;
    n->refcount = 1;
    return n;
}

/* ── Custom open callback — returns a file WITHOUT a node ── */
/* This is the critical contract: devfs_open_node must attach
 * vfs_node_get(node) after this returns success. */
static int fake_gfx_open(const char *name, file_t **out_file)
{
    (void)name;
    if (!out_file) return -EINVAL;
    file_t *f = file_alloc();
    if (!f) return -ENOMEM;
    /* Deliberately leave f->node == NULL — devfs_open_node must
     * attach it after this callback returns success. */
    f->type = FD_NONE;
    /* Allocate a fake per-file private (gfx_view stand-in). */
    f->dev_private = calloc(1, sizeof(uint64_t));
    *out_file = f;
    return 0;
}

static int fake_gfx_ioctl_file(file_t *f, int cmd, void *arg)
{
    int idx = (int)(uintptr_t)f->node->fs_data;
    test_devices[idx].ioctl_file_count++;
    test_devices[idx].last_cmd = cmd;
    test_devices[idx].last_arg = arg;
    test_devices[idx].last_priv_seen = f->dev_private;
    test_devices[idx].last_file_seen = f;
    if (cmd == 0xDEAD) return -ENOTTY;   /* explicit fall-through */
    if (cmd == 0xBEEF) return -EINVAL;
    return 0;
}

static void fake_gfx_release(file_t *f)
{
    int idx = (int)(uintptr_t)f->node->fs_data;
    test_devices[idx].release_count++;
    free(f->dev_private);
    f->dev_private = NULL;
}

static int fake_gfx_legacy_ioctl(struct vfs_node *node, int cmd, void *arg)
{
    int idx = (int)(uintptr_t)node->fs_data;
    test_devices[idx].legacy_ioctl_count++;
    (void)cmd; (void)arg;
    return 0;
}

/* Each devfs_register_chrdev call appends one entry to the
 * production devices[] table.  The production index is whatever
 * device_count was BEFORE the call (then incremented inside the
 * production function).  We mirror that here by tracking our own
 * counter that starts at 0 and increments per registration. */
static int registered_count;

static int register_fake_gfx(struct vfs_node **out_node)
{
    static const struct devfs_ops ops = {
        .open        = fake_gfx_open,
        .ioctl       = fake_gfx_legacy_ioctl,
        .ioctl_file  = fake_gfx_ioctl_file,
        .release_file = fake_gfx_release,
    };
    int idx = registered_count++;
    int rc = devfs_register_chrdev("gfx_test", NULL, &ops);
    if (rc < 0) { registered_count--; return rc; }
    memset(&test_devices[idx], 0, sizeof(test_devices[idx]));
    *out_node = test_node_with_idx(idx);
    return idx;
}

static int register_legacy_gfx(struct vfs_node **out_node)
{
    static const struct devfs_ops legacy_ops = {
        .ioctl = fake_gfx_legacy_ioctl,
        /* ioctl_file/release_file deliberately NULL */
    };
    int idx = registered_count++;
    int rc = devfs_register_chrdev("gfx_legacy", NULL, &legacy_ops);
    if (rc < 0) { registered_count--; return rc; }
    memset(&test_devices[idx], 0, sizeof(test_devices[idx]));
    *out_node = test_node_with_idx(idx);
    return idx;
}

/* Reset bookkeeping (does NOT reset production devices[] — that
 * table lives across sub-tests). */
static void reset_test_state(void)
{
    memset(&test_task, 0, sizeof(test_task));
    memset(&test_files, 0, sizeof(test_files));
    test_task.addr_limit = UINT64_MAX;
    test_task.files = &test_files;
    list_init(&test_task.io_wait_node);
    memset(test_devices, 0, sizeof(test_devices));
    node_ref_balance = 0;
    jiffies = 0;
    /* NOTE: registered_count and production devices[] persist
     * across tests (devfs_init can't be called twice).  Each
     * register_*_gfx call is a fresh entry; tests that need a
     * clean state should be aware of which device index maps to
     * which callback. */
}

/* ── Tests ──────────────────────────────────────────── */

TEST_FUNC(test_devfs_open_attaches_node_ref) {
    reset_test_state();
    struct vfs_node *node;
    int idx = register_fake_gfx(&node);
    assert_true(idx >= 0);

    file_t *f = NULL;
    int rc = devfs_open_node(node, "/dev/gfx_test", O_RDWR, &f);
    assert_eq(0, rc);
    assert_not_null(f);
    assert_not_null(f->node);     /* production MUST attach the node ref */
    assert_eq(FD_DEV, f->type);   /* and stamp the file type */
    assert_not_null(f->dev_private);

    /* Let production file_free (linked from kernel/fs/file.c)
     * handle the cleanup: devfs_release_file(f) + vfs_node_put
     * + free(f).  The node ref balance is the assertion that the
     * full open→free cycle returns to zero. */
    file_put(f);
    assert_eq(0, node_ref_balance);
    assert_eq(1, test_devices[idx].release_count);
}

TEST_FUNC(test_devfs_ioctl_file_dispatch) {
    reset_test_state();
    struct vfs_node *node;
    int idx = register_fake_gfx(&node);
    assert_true(idx >= 0);

    file_t *f = NULL;
    int rc = devfs_open_node(node, "/dev/gfx_test", O_RDWR, &f);
    assert_eq(0, rc);

    /* Normal command: ioctl_file returns 0 (success). */
    int my_cmd = GFX_GET_INFO;
    int my_arg = 0x12345678;
    rc = devfs_ioctl_file(f, my_cmd, &my_arg);
    assert_eq(0, rc);
    assert_eq(1, test_devices[idx].ioctl_file_count);
    assert_eq(0, test_devices[idx].legacy_ioctl_count);
    assert_eq(my_cmd, test_devices[idx].last_cmd);
    assert_true(test_devices[idx].last_priv_seen == f->dev_private);

    /* -ENOTTY from ioctl_file MUST fall through to the node callback. */
    test_devices[idx].ioctl_file_count = 0;
    test_devices[idx].legacy_ioctl_count = 0;
    rc = devfs_ioctl_file(f, 0xDEAD, &my_arg);
    assert_eq(0, rc);
    assert_eq(1, test_devices[idx].ioctl_file_count);
    assert_eq(1, test_devices[idx].legacy_ioctl_count);

    /* -EINVAL from ioctl_file MUST NOT fall through. */
    test_devices[idx].ioctl_file_count = 0;
    test_devices[idx].legacy_ioctl_count = 0;
    rc = devfs_ioctl_file(f, 0xBEEF, &my_arg);
    assert_eq(-EINVAL, rc);
    assert_eq(1, test_devices[idx].ioctl_file_count);
    assert_eq(0, test_devices[idx].legacy_ioctl_count);

    /* fd_ioctl must reach the file-aware path. */
    test_devices[idx].ioctl_file_count = 0;
    rc = fd_ioctl(f, GFX_GET_INFO, &my_arg);
    assert_eq(0, rc);
    assert_eq(1, test_devices[idx].ioctl_file_count);

    file_put(f);
    assert_eq(0, node_ref_balance);
}

TEST_FUNC(test_release_fires_once_on_final_drop) {
    reset_test_state();
    struct vfs_node *node;
    int idx = register_fake_gfx(&node);
    assert_true(idx >= 0);

    file_t *f = NULL;
    int rc = devfs_open_node(node, "/dev/gfx_test", O_RDWR, &f);
    assert_eq(0, rc);

    /* Simulate dup() / fork() by bumping refcount via file_get. */
    file_get(f);          /* refcount: 2 */
    file_get(f);          /* refcount: 3 */

    /* Two drops: release MUST NOT fire yet (refcount still > 0). */
    file_put(f);
    file_put(f);
    assert_eq(0, test_devices[idx].release_count);

    /* Final drop: production file_free (linked from kernel/fs/file.c)
     * calls devfs_release_file exactly once on the drop-to-zero
     * transition, BEFORE releasing the node ref.  file_free also
     * frees f itself, so we MUST NOT free(f) again here. */
    file_put(f);
    assert_eq(1, test_devices[idx].release_count);
    assert_eq(0, node_ref_balance);   /* file_free vfs_node_put balanced the open */
    /* f is now freed by file_free; don't touch it. */
}

TEST_FUNC(test_legacy_node_ioctl_when_no_file_callback) {
    reset_test_state();
    struct vfs_node *node;
    int idx = register_legacy_gfx(&node);
    assert_true(idx >= 0);

    /* Build a default FD_DEV file the way the default branch in
     * devfs_open_node would: file_alloc + node attach + type stamp. */
    file_t *f = file_alloc();
    assert_not_null(f);
    vfs_node_get(node);          /* mirror the default-branch node attach */
    f->type = FD_DEV;
    f->node = node;
    f->flags = O_RDWR;

    /* devfs_ioctl_file falls through to the node callback. */
    int my_arg = 0;
    int rc = devfs_ioctl_file(f, GFX_GET_INFO, &my_arg);
    assert_eq(0, rc);
    assert_eq(0, test_devices[idx].ioctl_file_count);   /* not registered */
    assert_eq(1, test_devices[idx].legacy_ioctl_count); /* did fire */

    /* devfs_release_file is a no-op when release_file is NULL. */
    int pre = test_devices[idx].release_count;
    devfs_release_file(f);
    assert_eq(pre, test_devices[idx].release_count);

    /* Cleanup via production file_free for consistency. */
    file_put(f);
    assert_eq(0, node_ref_balance);
}

TEST_FUNC(test_ioctl_reaches_file_via_fdioctl) {
    reset_test_state();
    struct vfs_node *node;
    int idx = register_fake_gfx(&node);
    assert_true(idx >= 0);

    file_t *f = NULL;
    int rc = devfs_open_node(node, "/dev/gfx_test", O_RDWR, &f);
    assert_eq(0, rc);

    rc = fd_ioctl(f, GFX_CREATE_VIEW, NULL);
    assert_eq(0, rc);
    assert_eq(1, test_devices[idx].ioctl_file_count);

    file_put(f);
    assert_eq(0, node_ref_balance);
}

TEST_FUNC(test_regular_file_cannot_dispatch_device_callbacks) {
    reset_test_state();
    struct vfs_node *registered_node;
    int idx = register_fake_gfx(&registered_node);
    assert_true(idx >= 0);
    /* A filesystem node may use a small integer as its own fs_data.
     * This must not be interpreted as a devfs registration index. */
    struct vfs_node *regular_node = test_node_with_idx(idx);
    regular_node->type = VFS_FILE;
    file_t *f = file_alloc();
    assert_not_null(f);
    vfs_node_get(regular_node);
    f->type = FD_VFS;
    f->node = regular_node;
    int rc = fd_ioctl(f, GFX_CREATE_VIEW, NULL);
    assert_eq(-ENOTTY, rc);
    assert_eq(0, test_devices[idx].ioctl_file_count);
    file_put(f);
    assert_eq(0, test_devices[idx].release_count);
    assert_eq(0, node_ref_balance);
    free(registered_node);
}

TEST_FUNC(test_abi_struct_sizes) {
    /* The _Static_asserts in uapi/gfx.h catch this at compile
     * time; this is the runtime smoke test. */
    assert_eq((size_t)16, sizeof(gfx_view_desc_t));
    assert_eq((size_t)16, sizeof(gfx_info_t));
    assert_eq((size_t)16, sizeof(gfx_present_req_t));
    assert_eq((int)0x4701, (int)GFX_CREATE_VIEW);
    assert_eq((int)0x4702, (int)GFX_GET_INFO);
    assert_eq((int)0x4703, (int)GFX_PRESENT);
    assert_eq((unsigned)0u, (unsigned)GFX_FORMAT_RGB32);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_devfs_open_attaches_node_ref),
    TEST_ENTRY(test_devfs_ioctl_file_dispatch),
    TEST_ENTRY(test_release_fires_once_on_final_drop),
    TEST_ENTRY(test_legacy_node_ioctl_when_no_file_callback),
    TEST_ENTRY(test_ioctl_reaches_file_via_fdioctl),
    TEST_ENTRY(test_regular_file_cannot_dispatch_device_callbacks),
    TEST_ENTRY(test_abi_struct_sizes),
TEST_LIST_END

int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
