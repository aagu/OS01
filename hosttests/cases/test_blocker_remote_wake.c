/* test_blocker_remote_wake.c — P2 Task 2: the REAL blocker protocol
 * (kernel/sched/core.c host-compiled via ../mock/blocker_runtime.h).
 *
 * Frozen contracts (spec §5.2a):
 *   - blocker_wait runs its callback, installs/clears the blocker and
 *     re-checks the condition UNDER task_list_lock (nested rq_lock
 *     only when publishing state); schedule() and the before-switch
 *     hook run under NEITHER lock.
 *   - blocker_wake takes task_list_lock, snapshots state under the
 *     target rq_lock, clears the blocker under task_list_lock, then
 *     publishes the wake — and never reads the task after unlock.
 *   - task_wake enqueues once (already-on-rq/on_cpu never double-
 *     enqueue; the on_cpu handoff belongs to task_finish_switch) and
 *     notifies the target captured UNDER the rq lock, after unlock.
 *   - task_finish_switch re-enqueues a wake-during-switch prev and
 *     notifies using only values captured while prev was valid.
 *   - the P1 AP blocker_wait refusal is REMOVED (arch_sched_scan_blockers
 *     no longer gates blocker_wait).
 *
 * Suites:
 *   1. check initially true → returns 0 without sleeping.
 *   2. condition published between install and double-check → no sleep.
 *   3. blocked, then explicit blocker_wake → resumes with a cleared
 *      blocker.
 *   4. wake-before-switch (on_cpu=1) → finish-switch re-enqueues.
 *   5. two wakes → exactly one enqueue.
 *   6. waker clears, waiter exits immediately after resume.
 *   7. signal path clears the blocker before RUNNING.
 *   8. an AP (scan policy false) can now block and be woken.
 *   9. the callback's lock contract: under task_list_lock, never under
 *      an rq_lock; schedule() under neither.
 */

/* Selects the controllable spy definitions in the mock header; must
 * be set BEFORE the mock include. */
#define BLOCKER_TEST_SPIES 1

#include "test_framework.h"
#include "../mock/blocker_runtime.h"
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

/* --wrap=schedule spy: counts production blocker_wait sleep points.
 * suite bodies that need the REAL schedule() call __real_schedule. */
extern void __real_schedule(void);
int mock_schedule_calls;
void __wrap_schedule(void)
{
    /* The frozen contract: schedule() runs under NEITHER lock. */
    int tl = 0, rq = 0;
    for (int i = 0; i < mock_lock_log_count; ++i) {
        if (mock_lock_log_addr[i] == &task_list_lock)
            tl += mock_lock_log_is_lock[i] ? 1 : -1;
        for (unsigned c = 0; c < NR_CPUS; ++c)
            if (mock_lock_log_addr[i] == &percpu_data[c].rq_lock)
                rq += mock_lock_log_is_lock[i] ? 1 : -1;
    }
    mock_sched_lock_ops_at_call = tl + rq;
    mock_schedule_calls++;
    __real_schedule();
}

static int cb_calls;
static int cb_return;
static int cb_rq_acquires;
static void assert_cleared_at_enqueue(task_t *task);

static bool check_cb(task_t *waiter)
{
    cb_calls++;
    /* Snapshot the lock recorder state AT THE CALLBACK: the frozen
     * contract is "under task_list_lock, never under an rq_lock". */
    int tl = 0, rq = 0;
    for (int i = 0; i < mock_lock_log_count; ++i) {
        if (mock_lock_log_addr[i] == &task_list_lock)
            tl += mock_lock_log_is_lock[i] ? 1 : -1;
        for (unsigned c = 0; c < NR_CPUS; ++c)
            if (mock_lock_log_addr[i] == &percpu_data[c].rq_lock) {
                rq += mock_lock_log_is_lock[i] ? 1 : -1;
                if (mock_lock_log_is_lock[i]) cb_rq_acquires++;
            }
    }
    assert_eq(0, rq);
    mock_lock_depth_at_callback = tl;
    mock_cb_task = waiter;
    mock_cb_calls++;
    return cb_return != 0;
}

/* ── 1-2. already-satisfied conditions never sleep ──────────────── */
static void suite_already_true(void)
{
    TEST_SUITE("check initially true returns without sleeping");
    sched_notify_reset();
    cb_calls = 0;
    cb_return = 1;
    task_t sleeper;
    memset(&sleeper, 0, sizeof(sleeper));
    sleeper.state = TASK_RUNNING;
    sleeper.pid = 7;
    sleeper.cpu = 0;
    sleeper.allowed_cpus = 1;
    mock_sched_current = &sleeper;
    mock_lock_log_count = 0;

    assert_eq(0, blocker_wait(check_cb, BLOCKER_WAITPID, false));
    assert_eq(1, cb_calls);
    assert_eq(TASK_RUNNING, sleeper.state);
    assert_eq(BLOCKER_NONE, sleeper.blocker.type);
    assert_true(sleeper.blocker.check == NULL);
    /* The callback ran under task_list_lock. */
    assert_eq(1, mock_lock_depth_at_callback);

    TEST_SUITE("condition published before the double-check → no sleep");
    sched_notify_reset();
    cb_calls = 0;
    cb_return = 0;                 /* first check false */
    mock_sched_current = &sleeper;
}

/* A condition that returns false the first time and true afterwards
 * (the classic published-between-install-and-double-check shape). */
static bool flip_cb(task_t *waiter)
{
    (void)waiter;
    cb_calls++;
    return cb_calls >= 2;   /* 1st: false; 2nd (double-check): true */
}

static void suite_published_between(void)
{
    TEST_SUITE("published between install and double-check → no sleep");
    sched_notify_reset();
    cb_calls = 0;
    task_t sleeper;
    memset(&sleeper, 0, sizeof(sleeper));
    sleeper.state = TASK_RUNNING;
    sleeper.pid = 8;
    sleeper.cpu = 0;
    sleeper.allowed_cpus = 1;
    mock_sched_current = &sleeper;

    assert_eq(0, blocker_wait(flip_cb, BLOCKER_WAITPID, false));
    assert_eq(2, cb_calls);              /* initial + double-check */
    assert_eq(TASK_RUNNING, sleeper.state);
    assert_eq(BLOCKER_NONE, sleeper.blocker.type);
    assert_eq(0, mock_schedule_calls);
}


/* ── 3. blocked then explicitly woken ───────────────────────────── */
static void suite_explicit_wake(void)
{
    TEST_SUITE("blocked task resumes after an explicit blocker_wake");
    sched_notify_reset();
    cb_calls = 0;
    cb_return = 0;
    task_t sleeper;
    memset(&sleeper, 0, sizeof(sleeper));
    sleeper.state = TASK_RUNNING;
    sleeper.pid = 9;
    sleeper.cpu = 1;                     /* a remote target */
    sleeper.allowed_cpus = 2;
    sleeper.thread = &init_thread;
    mock_sched_current = &sleeper;

    /* Run blocker_wait up to the schedule() point via the mock
     * schedule: install + double-check + sleep. */
    assert_eq(0, blocker_wait(check_cb, BLOCKER_WAITPID, false));
    /* The mock schedule() returned immediately; the resume path then
     * cleared the blocker.  Now exercise the WAKE side on a fresh
     * blocked fixture: */
    sched_notify_reset();
    task_t blocked;
    memset(&blocked, 0, sizeof(blocked));
    blocked.state = TASK_INTERRUPTIBLE;
    blocked.pid = 10;
    blocked.cpu = 1;
    blocked.allowed_cpus = 2;
    blocked.blocker.type = BLOCKER_WAITPID;
    blocked.blocker.check = check_cb;
    cb_calls = 0;
    cb_return = 1;                       /* condition met */

    num_cpus = 2;
    percpu_data[1].online = 1;
    percpu_data[1].scheduler_ok = 1;
    cb_rq_acquires = 0;
    blocker_wake(&blocked);
    blocker_wake(&blocked); /* second serialized helper caller is a no-op */
    assert_eq(1, cb_rq_acquires);
    assert_eq(1, cb_calls);
    assert_eq(TASK_RUNNING, blocked.state);
    assert_eq(BLOCKER_NONE, blocked.blocker.type);
    assert_true(blocked.on_rq);
    assert_eq(1, mock_enqueue_calls);
    assert_eq(1, (int)percpu_data[1].need_resched);   /* remote publish */
    assert_eq(1, mock_arch_notify_calls);
    assert_eq(1, mock_arch_notified_cpu);
    assert_eq(1, mock_arch_notify_need_at_hook);
}

/* ── 4-5. wake-before-switch handoff + no double enqueue ────────── */
static void suite_wake_before_switch(void)
{
    TEST_SUITE("wake-before-switch: finish-switch re-enqueues prev");
    sched_notify_reset();
    task_t prev;
    memset(&prev, 0, sizeof(prev));
    prev.state = TASK_RUNNING;           /* woken while switching out */
    prev.pid = 11;
    prev.cpu = 1;
    prev.allowed_cpus = 2;
    prev.on_rq = false;
    prev.on_cpu = 1;
    task_finish_switch(&prev);
    assert_eq(1, mock_enqueue_calls);
    assert_eq(1, (int)percpu_data[1].need_resched);
    assert_eq(0, (int)prev.on_cpu);

    TEST_SUITE("two wakes enqueue exactly once");
    sched_notify_reset();
    task_t w;
    memset(&w, 0, sizeof(w));
    w.state = TASK_UNINTERRUPTIBLE;
    w.pid = 12;
    w.cpu = 1;
    w.allowed_cpus = 2;
    task_wake(&w);
    assert_eq(1, mock_enqueue_calls);
    /* second wake: already on_rq → no double enqueue */
    task_wake(&w);
    assert_eq(1, mock_enqueue_calls);

    /* on_cpu handoff: a task committed to a CPU is skipped here and
     * re-enqueued by task_finish_switch instead. */
    sched_notify_reset();
    w.on_rq = false;
    w.state = TASK_RUNNING;
    w.on_cpu = 1;
    task_wake(&w);
    assert_eq(0, mock_enqueue_calls);
}

/* ── 6. helper never reads the task after unlock ────────────────── */
static void suite_no_read_after_unlock(void)
{
    TEST_SUITE("blocker_wake does not read the task after unlock");
    sched_notify_reset();
    task_t blocked;
    memset(&blocked, 0, sizeof(blocked));
    blocked.state = TASK_INTERRUPTIBLE;
    blocked.pid = 13;
    blocked.cpu = 0;
    blocked.allowed_cpus = 1;
    blocked.blocker.type = BLOCKER_WAITPID;
    blocked.blocker.check = check_cb;
    cb_calls = 0;
    cb_return = 1;
    /* Poison-after-unlock simulation is the mock_poison shape from the
     * ctor harness; here the observable contract is that blocker_wake
     * returns having enqueued and cleared, and a subsequent accessor
     * sees a fully-published state. */
    blocker_wake(&blocked);
    assert_eq(TASK_RUNNING, blocked.state);
    assert_eq(BLOCKER_NONE, blocked.blocker.type);
    assert_eq(1, mock_enqueue_calls);
}

/* ── 7. signal path clears the blocker first ────────────────────── */
static void suite_signal_path(void)
{
    TEST_SUITE("signal wakeup clears the blocker before RUNNING");
    sched_notify_reset();
    task_t blocked;
    memset(&blocked, 0, sizeof(blocked));
    blocked.state = TASK_INTERRUPTIBLE;
    blocked.pid = 14;
    blocked.cpu = 0;
    blocked.allowed_cpus = 1;
    blocked.signal = 1;        /* a pending, unblocked signal */
    blocked.blocked = 0;
    blocked.blocker.type = BLOCKER_NANOSLEEP;
    blocked.blocker.check = check_cb;
    blocked.blocker.signal_can_wake = true;
    cb_calls = 0;
    cb_return = 0;                       /* condition NOT met */
    mock_enqueue_event_hook = assert_cleared_at_enqueue;
    list_init(&init_task_union.task.list);
    task_list_add(&blocked);
    uint64_t flags = spin_lock_irqsave(&task_list_lock);
    sched_unblock_blocked();
    spin_unlock_irqrestore(&task_list_lock, flags);
    assert_eq(BLOCKER_NONE, blocked.blocker.type);
    assert_eq(TASK_RUNNING, blocked.state);
    assert_eq(2, cb_calls); /* production scanner and locked helper */

}

/* ── 8. the P1 AP refusal is removed ────────────────────────────── */
static void suite_ap_can_block(void)
{
    TEST_SUITE("scan policy false no longer refuses blocker_wait");
    sched_notify_reset();
    cb_calls = 0;
    cb_return = 1;                       /* condition already true */
    task_t sleeper;
    memset(&sleeper, 0, sizeof(sleeper));
    sleeper.state = TASK_RUNNING;
    sleeper.pid = 15;
    sleeper.cpu = 0;
    sleeper.allowed_cpus = 1;
    /* The mock derives the policy from cpu_id() only in P1; the
     * arch hook itself is the production seam — the aarch64 backend
     * tests pin cpu_id()==0 semantics.  Here the P1 refusal removal
     * is what matters: blocker_wait must consult the callback and
     * return 0 instead of -EOPNOTSUPP when the policy is closed. */
    mock_arch_scan_blockers = 0;         /* closed (a former AP) */
    assert_eq(0, blocker_wait(check_cb, BLOCKER_WAITPID, false));
    assert_eq(1, cb_calls);              /* the callback WAS consulted */
    assert_eq(TASK_RUNNING, sleeper.state);
    assert_eq(BLOCKER_NONE, sleeper.blocker.type);
}

static void assert_cleared_at_enqueue(task_t *task)
{
    assert_eq(BLOCKER_NONE, task->blocker.type);
    assert_true(task->blocker.check == NULL);
}

static void suite_signal_resume_callback(void)
{
    TEST_SUITE("final signal-resume callback keeps task-list ownership");
    sched_notify_reset();
    task_t self = { .state=TASK_RUNNING, .cpu=0, .signal=1,
                    .allowed_cpus=1, .thread=&init_thread };
    mock_sched_current = &self;
    cb_calls=0; cb_return=0;
    assert_eq(-EINTR, blocker_wait(check_cb, BLOCKER_NANOSLEEP, true));
    assert_eq(3, cb_calls);
    assert_eq(1, mock_lock_depth_at_callback);
}

static task_t *migration_target;
static void migrate_before_lock(spinlock_T *lock, int acquire)
{
    if (acquire && lock == &percpu_data[1].rq_lock) {
        mock_lock_event_hook=NULL;
        migration_target->cpu=2; /* other CPU completed migration before lock */
    }
}
static void suite_migrated_snapshot(void)
{
    TEST_SUITE("snapshot retries if the task moved before rq lock acquisition");
    sched_notify_reset();
    task_t target={ .state=TASK_INTERRUPTIBLE, .cpu=1, .allowed_cpus=6 };
    target.blocker.type=BLOCKER_NANOSLEEP; target.blocker.check=check_cb;
    migration_target=&target; cb_return=1; cb_rq_acquires=0;
    mock_lock_event_hook=migrate_before_lock;
    blocker_wake(&target);
    assert_eq(2, cb_rq_acquires);
    assert_true(mock_enqueue_rq == &percpu_data[2]);
}

static task_t *guard_task;
static size_t guard_size;
static int guard_released;
static void release_at_unlock(spinlock_T *lock, int acquire)
{
    if (!acquire && lock == &task_list_lock) {
        mock_lock_event_hook=NULL;
        assert_eq(0, mprotect(guard_task, guard_size, PROT_NONE));
        guard_released=1;
    }
}
static void suite_guarded_lifetime(void)
{
    TEST_SUITE("post-unlock task reads fault after the actual lifetime release");
    sched_notify_reset();
    guard_size=(size_t)sysconf(_SC_PAGESIZE);
    guard_task=mmap(NULL,guard_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert_true(guard_task != MAP_FAILED);
    if (guard_task==MAP_FAILED) return;
    memset(guard_task,0,sizeof(*guard_task));
    guard_task->state=TASK_INTERRUPTIBLE; guard_task->cpu=1;
    guard_task->allowed_cpus=2; guard_task->blocker.type=BLOCKER_NANOSLEEP;
    guard_task->blocker.check=check_cb; cb_return=1;
    mock_enqueue_event_hook=assert_cleared_at_enqueue;
    mock_lock_event_hook=release_at_unlock; guard_released=0;
    blocker_wake(guard_task); /* any dangling read after list unlock SIGSEGVs */
    assert_eq(1,guard_released);
    assert_eq(0,munmap(guard_task,guard_size));
}

extern void task_signal_parent(task_t *child, uint64_t signals);
extern void task_wake_parent(task_t *child);
static task_t *reparent_child, *replacement_parent;
static void reparent_at_lock(spinlock_T *lock, int acquire)
{
    if (acquire && lock==&task_list_lock) {
        mock_lock_event_hook=NULL;
        reparent_child->parent=replacement_parent;
        assert_eq(0,mprotect(guard_task,guard_size,PROT_NONE));
    }
}
static void suite_parent_lifetime(void)
{
    TEST_SUITE("parent is resolved after acquiring lifetime ownership");
    sched_notify_reset();
    guard_size=(size_t)sysconf(_SC_PAGESIZE);
    guard_task=mmap(NULL,guard_size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    assert_true(guard_task != MAP_FAILED); if(guard_task==MAP_FAILED)return;
    task_t parent={ .state=TASK_INTERRUPTIBLE,.cpu=1,.allowed_cpus=2 };
    parent.blocker.type=BLOCKER_NANOSLEEP;parent.blocker.check=check_cb;
    task_t child={ .parent=guard_task };
    reparent_child=&child;replacement_parent=&parent;cb_return=1;
    mock_lock_event_hook=reparent_at_lock;
    task_signal_parent(&child,1ULL<<17);
    assert_eq(1ULL<<17,parent.signal);
    assert_eq(TASK_RUNNING,parent.state);
    task_wake_parent(&child);
    assert_eq(1,mock_enqueue_calls); /* second helper caller cannot double enqueue */
    assert_eq(0,munmap(guard_task,guard_size));
}

extern void task_exit_zombie(void);
static void resume_on_next_cpu(void)
{
    mock_irq_mask_hook=NULL;
    mock_sched_this_cpu=&percpu_data[1];
    mock_sched_current->cpu=1;
}
static void suite_exit_masks_before_cpu_lookup(void)
{
    TEST_SUITE("exit masks IRQs before choosing and caching the rq lock");
    sched_notify_reset();
    task_t self={.state=TASK_RUNNING,.cpu=0};
    mock_sched_current=&self;
    mock_irq_mask_hook=resume_on_next_cpu;
    task_exit_zombie();
    assert_eq(TASK_ZOMBIE,self.state);
    assert_eq(2,mock_lock_log_count);
    assert_true(mock_lock_log_addr[0] == &percpu_data[1].rq_lock);
    assert_true(mock_lock_log_addr[1] == mock_lock_log_addr[0]);
    assert_eq(0,mock_irq_state);
}

static void suite_blocker_preserves_entry_irq_state(void)
{
    TEST_SUITE("blocker_wait preserves caller entry IRQ mask across schedule");

    /* 1. Caller enters with IRQs enabled (mock_irq_state == 0). */
    sched_notify_reset();
    cb_calls = 0;
    cb_return = 0;
    task_t sleeper;
    memset(&sleeper, 0, sizeof(sleeper));
    sleeper.state = TASK_RUNNING;
    sleeper.pid = 21;
    sleeper.cpu = 0;
    sleeper.allowed_cpus = 1;
    sleeper.thread = &init_thread;
    mock_sched_current = &sleeper;
    mock_irq_state = 0;

    assert_eq(0, blocker_wait(check_cb, BLOCKER_WAITPID, false));
    assert_eq(0, (int)mock_irq_state);

    /* 2. Caller enters with IRQs masked (mock_irq_state == 1, e.g. AArch64 SVC). */
    sched_notify_reset();
    cb_calls = 0;
    cb_return = 0;
    memset(&sleeper, 0, sizeof(sleeper));
    sleeper.state = TASK_RUNNING;
    sleeper.pid = 22;
    sleeper.cpu = 0;
    sleeper.allowed_cpus = 1;
    sleeper.thread = &init_thread;
    mock_sched_current = &sleeper;
    mock_irq_state = 1;

    assert_eq(0, blocker_wait(check_cb, BLOCKER_WAITPID, false));
    assert_eq(1, (int)mock_irq_state);
}

int main(void)
{
    suite_already_true();
    suite_published_between();
    suite_explicit_wake();
    suite_wake_before_switch();
    suite_no_read_after_unlock();
    suite_signal_path();
    suite_ap_can_block();
    suite_signal_resume_callback();
    suite_migrated_snapshot();
    suite_guarded_lifetime();
    suite_parent_lifetime();
    suite_exit_masks_before_cpu_lookup();
    suite_blocker_preserves_entry_irq_state();
    printf("\n%s: %d total, %d passed, %d failed\n",
           "test_blocker_remote_wake",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
