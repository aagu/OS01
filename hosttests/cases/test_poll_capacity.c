/* test_poll_capacity.c — the poll capacity contract (ARCH-10 Task 7).
 *
 * poll() sizes BOTH of its per-call buffers (the pollfd copy and the
 * wait-entry array) from the task's RLIMIT_NOFILE soft limit.  This
 * case drives the REAL production code (kernel/fs/poll.c compiled by
 * the hosttests/mk/fs.mk rules) and asserts the contract:
 *
 *   - a poll() call admits exactly the soft limit and refuses one more
 *     (Linux semantics for nfds beyond RLIMIT_NOFILE);
 *   - every descriptor registers two wait entries (read + write), and
 *     a descriptor repeated in the array is charged once PER ENTRY;
 *   - a wait chain that cannot be fully registered fails explicitly
 *     with -ENOMEM instead of sleeping forever (no schedule call);
 *   - data delivered to the LAST descriptor wins over a signal raised
 *     in the same window (the data-before-EINTR rule);
 *   - every allocation failure and every user-copy failure leaves no
 *     active timeout node and no entry node behind.
 *
 * The case is built TWICE from this single source:
 *
 *   test_poll_capacity.elf     the default build (OS01_NOFILE_CAPACITY=16)
 *   test_poll_nofile64.elf     the NOFILE64 build (OS01_NOFILE_CAPACITY=64,
 *                              separate object directory — the 16-layout
 *                              objects are never mixed with the 64-layout
 *                              ones)
 *
 * Every expectation is derived from CAPACITY (this build's capacity),
 * so the same source asserts the same contract in both layouts.
 */

#include <test_framework.h>
#include <errno.h>
#include <fcntl.h>
#include <fs/file.h>
#include <fs/poll.h>
#include <tty/pty.h>
#include <lwip/api.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>

/* This build's descriptor capacity: 16 by default, 64 in the
 * NOFILE64 build.  The soft limit the production code reads through
 * task_nofile_soft_current() is the same value in the host harness
 * (hosttests/mock/nofile_soft_stub.c reports OS01_NOFILE_CAPACITY). */
#define CAPACITY  OS01_NOFILE_CAPACITY

/* ---------- Process-local task + descriptor table ---------- */

static task_t  test_task;
static files_t test_files;
task_t *poll_test_current = &test_task;

/* jiffies backs the inactive clocksource fallback
 * (clocksource_read_ns() == jiffies * 10ms). */
uint64_t volatile jiffies = 0;

/* ---------- kmalloc / kfree with failure injection ---------- */

#define KMALLOC_MAX 8
static size_t kmalloc_sizes[KMALLOC_MAX];
static void  *kmalloc_ptrs[KMALLOC_MAX];
static int    kmalloc_calls;
static int    kmalloc_fail_at = -1;   /* index of the failing call */
static void  *freed_ptrs[KMALLOC_MAX];
static int    freed_count;

void *kmalloc(size_t size)
{
    int idx = kmalloc_calls++;
    if (idx < KMALLOC_MAX) {
        kmalloc_sizes[idx] = size;
        kmalloc_ptrs[idx] = NULL;
    }
    if (kmalloc_fail_at == idx)
        return NULL;
    void *p = malloc(size ? size : 1);
    if (idx < KMALLOC_MAX)
        kmalloc_ptrs[idx] = p;
    return p;
}

size_t kfree(void *ptr)
{
    if (ptr) {
        if (freed_count < KMALLOC_MAX)
            freed_ptrs[freed_count++] = ptr;
        free(ptr);
    }
    return 0;
}

static bool was_freed(void *p)
{
    if (!p)
        return false;
    for (int i = 0; i < freed_count; i++)
        if (freed_ptrs[i] == p)
            return true;
    return false;
}

/* Every buffer that WAS allocated must have been released again.  A
 * failed allocation returns NULL — there is nothing to release. */
static void assert_all_allocations_released(void)
{
    for (int i = 0; i < kmalloc_calls; i++)
        if (kmalloc_ptrs[i])
            assert_true(was_freed(kmalloc_ptrs[i]));
}

/* ---------- descriptor fixtures ---------- */

/* One PTY master per descriptor index.  Its slave→master pipe is
 * empty (the read side waits) and its master→slave pipe is full (the
 * write side waits), so every descriptor registers TWO wait entries. */
static pty_t  pty_pool[CAPACITY];
static pipe_t pty_master_to_slave[CAPACITY];
static pipe_t pty_slave_to_master[CAPACITY];
static file_t fd_pool[CAPACITY];

/* ---------- wait queue with real semantics ---------- */

void wait_queue_init(wait_queue_t *wq)
{
    list_init(&wq->head);
    spin_init(&wq->lock);
}

void task_wake(task_t *t)
{
    t->state = TASK_RUNNING;
}

static void do_wake_all_locked(wait_queue_t *wq)
{
    while (!list_is_empty(&wq->head)) {
        list_t *node = wq->head.next;
        list_del_init(node);
        task_t *t = container_of(node, task_t, io_wait_node);
        task_wake(t);
    }
}

void wait_queue_wake_one(wait_queue_t *wq)
{
    uint64_t flags = spin_lock_irqsave(&wq->lock);
    if (!list_is_empty(&wq->head)) {
        list_t *node = wq->head.next;
        list_del_init(node);
        task_wake(container_of(node, task_t, io_wait_node));
    }
    spin_unlock_irqrestore(&wq->lock, flags);
}

void wait_queue_wake_all(wait_queue_t *wq)
{
    uint64_t flags = spin_lock_irqsave(&wq->lock);
    do_wake_all_locked(wq);
    spin_unlock_irqrestore(&wq->lock, flags);
}

/* Producer hook: fired inside wait_queue_arm(), i.e. after the first
 * scan registered the fd wait chains but before the task is enqueued
 * on pt.wq.  The hook makes the selected descriptor readable and
 * raises a signal in that same window — the exact interleaving the
 * data-before-EINTR rule has to survive. */
static int arm_hook_fd = -1;

void wait_queue_arm(wait_queue_t *wq)
{
    if (arm_hook_fd >= 0) {
        int idx = arm_hook_fd;
        arm_hook_fd = -1;
        pipe_t *p = &pty_slave_to_master[idx];
        uint64_t flags = spin_lock_irqsave(&p->lock);
        if (p->buf)
            p->buf[p->head] = 'R';
        p->head = (p->head + 1) % PIPE_SIZE;
        spin_unlock_irqrestore(&p->lock, flags);
        current->signal = 1;
        current->blocked = 0;
    }

    uint64_t flags = spin_lock_irqsave(&wq->lock);
    list_add_to_before(&wq->head, &current->io_wait_node);
    current->state = TASK_INTERRUPTIBLE;
    spin_unlock_irqrestore(&wq->lock, flags);
}

void wait_queue_disarm(void)
{
    if (!list_is_empty(&current->io_wait_node))
        list_del_init(&current->io_wait_node);
    current->state = TASK_RUNNING;
}

void wait_queue_sleep(wait_queue_t *wq)
{
    wait_queue_arm(wq);
    schedule();
    wait_queue_disarm();
}

/* ---------- schedule mock ---------- */

static int schedule_calls;

void schedule(void)
{
    schedule_calls++;
    jiffies += 1;      /* let a finite timeout expire */
}

/* ---------- user copy with fault injection ---------- */

static int copy_from_fail;
static int copy_to_fail;

ssize_t copy_from_user_ft_res(void *dst, const void *src, size_t n,
                              void (*on_fault)(void *), void *arg)
{
    (void)on_fault; (void)arg;
    if (copy_from_fail)
        return -EFAULT;
    memcpy(dst, src, n);
    return (ssize_t)n;
}

ssize_t copy_to_user_ft_res(void *dst, const void *src, size_t n,
                            void (*on_fault)(void *), void *arg)
{
    (void)on_fault; (void)arg;
    if (copy_to_fail)
        return -EFAULT;
    memcpy(dst, src, n);
    return (ssize_t)n;
}

/* ---------- lwIP stubs (file.c's FD_SOCKET branch) ---------- */

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

/* ---------- descriptor fixtures ---------- */

static void init_pipe(pipe_t *p, bool full)
{
    memset(p, 0, sizeof(*p));
    p->buf = malloc(PIPE_SIZE);
    p->head = full ? PIPE_SIZE - 1 : 0;
    p->tail = 0;
    p->readers = 1;
    p->writers = 1;
    spin_init(&p->lock);
    wait_queue_init(&p->read_wait);
    wait_queue_init(&p->write_wait);
    list_init(&p->read_poll);
    list_init(&p->write_poll);
}

static void setup_fds(void)
{
    for (int i = 0; i < CAPACITY; i++) {
        init_pipe(&pty_master_to_slave[i], true);
        init_pipe(&pty_slave_to_master[i], false);

        pty_t *pty = &pty_pool[i];
        memset(pty, 0, sizeof(*pty));
        pty->allocated = true;
        pty->master_to_slave = &pty_master_to_slave[i];
        pty->slave_to_master = &pty_slave_to_master[i];
        spin_init(&pty->state_lock);

        file_t *f = &fd_pool[i];
        memset(f, 0, sizeof(*f));
        f->type = FD_PTY_MASTER;
        f->refcount = 1;
        f->flags = O_RDWR;
        f->pty = pty;

        test_files.fd[i] = f;
    }
}

/* ---------- runtime fixtures ---------- */

static void reset_runtime(void)
{
    memset(&test_task, 0, sizeof(test_task));
    memset(&test_files, 0, sizeof(test_files));
    memset(kmalloc_sizes, 0, sizeof(kmalloc_sizes));
    memset(kmalloc_ptrs, 0, sizeof(kmalloc_ptrs));
    memset(freed_ptrs, 0, sizeof(freed_ptrs));
    kmalloc_calls = 0;
    freed_count = 0;
    kmalloc_fail_at = -1;
    schedule_calls = 0;
    copy_from_fail = 0;
    copy_to_fail = 0;
    arm_hook_fd = -1;
    test_task.addr_limit = UINT64_MAX;
    test_task.files = &test_files;
    test_task.state = TASK_RUNNING;
    list_init(&test_task.io_wait_node);
    jiffies = 0;
}

/* The timeout registry is global production state: after every return
 * path it must be empty again (no leaked timeout node). */
extern poll_timeout_node_t *poll_timeout_head;

static bool fd_chains_empty(void)
{
    for (int i = 0; i < CAPACITY; i++) {
        if (!list_is_empty(&pty_slave_to_master[i].read_poll))
            return false;
        if (!list_is_empty(&pty_master_to_slave[i].write_poll))
            return false;
    }
    return true;
}

/* ============================================================
 * Capacity admission
 * ============================================================ */

TEST_FUNC(test_poll_admits_the_soft_limit_and_refuses_one_more)
{
    reset_runtime();
    setup_fds();

    struct pollfd pfds[CAPACITY + 1];
    for (int i = 0; i < CAPACITY + 1; i++) {
        pfds[i].fd = i;
        pfds[i].events = POLLIN | POLLOUT;
        pfds[i].revents = 0;
    }

    /* Exactly the soft limit is admitted.  Nothing is ready and the
     * timeout is 0, so the call returns 0 — and NOT -EINVAL. */
    int64_t rc = do_poll(pfds, CAPACITY, 0);
    assert_eq(0, rc);

    /* One descriptor beyond the soft limit is refused. */
    rc = do_poll(pfds, CAPACITY + 1, 0);
    assert_eq(-(int64_t)EINVAL, rc);

    assert_true(fd_chains_empty());
    assert_null(poll_timeout_head);
}

#if CAPACITY >= 64
TEST_FUNC(test_nofile64_admits_17_and_32_descriptors)
{
    reset_runtime();
    setup_fds();

    struct pollfd pfds[CAPACITY];
    for (int i = 0; i < CAPACITY; i++) {
        pfds[i].fd = i;
        pfds[i].events = POLLIN | POLLOUT;
        pfds[i].revents = 0;
    }

    int64_t rc = do_poll(pfds, 17, 0);
    assert_eq(0, rc);
    rc = do_poll(pfds, 32, 0);
    assert_eq(0, rc);
    rc = do_poll(pfds, CAPACITY, 0);
    assert_eq(0, rc);

    assert_true(fd_chains_empty());
    assert_null(poll_timeout_head);
}
#endif

/* ============================================================
 * Zero-descriptor semantics
 * ============================================================ */

TEST_FUNC(test_poll_zero_descriptors_allocates_no_pollfd_copy)
{
    reset_runtime();
    setup_fds();

    /* nfds == 0: the user array is not touched and the pollfd copy is
     * NOT allocated; a non-positive timeout returns immediately. */
    int64_t rc = do_poll(NULL, 0, 0);
    assert_eq(0, rc);
    assert_eq(0, kmalloc_calls);
    assert_eq(0, schedule_calls);

    /* A positive timeout still goes through the wait protocol — but
     * still without a pollfd copy: only the wait entries (one internal
     * slot) are allocated. */
    rc = do_poll(NULL, 0, 100);
    assert_eq(0, rc);
    assert_eq(1, kmalloc_calls);
    assert_eq((size_t)1 * sizeof(poll_wait_entry_t), kmalloc_sizes[0]);
    assert_true(schedule_calls >= 1);
    assert_true(fd_chains_empty());
    assert_null(poll_timeout_head);
}

/* ============================================================
 * Wait-entry accounting
 * ============================================================ */

TEST_FUNC(test_every_descriptor_registers_two_wait_entries)
{
    reset_runtime();
    setup_fds();

    poll_table_t pt = {0};
    int rc = poll_table_setup(&pt, POLL_WAIT_SLOTS_PER_FD * CAPACITY);
    assert_eq(0, rc);
    if (rc != 0)
        return;                 /* a failed setup leaves no usable table */
    poll_table_init(&pt);

    for (int i = 0; i < CAPACITY; i++)
        (void)fd_poll(&fd_pool[i], POLLIN | POLLOUT, &pt);

    assert_eq(POLL_WAIT_SLOTS_PER_FD * CAPACITY, pt.nent);

    poll_table_cleanup(&pt);
    poll_table_destroy(&pt);
    assert_true(fd_chains_empty());
}

TEST_FUNC(test_duplicate_descriptors_are_charged_per_entry)
{
    reset_runtime();
    setup_fds();

    /* The SAME descriptor repeated CAPACITY times: the wait charge
     * follows the array entry, not the descriptor number. */
    poll_table_t pt = {0};
    int rc = poll_table_setup(&pt, POLL_WAIT_SLOTS_PER_FD * CAPACITY);
    assert_eq(0, rc);
    if (rc != 0)
        return;                 /* a failed setup leaves no usable table */
    poll_table_init(&pt);

    for (int i = 0; i < CAPACITY; i++)
        (void)fd_poll(&fd_pool[0], POLLIN | POLLOUT, &pt);

    assert_eq(POLL_WAIT_SLOTS_PER_FD * CAPACITY, pt.nent);

    poll_table_cleanup(&pt);
    poll_table_destroy(&pt);
    assert_true(fd_chains_empty());
}

TEST_FUNC(test_poll_heap_allocates_both_buffers)
{
    reset_runtime();
    setup_fds();

    struct pollfd pfds[2];
    for (int i = 0; i < 2; i++) {
        pfds[i].fd = i;
        pfds[i].events = POLLIN | POLLOUT;
        pfds[i].revents = 0;
    }

    int64_t rc = do_poll(pfds, 2, 0);
    assert_eq(0, rc);

    /* Two heap buffers per call: the pollfd copy and the wait-entry
     * array.  Neither lives on the kernel stack any more. */
    assert_eq(2, kmalloc_calls);
    assert_eq((size_t)2 * sizeof(struct pollfd), kmalloc_sizes[0]);
    assert_eq((size_t)(POLL_WAIT_SLOTS_PER_FD * 2) * sizeof(poll_wait_entry_t),
              kmalloc_sizes[1]);

    assert_true(fd_chains_empty());
    assert_null(poll_timeout_head);
}

/* ============================================================
 * Capacity shortfall
 * ============================================================ */

TEST_FUNC(test_wait_entry_shortfall_returns_enomem_without_sleeping)
{
    reset_runtime();
    setup_fds();

    /* A deliberately undersized wait table: ONE slot for a descriptor
     * that needs two (read + write).  The second registration cannot
     * fit — poll_wait() must report it instead of silently dropping
     * the wait (which would sleep forever). */
    poll_table_t pt = {0};
    int rc = poll_table_setup(&pt, 1);
    assert_eq(0, rc);
    if (rc != 0)
        return;                 /* a failed setup leaves no usable table */
    poll_table_init(&pt);

    struct pollfd pfds[1] = {
        { .fd = 0, .events = POLLIN | POLLOUT, .revents = 0 },
    };

    int64_t got = do_poll_core(pfds, 1, 1000, &pt);

    assert_eq(-(int64_t)ENOMEM, got);
    assert_eq(0, schedule_calls);
    assert_eq(TASK_RUNNING, current->state);
    assert_true(list_is_empty(&pt.wq.head));
    assert_true(list_is_empty(&current->io_wait_node));
    assert_true(fd_chains_empty());
    assert_null(poll_timeout_head);

    poll_table_destroy(&pt);
}

/* ============================================================
 * Data wins over EINTR
 * ============================================================ */

TEST_FUNC(test_data_at_the_last_descriptor_wins_over_a_signal)
{
    reset_runtime();
    setup_fds();

    struct pollfd pfds[CAPACITY];
    for (int i = 0; i < CAPACITY; i++) {
        pfds[i].fd = i;
        pfds[i].events = POLLIN;
        pfds[i].revents = 0;
    }

    /* The producer makes the LAST descriptor readable and raises a
     * signal in the same window (between the first scan and the
     * re-check).  The ready data must win over the signal. */
    arm_hook_fd = CAPACITY - 1;

    poll_table_t pt = {0};
    int rc = poll_table_setup(&pt, POLL_WAIT_SLOTS_PER_FD * CAPACITY);
    assert_eq(0, rc);
    if (rc != 0)
        return;                 /* a failed setup leaves no usable table */
    poll_table_init(&pt);

    int64_t got = do_poll_core(pfds, CAPACITY, -1 /* infinite */, &pt);

    assert_true(got >= 1);            /* ready — NOT -EINTR */
    assert_eq(0, schedule_calls);
    assert_eq(TASK_RUNNING, current->state);
    assert_true(list_is_empty(&pt.wq.head));
    assert_true(list_is_empty(&current->io_wait_node));
    assert_true(fd_chains_empty());
    assert_null(poll_timeout_head);

    poll_table_destroy(&pt);
}

/* ============================================================
 * Failure paths leave no residue
 * ============================================================ */

TEST_FUNC(test_every_kmalloc_failure_leaves_no_residue)
{
    for (int fail_at = 0; fail_at < 2; fail_at++) {
        reset_runtime();
        setup_fds();

        struct pollfd pfds[1] = {
            { .fd = 0, .events = POLLIN | POLLOUT, .revents = 0 },
        };

        kmalloc_fail_at = fail_at;
        int64_t got = do_poll(pfds, 1, 1000);
        kmalloc_fail_at = -1;

        assert_eq(-(int64_t)ENOMEM, got);
        /* The call stops AT the failing allocation, so exactly
         * fail_at + 1 allocations were attempted. */
        assert_eq(fail_at + 1, kmalloc_calls);
        /* Every buffer that WAS allocated has been released again. */
        assert_all_allocations_released();
        assert_eq(0, schedule_calls);
        assert_true(fd_chains_empty());
        assert_null(poll_timeout_head);
    }

    /* The do_poll path allocates exactly TWO buffers: the pollfd copy
     * and the wait entries.  A third allocation would need a matching
     * failure-injection point above — this assertion fails loudly if
     * one appears. */
    reset_runtime();
    setup_fds();
    struct pollfd pfds[1] = {
        { .fd = 0, .events = POLLIN | POLLOUT, .revents = 0 },
    };
    int64_t got = do_poll(pfds, 1, 0);   /* non-blocking: nothing is ready */
    assert_eq(0, got);
    assert_eq(2, kmalloc_calls);
    assert_all_allocations_released();
    assert_true(fd_chains_empty());
    assert_null(poll_timeout_head);
}

TEST_FUNC(test_input_copy_failure_leaves_no_residue)
{
    reset_runtime();
    setup_fds();

    struct pollfd pfds[1] = {
        { .fd = 0, .events = POLLIN | POLLOUT, .revents = 0 },
    };

    copy_from_fail = 1;
    int64_t got = do_poll(pfds, 1, 1000);
    copy_from_fail = 0;

    assert_eq(-(int64_t)EFAULT, got);
    /* The partially filled pollfd copy must be released. */
    assert_true(kmalloc_calls > 0);
    assert_all_allocations_released();
    assert_eq(0, schedule_calls);
    assert_true(fd_chains_empty());
    assert_null(poll_timeout_head);
}

TEST_FUNC(test_output_copy_failure_leaves_no_residue)
{
    reset_runtime();
    setup_fds();

    struct pollfd pfds[1] = {
        { .fd = 0, .events = POLLIN | POLLOUT, .revents = 0 },
    };

    copy_to_fail = 1;
    int64_t got = do_poll(pfds, 1, 1000);
    copy_to_fail = 0;

    /* The return value still reports the copy failure, even though
     * some revents may already have been written back. */
    assert_eq(-(int64_t)EFAULT, got);
    assert_all_allocations_released();
    assert_true(fd_chains_empty());
    /* The finite timeout node must have been unregistered. */
    assert_null(poll_timeout_head);
}

/* ============================================================
 * The capacity helper
 * ============================================================ */

TEST_FUNC(test_poll_wait_capacity_helper)
{
    int entries = -1;

    /* A zero-descriptor call still needs one internal wait slot. */
    assert_eq(0, poll_wait_capacity(0, &entries));
    assert_eq(1, entries);

    /* Two slots per descriptor. */
    entries = -1;
    assert_eq(0, poll_wait_capacity(3, &entries));
    assert_eq(POLL_WAIT_SLOTS_PER_FD * 3, entries);

    /* A capacity that cannot be represented as an int is refused. */
    assert_eq(-EOVERFLOW, poll_wait_capacity((uint64_t)INT_MAX, &entries));
    assert_eq(-EOVERFLOW, poll_wait_capacity(UINT64_MAX, &entries));

    /* A malformed request. */
    assert_eq(-EINVAL, poll_wait_capacity(1, NULL));
}

TEST_FUNC(test_poll_rejects_a_null_array_before_the_count_check)
{
    reset_runtime();
    setup_fds();

    /* A NULL array with descriptors to report is a bad POINTER, not a
     * bad count: the pointer check wins over the count check (Linux
     * do_poll order; the locked hostile-pointer regression expects
     * -EFAULT for poll(NULL, huge_nfds)). */
    int64_t rc = do_poll(NULL, 1, 0);
    assert_eq(-(int64_t)EFAULT, rc);

    rc = do_poll(NULL, (uint64_t)-1, 0);
    assert_eq(-(int64_t)EFAULT, rc);

    /* A valid array with more descriptors than the soft limit is the
     * count error. */
    struct pollfd pfds[1] = {
        { .fd = 0, .events = POLLIN, .revents = 0 },
    };
    rc = do_poll(pfds, CAPACITY + 1, 0);
    assert_eq(-(int64_t)EINVAL, rc);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_poll_admits_the_soft_limit_and_refuses_one_more),
#if CAPACITY >= 64
    TEST_ENTRY(test_nofile64_admits_17_and_32_descriptors),
#endif
    TEST_ENTRY(test_poll_rejects_a_null_array_before_the_count_check),
    TEST_ENTRY(test_poll_zero_descriptors_allocates_no_pollfd_copy),
    TEST_ENTRY(test_every_descriptor_registers_two_wait_entries),
    TEST_ENTRY(test_duplicate_descriptors_are_charged_per_entry),
    TEST_ENTRY(test_poll_heap_allocates_both_buffers),
    TEST_ENTRY(test_wait_entry_shortfall_returns_enomem_without_sleeping),
    TEST_ENTRY(test_data_at_the_last_descriptor_wins_over_a_signal),
    TEST_ENTRY(test_every_kmalloc_failure_leaves_no_residue),
    TEST_ENTRY(test_input_copy_failure_leaves_no_residue),
    TEST_ENTRY(test_output_copy_failure_leaves_no_residue),
    TEST_ENTRY(test_poll_wait_capacity_helper),
TEST_LIST_END

int main(void)
{
    printf("=== Test Runner: poll capacity (NOFILE=%d) ===\n", CAPACITY);
    RUN_ALL_TESTS();
    int failed = __test_stats.failed;
    return failed > 0 ? 1 : 0;
}
