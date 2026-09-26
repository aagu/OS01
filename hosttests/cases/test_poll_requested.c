/* Real production-path requested poll direction tests. */

#include <test_framework.h>
#include <errno.h>
#include <fcntl.h>
#include <fs/devfs.h>
#include <fs/file.h>
#include <fs/poll.h>
#include <tty/pty.h>
#include <fs/select.h>
#include <tty/tty.h>
#include <lwip/api.h>

static task_t test_task;
static files_t test_files;
task_t *poll_test_current = &test_task;

static size_t allocation_sizes[8];
static int allocation_count;
static int poll_wake_count;

void *kmalloc(size_t size)
{
    if (allocation_count < (int)(sizeof(allocation_sizes) /
                                 sizeof(allocation_sizes[0])))
        allocation_sizes[allocation_count] = size;
    allocation_count++;
    return malloc(size ? size : 1);
}

size_t kfree(void *ptr)
{
    free(ptr);
    return 0;
}

void wait_queue_init(wait_queue_t *wq)
{
    list_init(&wq->head);
    spin_init(&wq->lock);
}

void wait_queue_sleep(wait_queue_t *wq)
{
    (void)wq;
}

void wait_queue_wake_one(wait_queue_t *wq)
{
    (void)wq;
}

void wait_queue_wake_all(wait_queue_t *wq)
{
    (void)wq;
    poll_wake_count++;
}

void task_wake(task_t *task)
{
    (void)task;
}

void schedule(void) {}

uint64_t volatile jiffies;

// ── lwIP stubs (Task 2: required because fd_read's FD_SOCKET branch
// is now reachable via the linker after we call fd_read from the
// test).  These are no-ops because none of the fd_read tests
// exercise the socket path; they exist purely to satisfy the linker.
// ─────────────────────────────────────────────────────────────
err_t netconn_delete(struct netconn *conn)         { (void)conn; return ERR_OK; }
err_t netconn_recv(struct netconn *conn, struct netbuf **buf)
                                                   { (void)conn; (void)buf; return ERR_CLSD; }
err_t netconn_write(struct netconn *conn, const void *data, size_t size, u8_t flags)
                                                   { (void)conn; (void)data; (void)size; (void)flags; return ERR_OK; }
err_t netconn_write_partly(struct netconn *conn, const void *data, size_t size,
                           u8_t flags, size_t *bytes_written)
                                                   { (void)conn; (void)data; (void)size; (void)flags; if (bytes_written) *bytes_written = size; return ERR_OK; }
void netbuf_data(struct netbuf *buf, void **data, uint16_t *len)
                                                   { (void)buf; if (data) *data = NULL; if (len) *len = 0; }
int netbuf_next(struct netbuf *buf)                { (void)buf; return -1; }
void netbuf_delete(struct netbuf *buf)             { (void)buf; }

// syscall_check_user_range, copy_to_user_ft_res, copy_from_user_ft_res
// are already provided by poll_clocksource_stub.c (linked into the
// elf via POLL_PRODUCTION_OBJS).  Do NOT redefine them here.

// ── fd_read test fixtures (Task 2) ─────────────────────────
// file.c fd_read's FD_VFS/FD_DEV branch calls vfs_read() in a
// loop and copy_to_user_ft_res() per chunk.  Both are weak-linked
// from kernel/lib production objects; the test mocks them via
// the real test_platform.h dispatch (no libc wrappers).
//
// vfs_read_seq[i] is the i-th call's scripted response.  Each
// entry specifies both the return value (negative for errors,
// 0 for EOF, positive for bytes) and the byte count to write
// into the kernel bounce buffer (only meaningful when ret>0).
// The sequence terminates when vfs_read_seq_idx reaches
// vfs_read_seq_len — subsequent calls return -EIO so a runaway
// mock fails the test loudly instead of looping forever.
#define FD_READ_MAX_SEQ 8
struct fd_read_step {
    int64_t ret;
    size_t  written;
};
static struct fd_read_step vfs_read_seq[FD_READ_MAX_SEQ];
static int vfs_read_seq_len;
static int vfs_read_seq_idx;

int vfs_read(struct vfs_node *node, uint64_t offset,
             uint64_t size, void *buffer)
{
    (void)node; (void)offset;
    if (vfs_read_seq_idx >= vfs_read_seq_len) {
        // Runaway mock: never expect more steps than scripted.
        return -EIO;
    }
    struct fd_read_step s = vfs_read_seq[vfs_read_seq_idx++];
    if (s.ret > 0 && buffer && s.written > 0) {
        size_t copy = s.written < size ? s.written : size;
        memset(buffer, 0x42, copy);
    }
    return (int)s.ret;
}

// copy_to_user_ft_res is provided by poll_clocksource_stub.c (the
// simple memcpy stub is sufficient: the new fd_read tests don't
// need to inject faults).  poll_clocksource_stub.c's symbol is now
// weak so future tests can override with a tracking version.

static void reset_fd_read_mocks(void)
{
    memset(vfs_read_seq, 0, sizeof(vfs_read_seq));
    vfs_read_seq_len = 0;
    vfs_read_seq_idx = 0;
}

static void push_vfs_read(int64_t ret, size_t written)
{
    if (vfs_read_seq_len >= FD_READ_MAX_SEQ) return;
    vfs_read_seq[vfs_read_seq_len].ret = ret;
    vfs_read_seq[vfs_read_seq_len].written = written;
    vfs_read_seq_len++;
}

static void reset_runtime(void)
{
    memset(&test_task, 0, sizeof(test_task));
    memset(&test_files, 0, sizeof(test_files));
    memset(allocation_sizes, 0, sizeof(allocation_sizes));
    allocation_count = 0;
    poll_wake_count = 0;
    test_task.addr_limit = UINT64_MAX;
    test_task.files = &test_files;
    list_init(&test_task.io_wait_node);
}

static void init_pipe(pipe_t *pipe, bool full)
{
    memset(pipe, 0, sizeof(*pipe));
    pipe->head = full ? PIPE_SIZE - 1 : 0;
    pipe->tail = 0;
    pipe->readers = 1;
    pipe->writers = 1;
    spin_init(&pipe->lock);
    wait_queue_init(&pipe->read_wait);
    wait_queue_init(&pipe->write_wait);
    list_init(&pipe->read_poll);
    list_init(&pipe->write_poll);
}

static file_t make_blocked_pty_file(pty_t *pty,
                                    pipe_t *master_to_slave,
                                    pipe_t *slave_to_master)
{
    init_pipe(master_to_slave, true);
    init_pipe(slave_to_master, false);
    memset(pty, 0, sizeof(*pty));
    pty->allocated = true;
    pty->master_to_slave = master_to_slave;
    pty->slave_to_master = slave_to_master;

    file_t file = {
        .type = FD_PTY_MASTER,
        .refcount = 1,
        .flags = O_RDWR,
        .pty = pty,
    };
    return file;
}

TEST_FUNC(test_direction_policy)
{
    assert_true(poll_requested_read(POLLIN));
    assert_true(poll_requested_read(POLLRDNORM | POLLOUT));
    assert_false(poll_requested_read(POLLOUT));
    assert_true(poll_requested_write(POLLOUT));
    assert_false(poll_requested_write(POLLIN));
}

TEST_FUNC(test_requested_registration_policy)
{
    reset_runtime();

    file_t regular = {
        .type = FD_VFS,
        .flags = O_RDWR | O_CREAT | O_APPEND,
    };
    assert_eq(POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM,
              fd_poll(&regular, POLLIN | POLLOUT, NULL));

    pipe_t pipe;
    init_pipe(&pipe, false);
    file_t reader = {
        .type = FD_PIPE,
        .flags = O_RDONLY | O_NONBLOCK,
        .pipe = &pipe,
    };
    poll_table_t pt = {0};
    int rc = poll_table_setup(&pt, 2);
    assert_eq(0, rc);
    poll_table_init(&pt);
    uint32_t mask = fd_poll(&reader, POLLIN, &pt);
    assert_eq(0, mask);
    assert_eq(1, pt.nent);
    assert_false(list_is_empty(&pipe.read_poll));
    poll_table_cleanup(&pt);
    assert_eq(0, pt.nent);
    assert_true(list_is_empty(&pipe.read_poll));

    mask = fd_poll(&reader, POLLIN, NULL);
    assert_eq(0, mask);
    assert_true(list_is_empty(&pipe.read_poll));
    poll_table_destroy(&pt);
}

TEST_FUNC(test_real_pty_two_direction_registration_wake_and_cleanup)
{
    reset_runtime();
    pty_t pty;
    pipe_t master_to_slave;
    pipe_t slave_to_master;
    file_t master = make_blocked_pty_file(&pty, &master_to_slave,
                                          &slave_to_master);
    master.flags |= O_NONBLOCK;

    poll_table_t pt = {0};
    int rc = poll_table_setup(&pt, 2);
    assert_eq(0, rc);
    poll_table_init(&pt);
    uint32_t mask = fd_poll(&master, POLLIN | POLLOUT, &pt);
    assert_eq(0, mask);
    assert_eq(2, pt.nent);
    assert_false(list_is_empty(&slave_to_master.read_poll));
    assert_false(list_is_empty(&master_to_slave.write_poll));

    uint64_t flags = spin_lock_irqsave(&master_to_slave.lock);
    pipe_wake_writers(&master_to_slave);
    spin_unlock_irqrestore(&master_to_slave.lock, flags);
    assert_eq(1, poll_wake_count);
    assert_true(list_is_empty(&master_to_slave.write_poll));
    assert_false(list_is_empty(&slave_to_master.read_poll));

    flags = spin_lock_irqsave(&slave_to_master.lock);
    pipe_wake_readers(&slave_to_master);
    spin_unlock_irqrestore(&slave_to_master.lock, flags);
    assert_eq(2, poll_wake_count);
    assert_true(list_is_empty(&slave_to_master.read_poll));

    poll_table_cleanup(&pt);
    assert_eq(0, pt.nent);
    poll_table_destroy(&pt);

    rc = poll_table_setup(&pt, 2);
    assert_eq(0, rc);
    poll_table_init(&pt);
    master.flags = O_RDWR;
    mask = fd_poll(&master, POLLIN | POLLOUT, &pt);
    assert_eq(0, mask);
    assert_eq(2, pt.nent);
    poll_table_cleanup(&pt);
    assert_true(list_is_empty(&slave_to_master.read_poll));
    assert_true(list_is_empty(&master_to_slave.write_poll));
    poll_table_destroy(&pt);
}

TEST_FUNC(test_real_tty_and_default_devfs_paths)
{
    reset_runtime();
    tty_t tty;
    memset(&tty, 0, sizeof(tty));
    spin_init(&tty.ring_lock);
    list_init(&tty.read_poll);

    poll_table_t pt = {0};
    int rc = poll_table_setup(&pt, 1);
    assert_eq(0, rc);
    poll_table_init(&pt);
    uint32_t mask = tty_poll(&tty, POLLOUT, &pt);
    assert_eq(POLLOUT | POLLWRNORM, mask);
    assert_eq(0, pt.nent);
    mask = tty_poll(&tty, POLLIN, &pt);
    assert_eq(POLLOUT | POLLWRNORM, mask);
    assert_eq(1, pt.nent);
    assert_false(list_is_empty(&tty.read_poll));
    poll_table_cleanup(&pt);
    assert_true(list_is_empty(&tty.read_poll));
    poll_table_destroy(&pt);

    rc = devfs_register_chrdev("poll-default", NULL, NULL);
    assert_eq(0, rc);
    vfs_node_t node = {
        .type = VFS_CHRDEV,
        .fs_data = (void *)(uintptr_t)0,
    };
    file_t device = {
        .type = FD_DEV,
        .flags = O_RDWR | O_NONBLOCK,
        .node = &node,
    };
    mask = fd_poll(&device, POLLIN, NULL);
    assert_eq(POLLIN | POLLRDNORM | POLLOUT | POLLWRNORM, mask);
}

TEST_FUNC(test_poll_table_allocation_bounds)
{
    reset_runtime();
    poll_table_t pt = {0};
    int rc = poll_table_setup(&pt, 0);
    assert_eq(-EINVAL, rc);
    assert_eq(0, allocation_count);
    if (rc == 0)
        poll_table_destroy(&pt);

    allocation_count = 0;
    memset(allocation_sizes, 0, sizeof(allocation_sizes));
    memset(&pt, 0, sizeof(pt));
    rc = poll_table_setup(&pt, POLL_MAX_FDS * 2 + 1);
    assert_eq(-EINVAL, rc);
    assert_eq(0, allocation_count);
    if (rc == 0)
        poll_table_destroy(&pt);

    allocation_count = 0;
    memset(allocation_sizes, 0, sizeof(allocation_sizes));
    memset(&pt, 0, sizeof(pt));
    rc = poll_table_setup(&pt, POLL_MAX_FDS * 2);
    assert_eq(0, rc);
    assert_eq(1, allocation_count);
    assert_eq((size_t)(POLL_MAX_FDS * 2) * sizeof(poll_wait_entry_t),
              allocation_sizes[0]);
    poll_table_destroy(&pt);
}

TEST_FUNC(test_real_poll_and_select_allocate_two_slots_per_fd)
{
    reset_runtime();
    pty_t pty;
    pipe_t master_to_slave;
    pipe_t slave_to_master;
    file_t master = make_blocked_pty_file(&pty, &master_to_slave,
                                          &slave_to_master);

    struct pollfd pfds[POLL_MAX_FDS];
    for (int i = 0; i < POLL_MAX_FDS; i++) {
        test_files.fd[i] = &master;
        pfds[i].fd = i;
        pfds[i].events = POLLIN | POLLOUT;
        pfds[i].revents = 0;
    }
    int64_t rc = do_poll(pfds, POLL_MAX_FDS, 0);
    assert_eq(0, rc);
    assert_eq(1, allocation_count);
    assert_eq((size_t)(POLL_MAX_FDS * 2) * sizeof(poll_wait_entry_t),
              allocation_sizes[0]);
    assert_true(list_is_empty(&slave_to_master.read_poll));
    assert_true(list_is_empty(&master_to_slave.write_poll));

    kernel_fd_set readfds = {0};
    kernel_fd_set writefds = {0};
    kern_fd_set(0, &readfds);
    kern_fd_set(0, &writefds);
    struct timeval tv = {0, 0};
    allocation_count = 0;
    rc = do_select(1, &readfds, &writefds, NULL, &tv);
    assert_eq(0, rc);
    assert_eq(2, allocation_count);
    assert_eq(sizeof(struct pollfd), allocation_sizes[0]);
    assert_eq(2 * sizeof(poll_wait_entry_t), allocation_sizes[1]);
    assert_true(list_is_empty(&slave_to_master.read_poll));
    assert_true(list_is_empty(&master_to_slave.write_poll));

    kern_fd_set(0, &readfds);
    kern_fd_set(0, &writefds);
    struct timespec ts = {0, 0};
    allocation_count = 0;
    rc = do_pselect6(1, &readfds, &writefds, NULL, &ts, NULL);
    assert_eq(0, rc);
    assert_eq(2, allocation_count);
    assert_eq(sizeof(struct pollfd), allocation_sizes[0]);
    assert_eq(2 * sizeof(poll_wait_entry_t), allocation_sizes[1]);
    assert_true(list_is_empty(&slave_to_master.read_poll));
    assert_true(list_is_empty(&master_to_slave.write_poll));
}

// ── fd_read tests (Task 2: preserve negative errno when no bytes
// committed).  Each test wires a fake vfs_read/copy_to_user_ft_res
// and drives the real production fd_read with FD_VFS / FD_DEV.
// OS01_HOST_TEST in file.c neuters the kernel-half ops-pointer
// guard so a host-allocated vfs_ops_t passes the check.
// ─────────────────────────────────────────────────────────────
static int fake_dev_read(struct vfs_node *node, uint64_t offset,
                         uint64_t size, void *buffer)
{
    return vfs_read(node, offset, size, buffer);
}

static void make_vfs_file_with_ops(file_t *out, vfs_node_t **out_node,
                                   vfs_ops_t **out_ops)
{
    static vfs_ops_t  ops;
    static vfs_node_t node;
    static file_t     f;
    memset(&ops, 0, sizeof(ops));
    memset(&node, 0, sizeof(node));
    memset(&f, 0, sizeof(f));
    ops.read = fake_dev_read;
    node.ops = &ops;
    node.type = VFS_CHRDEV;
    f.type = FD_DEV;
    f.flags = O_RDONLY;
    f.node = &node;
    f.offset = 0;
    f.refcount = 1;
    *out_node = &node;
    *out_ops  = &ops;
    *out      = f;
}

TEST_FUNC(test_fd_read_eagain_propagates_errno)
{
    reset_runtime();
    reset_fd_read_mocks();
    file_t f; vfs_node_t *node; vfs_ops_t *ops;
    make_vfs_file_with_ops(&f, &node, &ops);
    push_vfs_read(-EAGAIN, 0);

    uint8_t buf[64];
    int64_t rc = fd_read(&f, buf, sizeof(buf));

    // Task 2 contract: when vfs_read returns -EAGAIN with no bytes
    // committed, fd_read must return -EAGAIN, NOT -1.
    assert_eq(-EAGAIN, rc);
    assert_eq(0, f.offset);
    assert_eq(1, vfs_read_seq_idx);
}

TEST_FUNC(test_fd_read_einval_propagates_errno)
{
    reset_runtime();
    reset_fd_read_mocks();
    file_t f; vfs_node_t *node; vfs_ops_t *ops;
    make_vfs_file_with_ops(&f, &node, &ops);
    push_vfs_read(-EINVAL, 0);

    uint8_t buf[64];
    int64_t rc = fd_read(&f, buf, sizeof(buf));

    assert_eq(-EINVAL, rc);
    assert_eq(0, f.offset);
    assert_eq(1, vfs_read_seq_idx);
}

TEST_FUNC(test_fd_read_eof_returns_zero)
{
    reset_runtime();
    reset_fd_read_mocks();
    file_t f; vfs_node_t *node; vfs_ops_t *ops;
    make_vfs_file_with_ops(&f, &node, &ops);
    push_vfs_read(0, 0);  // EOF

    uint8_t buf[64];
    int64_t rc = fd_read(&f, buf, sizeof(buf));

    // EOF must still return 0 (do NOT regress to -1 — busybox tail
    // depends on this distinction).
    assert_eq(0, rc);
    assert_eq(0, f.offset);
    assert_eq(1, vfs_read_seq_idx);
}

TEST_FUNC(test_fd_read_success_then_errno_returns_short)
{
    // To force fd_read to actually call vfs_read a second time we
    // must satisfy TWO constraints in the production loop:
    //   1. user size > UACCESS_BOUNCE_SIZE (64 KiB) so chunk=64 KiB
    //      and remaining stays > 0 after the first read;
    //   2. first vfs_read returns exactly UACCESS_BOUNCE_SIZE bytes
    //      so the `(uint64_t)n < chunk` short-read break is NOT
    //      taken (the production code treats `n==chunk` as "more
    //      data may follow" — see file.c:697).
    reset_runtime();
    reset_fd_read_mocks();
    file_t f; vfs_node_t *node; vfs_ops_t *ops;
    make_vfs_file_with_ops(&f, &node, &ops);
    push_vfs_read(64 * 1024, 64 * 1024);   // first: fill the 64 KiB chunk
    push_vfs_read(-EAGAIN, 0);             // then EAGAIN

    uint8_t *buf = malloc(128 * 1024);
    int64_t rc = fd_read(&f, buf, 128 * 1024);

    // Once any bytes are committed, fd_read returns the short count
    // (existing behaviour) and offset advances accordingly.  This
    // regression-tests that we did NOT change the post-commit path.
    assert_eq(64 * 1024, rc);
    assert_eq(64 * 1024, f.offset);
    assert_eq(2, vfs_read_seq_idx);
    free(buf);
}

TEST_FUNC(test_fd_read_success_then_eof_returns_short)
{
    reset_runtime();
    reset_fd_read_mocks();
    file_t f; vfs_node_t *node; vfs_ops_t *ops;
    make_vfs_file_with_ops(&f, &node, &ops);
    push_vfs_read(64 * 1024, 64 * 1024);   // first: fill the 64 KiB chunk
    push_vfs_read(0, 0);                   // then EOF

    uint8_t *buf = malloc(128 * 1024);
    int64_t rc = fd_read(&f, buf, 128 * 1024);

    assert_eq(64 * 1024, rc);
    assert_eq(64 * 1024, f.offset);
    assert_eq(2, vfs_read_seq_idx);
    free(buf);
}

TEST_FUNC(test_fd_read_vfs_path_same_contract)
{
    // Same errno-preservation contract for FD_VFS (no DEV-specific
    // difference, but the switch case is shared — guard against
    // accidental divergence).
    reset_runtime();
    reset_fd_read_mocks();
    vfs_ops_t ops; vfs_node_t node; file_t f;
    memset(&ops, 0, sizeof(ops));
    memset(&node, 0, sizeof(node));
    memset(&f, 0, sizeof(f));
    ops.read = fake_dev_read;
    node.ops = &ops;
    node.type = VFS_FILE;
    f.type = FD_VFS;
    f.flags = O_RDONLY;
    f.node = &node;
    push_vfs_read(-EIO, 0);

    uint8_t buf[64];
    int64_t rc = fd_read(&f, buf, sizeof(buf));
    assert_eq(-EIO, rc);
    assert_eq(0, f.offset);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_direction_policy),
    TEST_ENTRY(test_requested_registration_policy),
    TEST_ENTRY(test_real_pty_two_direction_registration_wake_and_cleanup),
    TEST_ENTRY(test_real_tty_and_default_devfs_paths),
    TEST_ENTRY(test_poll_table_allocation_bounds),
    TEST_ENTRY(test_real_poll_and_select_allocate_two_slots_per_fd),
    TEST_ENTRY(test_fd_read_eagain_propagates_errno),
    TEST_ENTRY(test_fd_read_einval_propagates_errno),
    TEST_ENTRY(test_fd_read_eof_returns_zero),
    TEST_ENTRY(test_fd_read_success_then_errno_returns_short),
    TEST_ENTRY(test_fd_read_success_then_eof_returns_short),
    TEST_ENTRY(test_fd_read_vfs_path_same_contract),
TEST_LIST_END

int main(void)
{
    printf("=== Test Runner ===\n");
    for (int i = 0; i < __test_table_size; i++) {
        printf("\n--- %s ---\n", __test_table[i].name);
        __test_table[i].fn();
    }

    int failed = __test_stats.failed;
    TEST_RESULTS();
    return failed > 0 ? 1 : 0;
}
