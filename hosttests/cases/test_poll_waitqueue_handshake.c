/* test_poll_waitqueue_handshake.c -- tests for the do_poll_core
 * arm-recheck-schedule handshake (Task 3 of PS/2 mouse driver plan).
 *
 * Compiled and linked identical to test_poll_requested.c:
 *   - Real kernel/fs/poll.c compiled via POLL_HOST_CFLAGS.
 *   - This TU provides the wait_queue helpers, task_wake, and
 *     schedule with REAL semantics (not stubs): wait_queue_arm
 *     enqueues and sets state=INTERRUPTIBLE under the wq lock;
 *     wait_queue_wake_one/all dequeues and sets state=RUNNING;
 *     schedule does NOT sleep, but records entry state and may
 *     fire an optional wake-during-schedule hook plus advance
 *     jiffies to let the loop terminate.
 *
 * Key contract verified:
 *   - do_poll_core never calls schedule unless the loop must
 *     sleep (i.e., scan1 not ready, scan2 not ready, no signal,
 *     deadline not yet reached).
 *   - On any return path: state equals TASK_RUNNING, current
 *     is not on pt.wq (io_wait_node empty), fd poll chains are
 *     cleaned, finite-timeout tmo node is unregistered.
 *   - Signal returns -EINTR without sleeping.
 *
 * Interleaving coverage (the brief's two race windows) is driven by a
 * producer hook fired from inside this TU's wait_queue_arm — the only
 * single-threaded vantage point from which we can observe that
 * do_poll_core has finished its first scan but not yet enqueued us:
 *
 *   wake-between-scan1-and-ARM   (the OLD code's lost-wakeup bug):
 *     hook fires BEFORE enqueue: push a byte into the pipe AND
 *     wait_queue_wake_all(pt.wq) — the wake lands on an EMPTY queue
 *     and is silently dropped, exactly like a real producer racing
 *     the poller.  NEW code must catch this via the post-ARM re-scan
 *     (pt=NULL) and return ready WITHOUT ever calling schedule().
 *     OLD code called wait_queue_sleep and slept — its schedule entry
 *     state was TASK_INTERRUPTIBLE with the wake already lost, so the
 *     "schedule_call_count == 0" assertion is the RED discriminator.
 *
 *   wake-between-ARM-and-schedule:
 *     hook fires AFTER enqueue: wait_queue_wake_all(pt.wq) dequeues
 *     us and sets TASK_RUNNING, but the data byte is NOT pushed yet
 *     (the schedule mock pushes it) so the re-scan still reports
 *     not-ready and do_poll_core proceeds to schedule().  The task
 *     must enter schedule as TASK_RUNNING — never re-entering
 *     uninterruptible sleep after a wake.
 *
 * On every return path we assert: state == TASK_RUNNING, current is
 * off pt.wq (io_wait_node empty), fd poll chains cleaned, and for
 * finite timeouts the tmo node is unregistered.
 */

#include <test_framework.h>
#include <errno.h>
#include <fcntl.h>
#include <fs/file.h>
#include <fs/poll.h>
#include <arch/x86_64/clocksource.h>
#include <time/clocksource.h>
#include <list.h>
#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <string.h>

/* ---------- Process-local task + files ---------- */

static task_t  test_task;
static files_t test_files;
task_t *poll_test_current = &test_task;

/* jiffies referenced by inline clocksource_read_ns() in
 * <arch/x86_64/clocksource.h> (host default: clocksource_active=false
 * => returns jiffies * 10ms).  Declared extern in <time/timer.h>; we
 * define it here. */
uint64_t volatile jiffies = 0;

/* ---------- kmalloc / kfree (bump-allocator captures sizes) ---------- */

#define KMALLOC_MAX 16
static size_t allocation_sizes[KMALLOC_MAX];
static int    allocation_count;

void *kmalloc(size_t size)
{
    if (allocation_count < KMALLOC_MAX)
        allocation_sizes[allocation_count] = size;
    allocation_count++;
    void *p = malloc(size ? size : 1);
    return p;
}

size_t kfree(void *ptr)
{
    if (ptr) free(ptr);
    return 0;
}

/* ---------- Real-semantics wait_queue_*  ---------- */

void wait_queue_init(wait_queue_t *wq)
{
    list_init(&wq->head);
    spin_init(&wq->lock);
}

void task_wake(task_t *t)
{
    t->state = TASK_RUNNING;
}

static void do_wake_one_locked(wait_queue_t *wq)
{
    if (!list_is_empty(&wq->head)) {
        list_t *node = wq->head.next;
        list_del_init(node);
        task_t *t = container_of(node, task_t, io_wait_node);
        task_wake(t);
    }
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
    do_wake_one_locked(wq);
    spin_unlock_irqrestore(&wq->lock, flags);
}

void wait_queue_wake_all(wait_queue_t *wq)
{
    uint64_t flags = spin_lock_irqsave(&wq->lock);
    do_wake_all_locked(wq);
    spin_unlock_irqrestore(&wq->lock, flags);
}

/* ---------- Producer hook (race-window interleaving) ----------
 * Fired from inside wait_queue_arm — the point where do_poll_core
 * has finished scan1 (fd chains registered) and is about to (or has
 * just) enqueued us on pt.wq.  This is the only single-threaded
 * vantage point over the scan1→ARM→schedule handshake. */

enum arm_hook_mode {
    ARM_HOOK_OFF = 0,
    ARM_HOOK_BEFORE_ENQUEUE,   /* wake lands on EMPTY queue: lost    */
    ARM_HOOK_AFTER_ENQUEUE,    /* wake dequeues us: caught           */
};

static enum arm_hook_mode arm_hook_mode;
static bool               arm_hook_fired;
static bool               schedule_push_byte;   /* schedule-time producer write */

static void mock_pipe_push_byte(uint8_t b);     /* defined below */

void wait_queue_arm(wait_queue_t *wq)
{
    if (arm_hook_mode == ARM_HOOK_BEFORE_ENQUEUE && !arm_hook_fired) {
        arm_hook_fired = true;
        /* Producer: make data visible, then wake.  pt.wq is still
         * EMPTY here — the wake is silently dropped (the old code's
         * lost-wakeup window). */
        mock_pipe_push_byte('R');
        wait_queue_wake_all(wq);
    }

    uint64_t flags = spin_lock_irqsave(&wq->lock);
    list_add_to_before(&wq->head, &current->io_wait_node);
    current->state = TASK_INTERRUPTIBLE;
    spin_unlock_irqrestore(&wq->lock, flags);

    if (arm_hook_mode == ARM_HOOK_AFTER_ENQUEUE && !arm_hook_fired) {
        arm_hook_fired = true;
        /* Producer wakes between ARM and schedule: dequeues us and
         * sets TASK_RUNNING.  Data is NOT pushed yet — the schedule
         * mock pushes it — so the post-ARM re-scan still reports
         * not-ready and we proceed to schedule() as RUNNING. */
        wait_queue_wake_all(wq);
    }
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

static int     schedule_call_count;
static int64_t schedule_entry_state;
static bool    schedule_wake_hook_enabled;
static bool    schedule_jiffies_bump_enabled;
static poll_table_t *current_pt_for_wake_hook;

void schedule(void)
{
    schedule_call_count++;
    schedule_entry_state = current->state;
    if (schedule_push_byte) {
        /* Producer's data lands while we are inside schedule (i.e.
         * the wake fired before schedule was entered and the write
         * became visible only now). */
        mock_pipe_push_byte('W');
    }
    if (schedule_wake_hook_enabled && current_pt_for_wake_hook)
        wait_queue_wake_one(&current_pt_for_wake_hook->wq);
    if (schedule_jiffies_bump_enabled)
        jiffies += 1;
}

/* ---------- Mock pipe for FD_PIPE fd ---------- */

static pipe_t mock_pipe;
static bool   mock_pipe_inited;

static void mock_pipe_init(void)
{
    if (mock_pipe_inited) return;
    memset(&mock_pipe, 0, sizeof(mock_pipe));
    /* kernel pipe_t has char *buf (heap-allocated, PIPE_SIZE bytes).
     * Allocate it so production fd_poll's pipe_full / pipe reads work. */
    mock_pipe.buf = malloc(PIPE_SIZE);
    spin_init(&mock_pipe.lock);
    list_init(&mock_pipe.read_poll);
    list_init(&mock_pipe.write_poll);
    mock_pipe.readers = 1;
    mock_pipe.writers = 1;
    mock_pipe_inited = true;
}

/* Push a byte into the mock pipe ring buffer.  Used to flip
 * readiness between test setup and do_poll_core call. */
static void mock_pipe_push_byte(uint8_t b)
{
    uint64_t flags = spin_lock_irqsave(&mock_pipe.lock);
    int next = (mock_pipe.head + 1) % PIPE_SIZE;
    mock_pipe.buf[mock_pipe.head] = b;
    mock_pipe.head = next;
    spin_unlock_irqrestore(&mock_pipe.lock, flags);
}

/* Drain the pipe (empty). */
static void mock_pipe_clear(void)
{
    uint64_t flags = spin_lock_irqsave(&mock_pipe.lock);
    mock_pipe.head = 0;
    mock_pipe.tail = 0;
    spin_unlock_irqrestore(&mock_pipe.lock, flags);
}

static file_t make_pipe_reader_file(void)
{
    file_t f = {
        .type = FD_PIPE,
        .refcount = 1,
        .flags = O_RDONLY | O_NONBLOCK,
        .pipe = &mock_pipe,
    };
    return f;
}

/* ---------- poll_timeout registry introspection ---------- */

extern spinlock_T            poll_timeout_lock;
extern poll_timeout_node_t  *poll_timeout_head;

static bool poll_tmo_head_contains(poll_table_t *pt)
{
    uint64_t flags = spin_lock_irqsave(&poll_timeout_lock);
    bool found = false;
    for (poll_timeout_node_t *n = poll_timeout_head; n; n = n->next) {
        if (n == &pt->tmo) { found = true; break; }
    }
    spin_unlock_irqrestore(&poll_timeout_lock, flags);
    return found;
}

/* ---------- Test runtime fixtures ---------- */

static void reset_runtime(void)
{
    memset(&test_task, 0, sizeof(test_task));
    memset(&test_files, 0, sizeof(test_files));
    memset(allocation_sizes, 0, sizeof(allocation_sizes));
    allocation_count = 0;
    schedule_call_count = 0;
    schedule_entry_state = 0;
    schedule_wake_hook_enabled    = false;
    schedule_jiffies_bump_enabled = false;
    current_pt_for_wake_hook = NULL;
    arm_hook_mode          = ARM_HOOK_OFF;
    arm_hook_fired         = false;
    schedule_push_byte     = false;
    test_task.addr_limit = UINT64_MAX;
    test_task.files = &test_files;
    test_task.state = TASK_RUNNING;
    list_init(&test_task.io_wait_node);
    jiffies = 0;
    mock_pipe_init();
    mock_pipe_clear();
}

static int64_t run_poll_core(struct pollfd *pfds, int nfds,
                            int64_t timeout_val, poll_table_t *pt)
{
    schedule_call_count = 0;
    schedule_entry_state = 0;
    return do_poll_core(pfds, (uint32_t)nfds, timeout_val, pt);
}

/* ============================================================
 * Unit tests for the wait_queue_handshake helpers themselves.
 * ============================================================ */

TEST_FUNC(test_arm_then_wake_one_makes_running_and_unlinks_node)
{
    reset_runtime();
    wait_queue_t wq;
    wait_queue_init(&wq);

    assert_eq(TASK_RUNNING, current->state);
    assert_true(list_is_empty(&wq.head));
    assert_true(list_is_empty(&current->io_wait_node));

    wait_queue_arm(&wq);
    assert_eq(TASK_INTERRUPTIBLE, current->state);
    assert_false(list_is_empty(&wq.head));
    assert_false(list_is_empty(&current->io_wait_node));

    wait_queue_wake_one(&wq);
    assert_eq(TASK_RUNNING, current->state);
    assert_true(list_is_empty(&wq.head));
    assert_true(list_is_empty(&current->io_wait_node));

    wait_queue_wake_one(&wq);
    assert_eq(TASK_RUNNING, current->state);
}

TEST_FUNC(test_disarm_when_already_removed_is_noop)
{
    reset_runtime();
    wait_queue_t wq;
    wait_queue_init(&wq);
    current->state = TASK_RUNNING;
    list_init(&current->io_wait_node);

    wait_queue_disarm();
    assert_eq(TASK_RUNNING, current->state);
    assert_true(list_is_empty(&current->io_wait_node));

    wait_queue_arm(&wq);
    assert_eq(TASK_INTERRUPTIBLE, current->state);
    wait_queue_wake_one(&wq);
    assert_true(list_is_empty(&current->io_wait_node));
    wait_queue_disarm();
    assert_eq(TASK_RUNNING, current->state);
    assert_true(list_is_empty(&wq.head));
}

TEST_FUNC(test_arm_under_lock_does_not_lose_wake_within_window)
{
    /* Single-threaded simulation of the race we expect
     * do_poll_core to handle: arm under the wq->lock catches a
     * wake that fires immediately after arm completes. */
    reset_runtime();
    wait_queue_t wq;
    wait_queue_init(&wq);

    wait_queue_arm(&wq);
    wait_queue_wake_one(&wq);

    assert_eq(TASK_RUNNING, current->state);
    assert_true(list_is_empty(&wq.head));
    assert_true(list_is_empty(&current->io_wait_node));
}

/* ============================================================
 * Integration tests on do_poll_core.
 *
 * We use production fd_poll's FD_PIPE branch with a mock pipe
 * whose head/tail we control directly.  Production fd_poll
 * reads mock_pipe->head/tail and returns POLLIN when not empty.
 *
 * Test expectations encoded as "schedule_call_count == 0"
 * distinguish fast-exit paths (ready, timeout-bypass, signal)
 * from the schedule path.  The two race windows (wake between
 * scan1 and ARM; wake between ARM and schedule) are driven by
 * the producer hook inside wait_queue_arm — see the TU header.
 * ============================================================ */

TEST_FUNC(test_poll_immediate_ready_no_schedule_no_residue)
{
    reset_runtime();
    /* Pre-load the pipe so fd_poll sees POLLIN on the very
     * first scan. */
    mock_pipe_push_byte('A');

    file_t f = make_pipe_reader_file();
    test_files.fd[0] = &f;
    struct pollfd pfds[1] = {
        { .fd = 0, .events = POLLIN, .revents = 0 },
    };

    poll_table_t pt = {0};
    int rc = poll_table_setup(&pt, 2);
    assert_eq(0, rc);

    int64_t got = run_poll_core(pfds, 1, 1000, &pt);
    assert_true(got >= 1);
    assert_eq(0, schedule_call_count);
    assert_eq(TASK_RUNNING, current->state);
    assert_true(list_is_empty(&pt.wq.head));
    assert_false(poll_tmo_head_contains(&pt));

    poll_table_destroy(&pt);
}

TEST_FUNC(test_poll_nonblocking_timeout_zero_returns_zero_no_schedule)
{
    reset_runtime();
    mock_pipe_clear();

    file_t f = make_pipe_reader_file();
    test_files.fd[0] = &f;
    struct pollfd pfds[1] = {
        { .fd = 0, .events = POLLIN, .revents = 0 },
    };

    poll_table_t pt = {0};
    int rc = poll_table_setup(&pt, 2);
    assert_eq(0, rc);

    int64_t got = run_poll_core(pfds, 1, 0, &pt);
    assert_eq(0, got);
    assert_eq(0, schedule_call_count);
    assert_eq(TASK_RUNNING, current->state);
    assert_true(list_is_empty(&pt.wq.head));
    assert_false(poll_tmo_head_contains(&pt));

    poll_table_destroy(&pt);
}

TEST_FUNC(test_poll_signal_returns_eintr_no_schedule_no_residue)
{
    reset_runtime();
    mock_pipe_clear();

    file_t f = make_pipe_reader_file();
    test_files.fd[0] = &f;
    struct pollfd pfds[1] = {
        { .fd = 0, .events = POLLIN, .revents = 0 },
    };

    current->signal = 1;
    current->blocked = 0;

    poll_table_t pt = {0};
    int rc = poll_table_setup(&pt, 2);
    assert_eq(0, rc);

    int64_t got = run_poll_core(pfds, 1, -1 /* infinite */, &pt);
    assert_eq(-(int64_t)EINTR, got);
    assert_eq(0, schedule_call_count);
    assert_eq(TASK_RUNNING, current->state);
    assert_true(list_is_empty(&pt.wq.head));
    assert_false(poll_tmo_head_contains(&pt));

    poll_table_destroy(&pt);
}

TEST_FUNC(test_poll_finite_timeout_no_ready_returns_zero_no_residue)
{
    /* Finite timeout (1ms): the deadline check fires in the
     * post-sleep path after the schedule hook bumps jiffies past
     * the deadline.  jiffies=0 at entry -> deadline = 0 + 1ms.
     * First iteration's pre-sleep deadline check is false
     * (ns=0 < deadline=1ms).  schedule hook bumps jiffies to 1
     * -> ns=10ms.  Post-sleep deadline check 10ms >= 1ms is
     * true -> return 0. */
    reset_runtime();
    mock_pipe_clear();

    file_t f = make_pipe_reader_file();
    test_files.fd[0] = &f;
    struct pollfd pfds[1] = {
        { .fd = 0, .events = POLLIN, .revents = 0 },
    };

    poll_table_t pt = {0};
    int rc = poll_table_setup(&pt, 2);
    assert_eq(0, rc);

    schedule_jiffies_bump_enabled = true;

    int64_t got = run_poll_core(pfds, 1, 1, &pt);
    assert_eq(0, got);
    assert_true(schedule_call_count >= 1);
    assert_eq(TASK_RUNNING, current->state);
    assert_true(list_is_empty(&pt.wq.head));
    assert_false(poll_tmo_head_contains(&pt));

    schedule_jiffies_bump_enabled = false;
    poll_table_destroy(&pt);
}

TEST_FUNC(test_poll_wake_during_schedule_resolves_cleanly)
{
    /* Finite-timeout + never-ready pipe: the schedule hook both
     * fires a wake and bumps jiffies so the loop eventually
     * trips the deadline check.  We assert no crash, final
     * state=RUNNING, pt.wq empty, tmo unregistered. */
    reset_runtime();
    mock_pipe_clear();

    file_t f = make_pipe_reader_file();
    test_files.fd[0] = &f;
    struct pollfd pfds[1] = {
        { .fd = 0, .events = POLLIN, .revents = 0 },
    };

    poll_table_t pt = {0};
    int rc = poll_table_setup(&pt, 2);
    assert_eq(0, rc);

    schedule_wake_hook_enabled    = true;
    schedule_jiffies_bump_enabled = true;
    current_pt_for_wake_hook      = &pt;

    int64_t got = run_poll_core(pfds, 1, 1, &pt);

    /* Loop terminates via the deadline check (jiffies=1 -> ns=10ms
     * > 1ms deadline; the post-sleep check also fires). */
    assert_eq(0, got);
    assert_true(schedule_call_count >= 1);
    assert_eq(TASK_RUNNING, current->state);
    assert_true(list_is_empty(&pt.wq.head));
    assert_true(list_is_empty(&current->io_wait_node));
    assert_true(list_is_empty(&mock_pipe.read_poll));
    assert_false(poll_tmo_head_contains(&pt));

    schedule_wake_hook_enabled    = false;
    schedule_jiffies_bump_enabled = false;
    current_pt_for_wake_hook      = NULL;
    poll_table_destroy(&pt);
}

TEST_FUNC(test_poll_wake_between_scan1_and_arm_is_not_lost)
{
    /* THE lost-wakeup window (brief: wake 位于初扫与任务入队之间):
     * the producer pushes data and wakes pt.wq AFTER scan1
     * registered the fd chains but BEFORE our task is enqueued.
     * The wake hits an empty queue and is silently dropped — the
     * old code then slept despite ready data.  The
     * arm-重查-schedule handshake must catch it: the post-ARM
     * re-scan (pt=NULL) sees the data and do_poll_core returns
     * ready WITHOUT ever calling schedule().  The
     * schedule_call_count == 0 assertion is the RED discriminator
     * against the old scan→wait_queue_sleep order. */
    reset_runtime();

    file_t f = make_pipe_reader_file();
    test_files.fd[0] = &f;
    struct pollfd pfds[1] = {
        { .fd = 0, .events = POLLIN, .revents = 0 },
    };

    poll_table_t pt = {0};
    int rc = poll_table_setup(&pt, 2);
    assert_eq(0, rc);

    arm_hook_mode = ARM_HOOK_BEFORE_ENQUEUE;

    int64_t got = run_poll_core(pfds, 1, -1 /* infinite */, &pt);

    assert_true(arm_hook_fired);        /* we did reach the arm path  */
    assert_true(got >= 1);              /* the data was not lost      */
    assert_eq(0, schedule_call_count);  /* never slept                */
    assert_eq(TASK_RUNNING, current->state);
    assert_true(list_is_empty(&pt.wq.head));
    assert_true(list_is_empty(&current->io_wait_node));
    assert_true(list_is_empty(&mock_pipe.read_poll));
    assert_eq(POLLIN, pfds[0].revents);

    poll_table_destroy(&pt);
}

TEST_FUNC(test_poll_wake_between_arm_and_schedule_no_resleep)
{
    /* Wake 位于任务入队与 schedule 之间: the producer wakes pt.wq
     * AFTER we are enqueued but BEFORE schedule — the wake dequeues
     * us and sets TASK_RUNNING.  The data byte only becomes visible
     * inside schedule (the mock pushes it there), so the post-ARM
     * re-scan correctly reports not-ready and do_poll_core proceeds
     * to schedule().  The task must ENTER schedule already
     * TASK_RUNNING (never re-enter uninterruptible sleep after a
     * wake), and the post-schedule rescan returns the ready count
     * with no residue. */
    reset_runtime();

    file_t f = make_pipe_reader_file();
    test_files.fd[0] = &f;
    struct pollfd pfds[1] = {
        { .fd = 0, .events = POLLIN, .revents = 0 },
    };

    poll_table_t pt = {0};
    int rc = poll_table_setup(&pt, 2);
    assert_eq(0, rc);

    arm_hook_mode      = ARM_HOOK_AFTER_ENQUEUE;
    schedule_push_byte = true;

    int64_t got = run_poll_core(pfds, 1, -1 /* infinite */, &pt);

    assert_true(arm_hook_fired);
    assert_eq(1, schedule_call_count);
    assert_eq(TASK_RUNNING, schedule_entry_state); /* woken pre-schedule */
    assert_true(got >= 1);
    assert_eq(TASK_RUNNING, current->state);
    assert_true(list_is_empty(&pt.wq.head));
    assert_true(list_is_empty(&current->io_wait_node));
    assert_true(list_is_empty(&mock_pipe.read_poll));
    assert_false(poll_tmo_head_contains(&pt));

    poll_table_destroy(&pt);
}

TEST_FUNC(test_poll_nfds_zero_eventually_unregisters_tmo)
{
    reset_runtime();
    mock_pipe_clear();

    poll_table_t pt = {0};
    int rc = poll_table_setup(&pt, 1);
    assert_eq(0, rc);

    /* jiffies=0 set deadline = 0 + 1ms = 1ms; pre-sleep deadline
     * check (ns=0 < 1ms) false; schedule hook bumps to 1; post-
     * sleep check 10ms >= 1ms true -> return 0. */
    schedule_jiffies_bump_enabled = true;

    int64_t got = run_poll_core(NULL, 0, 1, &pt);
    assert_eq(0, got);
    assert_true(schedule_call_count >= 1);
    assert_eq(TASK_RUNNING, current->state);
    assert_false(poll_tmo_head_contains(&pt));

    schedule_jiffies_bump_enabled = false;
    poll_table_destroy(&pt);
}

/* ============================================================ */

TEST_LIST_BEGIN
    TEST_ENTRY(test_arm_then_wake_one_makes_running_and_unlinks_node),
    TEST_ENTRY(test_disarm_when_already_removed_is_noop),
    TEST_ENTRY(test_arm_under_lock_does_not_lose_wake_within_window),
    TEST_ENTRY(test_poll_immediate_ready_no_schedule_no_residue),
    TEST_ENTRY(test_poll_nonblocking_timeout_zero_returns_zero_no_schedule),
    TEST_ENTRY(test_poll_signal_returns_eintr_no_schedule_no_residue),
    TEST_ENTRY(test_poll_finite_timeout_no_ready_returns_zero_no_residue),
    TEST_ENTRY(test_poll_wake_during_schedule_resolves_cleanly),
    TEST_ENTRY(test_poll_wake_between_scan1_and_arm_is_not_lost),
    TEST_ENTRY(test_poll_wake_between_arm_and_schedule_no_resleep),
    TEST_ENTRY(test_poll_nfds_zero_eventually_unregisters_tmo),
TEST_LIST_END

int main(void)
{
    printf("=== Test Runner: poll waitqueue handshake ===\n");
    for (int i = 0; i < __test_table_size; i++) {
        printf("\n--- %s ---\n", __test_table[i].name);
        __test_table[i].fn();
    }
    TEST_RESULTS();
    int failed = __test_stats.failed;
    return failed > 0 ? 1 : 0;
}
