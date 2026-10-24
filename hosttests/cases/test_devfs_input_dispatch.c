/*
 * test_devfs_input_dispatch.c — file-aware devfs read/poll dispatch
 *
 * GUI-2 plan Task 1: devfs dispatches read/poll per DEVICE NODE, so two
 * open file descriptions on the same node are indistinguishable.  The
 * input lease (Task 3) needs the FILE identity to reach the device, so
 * struct devfs_ops grows two optional hooks (appended at the end of the
 * table):
 *
 *   int      (*read_file)(struct file *, uint64_t offset, uint64_t size,
 *                         void *buffer);
 *   uint32_t (*poll_file)(struct file *, uint32_t requested,
 *                         struct poll_table *pt);
 *
 * and devfs_poll_file() is the new file-aware poll entry point that
 * fd_poll(FD_DEV) uses.  Devices that register neither hook keep the
 * legacy node read / node poll behaviour.
 *
 * Build (the same real file/devfs compile isolation the gfx file
 * lifecycle test uses, extended with the poll entry point):
 *   - the REAL kernel/fs/devfs.c + kernel/fs/file.c + kernel/fs/poll.c
 *     are host-compiled through POLL_HOST_CFLAGS (poll_test_runtime.h
 *     shadows the heavyweight kernel headers) and linked against the
 *     narrow mocks of the layers below that this TU provides;
 *   - the assertions therefore observe the PRODUCTION dispatch
 *     (devfs_register_chrdev → devfs read/poll bridge → fd_read /
 *     fd_poll), never a stub's return value.
 *
 * Tested production symbols: devfs_register_chrdev, devfs_get_private,
 * devfs_poll_file, fd_read, fd_poll.
 */

#include <test_framework.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <fs/devfs.h>
#include <fs/file.h>
#include <fs/poll.h>
#include <fs/vfs.h>
#include <block/blockdev.h>
#include <lwip/api.h>
#include <lwip/err.h>

/* ── Task + files plumbing (declared in poll_test_runtime.h) ── */

static task_t   test_task;
static files_t  test_files;
task_t *poll_test_current = &test_task;

/* jiffies is referenced by the inlined clocksource_read_ns() in the
 * production poll object. */
uint64_t volatile jiffies;

/* ── Narrow mocks of the layers below devfs / file / poll ── */

/* Slab stubs — host malloc/free. */
void *kmalloc(size_t size) { return malloc(size ? size : 1); }
size_t kfree(void *ptr) { free(ptr); return 0; }

/* wait_queue stubs. */
void wait_queue_init(wait_queue_t *wq)
{
    list_init(&wq->head);
    spin_init(&wq->lock);
}
void wait_queue_sleep(wait_queue_t *wq) { (void)wq; }
void wait_queue_wake_one(wait_queue_t *wq) { (void)wq; }
void wait_queue_wake_all(wait_queue_t *wq) { (void)wq; }

/* Sched stubs. */
void task_wake(task_t *task) { (void)task; }
void schedule(void) {}

/* The generic VFS node read.  The devfs bridge must NOT reach it: the
 * legacy fallback goes through devfs's own node read, so this counter
 * stays 0 in every case below. */
static int vfs_read_calls;
int vfs_read(struct vfs_node *node, uint64_t offset, uint64_t size,
             void *buffer)
{
    (void)node; (void)offset; (void)size; (void)buffer;
    vfs_read_calls++;
    return -EIO;
}

/* devfs_init() mounts /dev through this stub.  The ops pointer it
 * receives IS the devfs mount's vfs_file_ops — the table that carries the
 * read_file bridge the production fd_read path dispatches through. */
static const vfs_file_ops_t *devfs_mount_fops;
static vfs_fs_type_t *s_registered_devfs_type;
int vfs_register_filesystem(vfs_fs_type_t *fs_type)
{
    s_registered_devfs_type = fs_type;
    return 0;
}
int vfs_mount_fs(const char *type_name, const char *path,
                 block_device_t *dev, void *data)
{
    (void)type_name;
    vfs_node_t dummy_root;
    vfs_mount_t dummy_mp;
    memset(&dummy_root, 0, sizeof(dummy_root));
    memset(&dummy_mp, 0, sizeof(dummy_mp));
    dummy_mp.path = path;
    dummy_mp.root = &dummy_root;
    if (s_registered_devfs_type && s_registered_devfs_type->mount) {
        int rc = s_registered_devfs_type->mount(&dummy_mp, dev, data);
        if (rc < 0) return rc;
        devfs_mount_fops = dummy_mp.f_ops;
    }
    return 0;
}
int vfs_mount(const char *path, block_device_t *dev,
              const vfs_inode_ops_t *i_ops, const vfs_file_ops_t *f_ops,
              void *fs_data)
{
    (void)path; (void)dev; (void)i_ops; (void)fs_data;
    devfs_mount_fops = f_ops;
    return 0;
}

/* devfs_init() registers the built-in null/zero/random/serial devices.
 * Their handlers must link even though this test never touches them. */
void get_random_bytes(void *b, size_t n) { (void)b; (void)n; }
void random_add_entropy(const void *d, size_t n) { (void)d; (void)n; }
int  user_write_range_begin(uint64_t a, size_t l) { (void)a; (void)l; return 0; }
void user_write_range_end(void) {}
char read_serial(void) { return 0; }
void write_serial(char c) { (void)c; }

/* Logging stubs (devfs_init()/devfs_register_chrdev log through
 * log_debug). */
int g_log_level = 0;
void _log_write(int level, const char *fmt, ...) { (void)level; (void)fmt; }

/* lwIP stubs — referenced by file.c's FD_SOCKET branch; never reached. */
err_t netconn_delete(struct netconn *c) { (void)c; return ERR_OK; }
err_t netconn_recv(struct netconn *c, struct netbuf **b)
{ (void)c; (void)b; return ERR_CLSD; }
err_t netconn_write(struct netconn *c, const void *d, size_t s, u8_t f)
{ (void)c; (void)d; (void)s; (void)f; return ERR_OK; }
err_t netconn_write_partly(struct netconn *c, const void *d, size_t s,
                           u8_t f, size_t *w)
{ (void)c; (void)d; (void)s; (void)f; if (w) *w = s; return ERR_OK; }
void netbuf_data(struct netbuf *b, void **d, u16_t *l)
{ (void)b; if (d) *d = NULL; if (l) *l = 0; }
int netbuf_next(struct netbuf *b) { (void)b; return -1; }
void netbuf_delete(struct netbuf *b) { (void)b; }

/* Block-device stubs — devfs_read()/devfs_write() keep their block
 * path; this test only exercises character devices, so the block
 * devices are never reached. */
int block_device_read(block_device_t *dev, uint64_t lba, uint32_t count,
                      void *buffer)
{ (void)dev; (void)lba; (void)count; (void)buffer; return -EIO; }
int block_device_write(block_device_t *dev, uint64_t lba, uint32_t count,
                       const void *buffer)
{ (void)dev; (void)lba; (void)count; (void)buffer; return -EIO; }

/* ── Mock device A: file-aware hooks ────────────────────────── */

#define MOCK_MAX_CALLS 8

static struct {
    int         read_calls;
    struct file *read_file_seen[MOCK_MAX_CALLS];
    void       *read_buffer_seen[MOCK_MAX_CALLS];
    uint64_t    read_offset_seen[MOCK_MAX_CALLS];
    uint32_t    read_size_seen[MOCK_MAX_CALLS];
    int         poll_calls;
    struct file *poll_file_seen[MOCK_MAX_CALLS];
    uint32_t    poll_requested_seen[MOCK_MAX_CALLS];
} mock_aware;

static int mock_aware_read_file(struct file *f, uint64_t offset,
                                uint64_t size, void *buffer)
{
    int n = mock_aware.read_calls++;
    if (n < MOCK_MAX_CALLS) {
        mock_aware.read_file_seen[n]    = f;
        mock_aware.read_buffer_seen[n]  = buffer;
        mock_aware.read_offset_seen[n]  = offset;
        mock_aware.read_size_seen[n]    = (uint32_t)size;
    }
    if (buffer && size) memset(buffer, 0xA5, size);
    return (int)size;
}

static uint32_t mock_aware_poll_file(struct file *f, uint32_t requested,
                                     struct poll_table *pt)
{
    (void)pt;
    int n = mock_aware.poll_calls++;
    if (n < MOCK_MAX_CALLS) {
        mock_aware.poll_file_seen[n]        = f;
        mock_aware.poll_requested_seen[n]   = requested;
    }
    return POLLIN | POLLRDNORM;
}

/* ── Mock device B: legacy node-only callbacks ─────────────── */

static struct {
    int      read_calls;
    int      poll_calls;
    uint32_t poll_requested_seen;
} mock_legacy;

static int mock_legacy_read(struct vfs_node *node, uint64_t offset,
                            uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    mock_legacy.read_calls++;
    if (buffer && size) memset(buffer, 0x5A, size);
    return (int)size;
}

static uint32_t mock_legacy_poll(void *priv, uint32_t requested,
                                  struct poll_table *pt)
{
    (void)priv; (void)pt;
    mock_legacy.poll_calls++;
    mock_legacy.poll_requested_seen = requested;
    return POLLOUT | POLLWRNORM;
}

/* ── Mock device C: file-aware read that fails ─────────────── */

static struct {
    int file_read_calls;
    int legacy_read_calls;
} mock_error;

static int mock_error_read_file(struct file *f, uint64_t offset,
                                uint64_t size, void *buffer)
{
    (void)f; (void)offset; (void)size; (void)buffer;
    mock_error.file_read_calls++;
    return -EACCES;
}

static int mock_error_legacy_read(struct vfs_node *node, uint64_t offset,
                                  uint64_t size, void *buffer)
{
    (void)node; (void)offset; (void)size; (void)buffer;
    mock_error.legacy_read_calls++;
    return 0;
}

/* ── Mock device D: file-aware poll that reports "not ready" ── */

static struct {
    int file_poll_calls;
    int legacy_poll_calls;
} mock_zero;

static uint32_t mock_zero_poll_file(struct file *f, uint32_t requested,
                                    struct poll_table *pt)
{
    (void)f; (void)requested; (void)pt;
    mock_zero.file_poll_calls++;
    return 0;
}

static uint32_t mock_zero_legacy_poll(void *priv, uint32_t requested,
                                      struct poll_table *pt)
{
    (void)priv; (void)requested; (void)pt;
    mock_zero.legacy_poll_calls++;
    return POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM;
}

/* ── Fixture helpers ────────────────────────────────────────── */

static void reset_mocks(void)
{
    memset(&test_task, 0, sizeof(test_task));
    memset(&test_files, 0, sizeof(test_files));
    test_task.addr_limit = UINT64_MAX;
    test_task.files = &test_files;
    list_init(&test_task.io_wait_node);
    memset(&mock_aware, 0, sizeof(mock_aware));
    memset(&mock_legacy, 0, sizeof(mock_legacy));
    memset(&mock_error, 0, sizeof(mock_error));
    memset(&mock_zero, 0, sizeof(mock_zero));
    vfs_read_calls = 0;
}

/* Register a mock device and return its devfs table index.  The index
 * is discovered through devfs_get_private() so the test never has to
 * mirror the production device_count (devfs_init() registers five
 * built-in devices before the test's own). */
static int register_mock_device(const char *name, void *priv,
                                const struct devfs_ops *ops)
{
    if (devfs_register_chrdev(name, priv, ops) < 0) return -1;
    for (int i = 0; i < DEVFS_MAX_DEVICES; i++) {
        vfs_node_t probe;
        memset(&probe, 0, sizeof(probe));
        probe.type = VFS_CHRDEV;
        probe.fs_data = (void *)(uintptr_t)i;
        if (devfs_get_private(&probe) == priv) return i;
    }
    return -1;
}

static vfs_node_t mock_node(int idx)
{
    vfs_node_t n;
    memset(&n, 0, sizeof(n));
    n.type = VFS_CHRDEV;
    n.fs_data = (void *)(uintptr_t)idx;
    n.f_ops = devfs_mount_fops;
    n.refcount = 1;
    return n;
}

static file_t mock_dev_file(vfs_node_t *node)
{
    file_t f;
    memset(&f, 0, sizeof(f));
    f.type = FD_DEV;
    f.flags = O_RDONLY;
    f.node = node;
    f.refcount = 1;
    return f;
}

/* ── Tests ──────────────────────────────────────────────────── */

TEST_FUNC(test_file_identity_reaches_device) {
    reset_mocks();
    static int marker;
    static const struct devfs_ops ops = {
        .read_file = mock_aware_read_file,
        .poll_file = mock_aware_poll_file,
    };
    int idx = register_mock_device("devfs_input_aware", &marker, &ops);
    assert_true(idx >= 0);

    vfs_node_t node = mock_node(idx);
    file_t f1 = mock_dev_file(&node);
    file_t f2 = mock_dev_file(&node);

    /* Two open file descriptions on the SAME node must reach the device
     * as two distinct identities. */
    uint8_t b1[4], b2[4];
    int64_t rc1 = fd_read(&f1, b1, sizeof(b1));
    int64_t rc2 = fd_read(&f2, b2, sizeof(b2));
    assert_eq(4, rc1);
    assert_eq(4, rc2);
    assert_eq(2, mock_aware.read_calls);
    assert_true(mock_aware.read_file_seen[0] == &f1);
    assert_true(mock_aware.read_file_seen[1] == &f2);
    assert_true(mock_aware.read_file_seen[0] != mock_aware.read_file_seen[1]);
    assert_eq(0, mock_aware.read_offset_seen[0]);
    assert_eq(4, mock_aware.read_size_seen[0]);

    uint32_t m1 = fd_poll(&f1, POLLIN, NULL);
    uint32_t m2 = fd_poll(&f2, POLLIN, NULL);
    assert_eq(POLLIN | POLLRDNORM, m1);
    assert_eq(POLLIN | POLLRDNORM, m2);
    assert_eq(2, mock_aware.poll_calls);
    assert_true(mock_aware.poll_file_seen[0] == &f1);
    assert_true(mock_aware.poll_file_seen[1] == &f2);
    assert_true(mock_aware.poll_file_seen[0] != mock_aware.poll_file_seen[1]);
    assert_eq(POLLIN, mock_aware.poll_requested_seen[0]);

    /* The generic VFS node read is never used by the devfs path. */
    assert_eq(0, vfs_read_calls);
}

TEST_FUNC(test_legacy_fallback) {
    reset_mocks();
    static int marker;
    static const struct devfs_ops ops = {
        .read = mock_legacy_read,
        .poll = mock_legacy_poll,
    };
    int idx = register_mock_device("devfs_input_legacy", &marker, &ops);
    assert_true(idx >= 0);

    vfs_node_t node = mock_node(idx);
    file_t f = mock_dev_file(&node);

    uint8_t buf[4];
    int64_t rc = fd_read(&f, buf, sizeof(buf));
    assert_eq(4, rc);
    assert_eq(1, mock_legacy.read_calls);
    assert_eq(0x5A, buf[0]);
    assert_eq(0, vfs_read_calls);

    uint32_t mask = fd_poll(&f, POLLOUT, NULL);
    assert_eq(POLLOUT | POLLWRNORM, mask);
    assert_eq(1, mock_legacy.poll_calls);
    assert_eq(POLLOUT, mock_legacy.poll_requested_seen);
}

TEST_FUNC(test_invalid_node) {
    reset_mocks();

    /* An out-of-range device index is not a valid devfs node. */
    vfs_node_t bad = mock_node(DEVFS_MAX_DEVICES + 5);
    file_t f_bad = mock_dev_file(&bad);
    assert_eq(POLLNVAL, fd_poll(&f_bad, POLLIN, NULL));

    /* A device file without a node. */
    file_t f_none = mock_dev_file(NULL);
    assert_eq(POLLNVAL, fd_poll(&f_none, POLLIN, NULL));

    /* A node that is not a devfs device. */
    vfs_node_t regular = mock_node(0);
    regular.type = VFS_FILE;
    file_t f_regular = mock_dev_file(&regular);
    assert_eq(POLLNVAL, fd_poll(&f_regular, POLLIN, NULL));

    /* devfs_poll_file() rejects a non-device file and a NULL file. */
    file_t f_vfs = mock_dev_file(&regular);
    f_vfs.type = FD_VFS;
    assert_eq(POLLNVAL, devfs_poll_file(&f_vfs, POLLIN, NULL));
    assert_eq(POLLNVAL, devfs_poll_file(NULL, POLLIN, NULL));
}

TEST_FUNC(test_file_read_uses_bounce) {
    reset_mocks();
    static int marker;
    static const struct devfs_ops ops = {
        .read_file = mock_aware_read_file,
    };
    int idx = register_mock_device("devfs_input_bounce", &marker, &ops);
    assert_true(idx >= 0);

    vfs_node_t node = mock_node(idx);
    file_t f = mock_dev_file(&node);

    uint8_t user_buf[8];
    memset(user_buf, 0, sizeof(user_buf));
    int64_t rc = fd_read(&f, user_buf, 4);
    assert_eq(4, rc);
    assert_eq(1, mock_aware.read_calls);

    /* The hook received the KERNEL bounce buffer, never the user
     * pointer — the device must not write into user memory. */
    assert_true(mock_aware.read_buffer_seen[0] != user_buf);

    /* fd_read still owns the user-space copy. */
    assert_eq(0xA5, user_buf[0]);
    assert_eq(0xA5, user_buf[3]);
    assert_eq(0x00, user_buf[4]);
}

TEST_FUNC(test_read_file_error_propagates) {
    reset_mocks();
    static int marker;
    static const struct devfs_ops ops = {
        .read_file = mock_error_read_file,
        .read      = mock_error_legacy_read,
    };
    int idx = register_mock_device("devfs_input_error", &marker, &ops);
    assert_true(idx >= 0);

    vfs_node_t node = mock_node(idx);
    file_t f = mock_dev_file(&node);

    uint8_t buf[4];
    int64_t rc = fd_read(&f, buf, sizeof(buf));
    assert_eq(-EACCES, rc);
    assert_eq(1, mock_error.file_read_calls);
    /* The device decided to refuse the read: the node read must NOT be
     * used as an unconditional fallback (that would bypass EACCES). */
    assert_eq(0, mock_error.legacy_read_calls);
}

TEST_FUNC(test_poll_file_zero_is_not_always_ready) {
    reset_mocks();
    static int marker;
    static const struct devfs_ops ops = {
        .poll_file = mock_zero_poll_file,
        .poll      = mock_zero_legacy_poll,
    };
    int idx = register_mock_device("devfs_input_zero", &marker, &ops);
    assert_true(idx >= 0);

    vfs_node_t node = mock_node(idx);
    file_t f = mock_dev_file(&node);

    uint32_t mask = fd_poll(&f, POLLIN, NULL);
    /* 0 is a legitimate answer ("not ready"), not "hook absent". */
    assert_eq(0, mask);
    assert_eq(1, mock_zero.file_poll_calls);
    assert_eq(0, mock_zero.legacy_poll_calls);
}

TEST_FUNC(test_invalid_path_never_touches_the_device) {
    reset_mocks();
    static int marker;
    static const struct devfs_ops ops = {
        .read_file = mock_aware_read_file,
        .poll_file = mock_aware_poll_file,
    };
    int idx = register_mock_device("devfs_input_invalid", &marker, &ops);
    assert_true(idx >= 0);

    /* Every invalid path is built on the mock device's OWN index, so a
     * missing check would reach the device: the counters below then go
     * up and the case fails. */
    vfs_node_t node = mock_node(idx);

    /* 1. A file that is not a device file: the bridge itself rejects it.
     *    (fd_poll(FD_VFS) is the plain-file path and never reaches the
     *    bridge, so the bridge entry point is called directly here.) */
    file_t f_vfs = mock_dev_file(&node);
    f_vfs.type = FD_VFS;
    assert_eq(POLLNVAL, devfs_poll_file(&f_vfs, POLLIN, NULL));
    assert_eq(0, mock_aware.poll_calls);

    /* 2. A node that is not a devfs device. */
    vfs_node_t regular = mock_node(idx);
    regular.type = VFS_FILE;
    file_t f_regular = mock_dev_file(&regular);
    assert_eq(POLLNVAL, fd_poll(&f_regular, POLLIN, NULL));
    assert_eq(0, mock_aware.poll_calls);

    /* 3. The read bridge rejects the same node BEFORE the device runs. */
    file_t f_read = mock_dev_file(&regular);
    uint8_t buf[4];
    memset(buf, 0, sizeof(buf));
    assert_eq(-1, fd_read(&f_read, buf, sizeof(buf)));
    assert_eq(0, mock_aware.read_calls);

    /* 4. A device file without a node. */
    file_t f_none = mock_dev_file(NULL);
    assert_eq(POLLNVAL, fd_poll(&f_none, POLLIN, NULL));
    assert_eq(0, mock_aware.poll_calls);

    /* 5. An out-of-range device index. */
    vfs_node_t bad = mock_node(DEVFS_MAX_DEVICES + 5);
    file_t f_bad = mock_dev_file(&bad);
    assert_eq(POLLNVAL, fd_poll(&f_bad, POLLIN, NULL));
    assert_eq(0, mock_aware.poll_calls);

    /* The generic VFS node read is never used either. */
    assert_eq(0, vfs_read_calls);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_file_identity_reaches_device),
    TEST_ENTRY(test_legacy_fallback),
    TEST_ENTRY(test_invalid_node),
    TEST_ENTRY(test_file_read_uses_bounce),
    TEST_ENTRY(test_read_file_error_propagates),
    TEST_ENTRY(test_poll_file_zero_is_not_always_ready),
    TEST_ENTRY(test_invalid_path_never_touches_the_device),
TEST_LIST_END

int main(void)
{
    /* Mount the real devfs so the test drives the production vfs_ops
     * table (the one that carries the read_file bridge). */
    devfs_init();
    assert_not_null(devfs_mount_fops);

    printf("=== Test Runner ===\n");
    for (int i = 0; i < __test_table_size; i++) {
        printf("\n--- %s ---\n", __test_table[i].name);
        __test_table[i].fn();
    }

    int failed = __test_stats.failed;
    TEST_RESULTS();
    return failed > 0 ? 1 : 0;
}
