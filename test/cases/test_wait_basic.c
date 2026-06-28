/* test_wait_basic.c — unit tests for wait_queue + completion
 *
 * Self-contained: copies type definitions from kernel headers
 * rather than including them (avoids kernel include chain).
 *
 * Tests the pure-logic paths: init, wake_one, wake_all, complete.
 * wait_queue_sleep / wait_for_completion require schedule(),
 * which runs in QEMU — those are integration-tested by booting.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Minimal mock types (match kernel struct layouts exactly) ── */

typedef struct List {
    struct List *next, *prev;
} list_t;

static inline void list_init(list_t *e)  { e->next = e; e->prev = e; }
static inline int  list_is_empty(list_t *e) { return e->next == e && e->prev == e; }

static inline void __list_add(list_t *n, list_t *p, list_t *nx)
{ nx->prev = n; n->next = nx; n->prev = p; p->next = n; }

static inline void list_add_to_before(list_t *h, list_t *n)
{ __list_add(n, h->prev, h); }

static inline void list_del_init(list_t *e)
{ e->prev->next = e->next; e->next->prev = e->prev; e->next = e; e->prev = e; }

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - __builtin_offsetof(type, member)))

/* spinlock_T + init + lock/unlock + irqsave/irqrestore:
   already provided by test_platform.h (force-included). */

#define TASK_RUNNING       (1 << 0)
#define TASK_INTERRUPTIBLE (1 << 1)

typedef struct task_struct {
    list_t      list;
    volatile int64_t state;
    uint64_t    flags;
    void       *mm, *thread;
    uint64_t    addr_limit;
    int64_t     pid, counter, signal, priority;
    uint32_t    cpu;
    void       *stack_alloc_base;
    void       *files, *parent;
    int64_t     exit_code;
    list_t      wait_list;
    list_t      io_wait_node;
    void       *sighand;
    void       *fpu_save;
} task_t;

/* ── Copied from kernel/include/kernel/wait.h ─────────────── */

typedef struct {
    list_t      head;
    spinlock_T  lock;
} wait_queue_t;

/* ── Copied from kernel/intr/wait.c (pure logic only) ─────── */

static void wait_queue_init(wait_queue_t *wq)
{
    list_init(&wq->head);
    spin_init(&wq->lock);
}

static void wait_queue_wake_one(wait_queue_t *wq)
{
    uint64_t flags = spin_lock_irqsave(&wq->lock);
    if (!list_is_empty(&wq->head)) {
        list_t *node = wq->head.next;
        list_del_init(node);
        task_t *t = container_of(node, task_t, io_wait_node);
        t->state = TASK_RUNNING;
    }
    spin_unlock_irqrestore(&wq->lock, flags);
}

static void wait_queue_wake_all(wait_queue_t *wq)
{
    uint64_t flags = spin_lock_irqsave(&wq->lock);
    while (!list_is_empty(&wq->head)) {
        list_t *node = wq->head.next;
        list_del_init(node);
        task_t *t = container_of(node, task_t, io_wait_node);
        t->state = TASK_RUNNING;
    }
    spin_unlock_irqrestore(&wq->lock, flags);
}

/* ── Copied from kernel/include/kernel/completion.h ────────── */

typedef struct {
    volatile int done;
    wait_queue_t wq;
} completion_t;

/* ── Copied from kernel/completion.c (pure logic only) ─────── */

static void completion_init(completion_t *c)
{
    c->done = 0;
    wait_queue_init(&c->wq);
}

static void complete(completion_t *c)
{
    __sync_fetch_and_add(&c->done, 1);
    wait_queue_wake_one(&c->wq);
}

/* ═══════════════════════════════════════════════════════════
 *  Test framework
 * ═══════════════════════════════════════════════════════════ */

static int tests_run  = 0;
static int tests_pass = 0;
static int tests_fail = 0;
static const char *test_name;
static int failed;

#define TEST(name) do { tests_run++; test_name = name; failed = 0; } while(0)
#define CHECK(cond) do { if (!(cond)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failed = 1; } } while(0)
#define CHECK_EQ(a, b) do { int64_t _a=(int64_t)(a), _b=(int64_t)(b); if (_a!=_b) { printf("  FAIL %s:%d: %s == %s  (%ld != %ld)\n", __FILE__, __LINE__, #a, #b, (long)_a, (long)_b); failed = 1; } } while(0)
#define END_TEST() do { if (failed) tests_fail++; else tests_pass++; } while(0)

/* ═══════════════════════════════════════════════════════════
 *  Tests
 * ═══════════════════════════════════════════════════════════ */

static void test_wq_init(void)
{
    TEST("wait_queue_init sets up empty list + unlocked lock");
    wait_queue_t wq;
    wait_queue_init(&wq);
    CHECK(list_is_empty(&wq.head));
    CHECK(wq.lock.lock == 1);
    END_TEST();
}

static void test_wq_wake_one_empty(void)
{
    TEST("wait_queue_wake_one on empty queue is safe no-op");
    wait_queue_t wq;
    wait_queue_init(&wq);
    wait_queue_wake_one(&wq);
    CHECK(list_is_empty(&wq.head));
    END_TEST();
}

static void test_wq_wake_one_fifo(void)
{
    TEST("wait_queue_wake_one wakes exactly one waiter in FIFO order");
    wait_queue_t wq;
    wait_queue_init(&wq);

    task_t t1, t2;
    memset(&t1, 0, sizeof(t1)); list_init(&t1.io_wait_node);
    memset(&t2, 0, sizeof(t2)); list_init(&t2.io_wait_node);
    t1.state = TASK_INTERRUPTIBLE;
    t2.state = TASK_INTERRUPTIBLE;

    list_add_to_before(&wq.head, &t1.io_wait_node); // t1 enqueued first
    list_add_to_before(&wq.head, &t2.io_wait_node); // t2 enqueued second

    // head → t1 → t2 → head
    wait_queue_wake_one(&wq);
    CHECK_EQ(t1.state, TASK_RUNNING);       // t1 was first in
    CHECK_EQ(t2.state, TASK_INTERRUPTIBLE); // t2 still asleep
    CHECK(!list_is_empty(&wq.head));

    wait_queue_wake_one(&wq);
    CHECK_EQ(t2.state, TASK_RUNNING);       // t2 now woken
    CHECK(list_is_empty(&wq.head));
    END_TEST();
}

static void test_wq_wake_all(void)
{
    TEST("wait_queue_wake_all wakes every waiter and empties queue");
    wait_queue_t wq;
    wait_queue_init(&wq);

    task_t tasks[3];
    for (int i = 0; i < 3; i++) {
        memset(&tasks[i], 0, sizeof(task_t));
        list_init(&tasks[i].io_wait_node);
        tasks[i].state = TASK_INTERRUPTIBLE;
        list_add_to_before(&wq.head, &tasks[i].io_wait_node);
    }

    wait_queue_wake_all(&wq);
    for (int i = 0; i < 3; i++)
        CHECK_EQ(tasks[i].state, TASK_RUNNING);
    CHECK(list_is_empty(&wq.head));
    END_TEST();
}

static void test_wq_wake_all_no_waiters(void)
{
    TEST("wait_queue_wake_all on empty queue is safe no-op");
    wait_queue_t wq;
    wait_queue_init(&wq);
    wait_queue_wake_all(&wq);
    CHECK(list_is_empty(&wq.head));
    END_TEST();
}

static void test_completion_init(void)
{
    TEST("completion_init: done=0, wq empty");
    completion_t c;
    completion_init(&c);
    CHECK_EQ(c.done, 0);
    CHECK(list_is_empty(&c.wq.head));
    END_TEST();
}

static void test_complete_no_waiter(void)
{
    TEST("complete without waiter increments done counter");
    completion_t c;
    completion_init(&c);
    complete(&c);
    CHECK_EQ(c.done, 1);
    complete(&c);
    CHECK_EQ(c.done, 2);
    complete(&c);
    CHECK_EQ(c.done, 3);
    END_TEST();
}

static void test_complete_wakes_waiter(void)
{
    TEST("complete increments done and wakes one waiter");
    completion_t c;
    completion_init(&c);

    // Enqueue a "sleeping" task directly on the completion wq
    task_t t;
    memset(&t, 0, sizeof(t));
    list_init(&t.io_wait_node);
    t.state = TASK_INTERRUPTIBLE;
    list_add_to_before(&c.wq.head, &t.io_wait_node);

    complete(&c);
    CHECK_EQ(c.done, 1);
    CHECK_EQ(t.state, TASK_RUNNING);
    CHECK(list_is_empty(&c.wq.head));
    END_TEST();
}

static void test_complete_wakes_only_one(void)
{
    TEST("complete wakes exactly one waiter, leaves others");
    completion_t c;
    completion_init(&c);

    task_t t1, t2;
    memset(&t1, 0, sizeof(t1)); list_init(&t1.io_wait_node);
    memset(&t2, 0, sizeof(t2)); list_init(&t2.io_wait_node);
    t1.state = TASK_INTERRUPTIBLE;
    t2.state = TASK_INTERRUPTIBLE;
    list_add_to_before(&c.wq.head, &t1.io_wait_node);
    list_add_to_before(&c.wq.head, &t2.io_wait_node);

    complete(&c);
    CHECK_EQ(c.done, 1);
    CHECK_EQ(t1.state, TASK_RUNNING);       // only first waiter woken
    CHECK_EQ(t2.state, TASK_INTERRUPTIBLE); // still asleep
    CHECK(!list_is_empty(&c.wq.head));
    END_TEST();
}

/* ═══════════════════════════════════════════════════════════
 *  Runner
 * ═══════════════════════════════════════════════════════════ */

int main(void)
{
    printf("=== Test Runner: wait_queue + completion ===\n");

    test_wq_init();
    test_wq_wake_one_empty();
    test_wq_wake_one_fifo();
    test_wq_wake_all();
    test_wq_wake_all_no_waiters();
    test_completion_init();
    test_complete_no_waiter();
    test_complete_wakes_waiter();
    test_complete_wakes_only_one();

    printf("  Total: %d | Passed: %d | Failed: %d\n",
           tests_run, tests_pass, tests_fail);
    if (tests_fail == 0)
        printf("  >>> ALL TESTS PASSED <<<\n");
    return tests_fail ? 1 : 0;
}
