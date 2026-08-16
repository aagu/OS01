/* Real production-path requested poll direction tests. */

#include <test_framework.h>
#include <errno.h>
#include <fcntl.h>
#include <fs/devfs.h>
#include <kernel/file.h>
#include <kernel/poll.h>
#include <kernel/pty.h>
#include <kernel/select.h>
#include <kernel/tty.h>

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

TEST_LIST_BEGIN
    TEST_ENTRY(test_direction_policy),
    TEST_ENTRY(test_requested_registration_policy),
    TEST_ENTRY(test_real_pty_two_direction_registration_wake_and_cleanup),
    TEST_ENTRY(test_real_tty_and_default_devfs_paths),
    TEST_ENTRY(test_poll_table_allocation_bounds),
    TEST_ENTRY(test_real_poll_and_select_allocate_two_slots_per_fd),
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
