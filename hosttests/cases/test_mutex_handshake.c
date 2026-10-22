#include "test_framework.h"
#include <sync/mutex.h>
#include <setjmp.h>
#include <errno.h>

int mutex_test_irq_enabled;
task_t mutex_test_task;
static mutex_t lock;
static task_t next_waiter;
static int hook, schedules;
static jmp_buf unexpected_sleep;

void task_wake(task_t *t) { t->state = TASK_RUNNING; }
void __real_wait_queue_arm(wait_queue_t *wq);
void __real_wait_queue_sleep(wait_queue_t *wq);

// Fire the producer precisely in the failed-CAS -> enqueue window.
static void before_arm(void) {
    if (hook == 1) { hook = 0; mutex_unlock(&lock); }
}
void __wrap_wait_queue_arm(wait_queue_t *wq) {
    before_arm();
    __real_wait_queue_arm(wq);
    if (hook == 2) { hook = 0; mutex_unlock(&lock); }
    if (hook == 3) { hook = 0; current->signal = 1; task_wake(current); }
    if (hook == 4) {
        next_waiter = (task_t){.pid = 3, .state = TASK_INTERRUPTIBLE};
        list_init(&next_waiter.io_wait_node);
        list_add_to_before(&wq->head, &next_waiter.io_wait_node);
    }
}
void __wrap_wait_queue_sleep(wait_queue_t *wq) {
    before_arm();
    __real_wait_queue_sleep(wq);
}
void schedule(void) {
    schedules++;
    assert_eq(0, mutex_test_irq_enabled);
    // There can be no future wake once the only producer has unlocked.
    if (!lock.owner && current->state == TASK_INTERRUPTIBLE)
        longjmp(unexpected_sleep, 1);
    if (lock.owner) mutex_unlock(&lock);
    if (hook == 4) { hook = 0; current->signal = 1; }
}
static void reset(int mode) {
    mutex_test_irq_enabled = 1;
    mutex_test_task = (task_t){.pid = 2, .state = TASK_RUNNING};
    list_init(&current->io_wait_node);
    mutex_init(&lock);
    lock.owner = 1;
    hook = mode; schedules = 0;
}
static void run_gap(bool interruptible) {
    reset(1);
    int slept_forever = setjmp(unexpected_sleep);
    if (!slept_forever) {
        if (interruptible) assert_eq(0, mutex_lock_interruptible(&lock));
        else mutex_lock(&lock);
    }
    assert_eq(0, slept_forever);
    assert_eq(2, lock.owner);
    assert_eq(TASK_RUNNING, current->state);
    assert_true(list_is_empty(&lock.wq.head));
    assert_true(list_is_empty(&current->io_wait_node));
    assert_eq(1, mutex_test_irq_enabled);
}
TEST_FUNC(unlock_before_enqueue_is_not_lost) { run_gap(false); }
TEST_FUNC(interruptible_unlock_before_enqueue_is_not_lost) { run_gap(true); }
TEST_FUNC(wake_after_enqueue_is_not_overwritten) {
    reset(2); mutex_lock(&lock);
    assert_eq(2, lock.owner);
    assert_eq(TASK_RUNNING, current->state);
    assert_true(list_is_empty(&lock.wq.head));
}
TEST_FUNC(contended_lock_waits_then_acquires) {
    reset(0); mutex_lock(&lock);
    assert_eq(1, schedules);
    assert_eq(2, lock.owner);
    assert_true(list_is_empty(&lock.wq.head));
    assert_eq(1, mutex_test_irq_enabled);
}
TEST_FUNC(signal_after_arm_returns_eintr_without_residue) {
    reset(3);
    assert_eq(-EINTR, mutex_lock_interruptible(&lock));
    assert_eq(0, schedules);
    assert_eq(1, lock.owner);
    assert_eq(TASK_RUNNING, current->state);
    assert_true(list_is_empty(&lock.wq.head));
}
TEST_FUNC(pending_signal_returns_eintr_without_sleep) {
    reset(0); current->signal = 1;
    assert_eq(-EINTR, mutex_lock_interruptible(&lock));
    assert_eq(0, schedules);
    assert_true(list_is_empty(&lock.wq.head));
}
TEST_FUNC(cancelled_waiter_does_not_consume_next_waiters_wake) {
    reset(4);
    assert_eq(-EINTR, mutex_lock_interruptible(&lock));
    assert_eq(0, lock.owner);
    assert_eq(TASK_RUNNING, next_waiter.state);
    assert_true(list_is_empty(&lock.wq.head));
}
TEST_FUNC(contended_lock_preserves_disabled_irqs) {
    reset(0);
    mutex_test_irq_enabled = 0;
    mutex_lock(&lock);
    assert_eq(2, lock.owner);
    assert_eq(0, mutex_test_irq_enabled);
    assert_true(list_is_empty(&lock.wq.head));
}
TEST_LIST_BEGIN
    TEST_ENTRY(unlock_before_enqueue_is_not_lost),
    TEST_ENTRY(interruptible_unlock_before_enqueue_is_not_lost),
    TEST_ENTRY(wake_after_enqueue_is_not_overwritten),
    TEST_ENTRY(contended_lock_waits_then_acquires),
    TEST_ENTRY(signal_after_arm_returns_eintr_without_residue),
    TEST_ENTRY(pending_signal_returns_eintr_without_sleep),
    TEST_ENTRY(cancelled_waiter_does_not_consume_next_waiters_wake),
    TEST_ENTRY(contended_lock_preserves_disabled_irqs),
TEST_LIST_END
int main(void) {
    TEST_SUITE("production mutex/waitqueue handshake");
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}
