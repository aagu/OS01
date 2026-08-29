#include "test_framework.h"
#include <kernel/rwlock.h>
#include <kernel/seqlock.h>
#include <pthread.h>
#include <sched.h>

static rwlock_t contention_lock;
static pthread_mutex_t control_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t control_changed = PTHREAD_COND_INITIALIZER;
static int writer_count;
static int writer_order[2];
static int writer_release[2];

static void reset_writer_control(void)
{
    pthread_mutex_lock(&control_lock);
    writer_count = 0;
    writer_release[0] = 0;
    writer_release[1] = 0;
    pthread_mutex_unlock(&control_lock);
}

static void wait_for_writers(int expected)
{
    pthread_mutex_lock(&control_lock);
    while (writer_count < expected)
        pthread_cond_wait(&control_changed, &control_lock);
    pthread_mutex_unlock(&control_lock);
}

static int acquired_writer(int order)
{
    int index;
    pthread_mutex_lock(&control_lock);
    index = writer_order[order];
    pthread_mutex_unlock(&control_lock);
    return index;
}

static int current_writer_count(void)
{
    int count;
    pthread_mutex_lock(&control_lock);
    count = writer_count;
    pthread_mutex_unlock(&control_lock);
    return count;
}

static int queued_writer_count(void)
{
    uint64_t state = __atomic_load_n(&contention_lock.state, __ATOMIC_ACQUIRE);
    return (int)((state >> 32) & 0x7fffffffU);
}

static void release_writer(int index)
{
    pthread_mutex_lock(&control_lock);
    writer_release[index] = 1;
    pthread_cond_broadcast(&control_changed);
    pthread_mutex_unlock(&control_lock);
}

static void *contending_writer(void *arg)
{
    int index = (int)(uintptr_t)arg;
    rwlock_write_lock(&contention_lock);
    pthread_mutex_lock(&control_lock);
    writer_order[writer_count++] = index;
    pthread_cond_broadcast(&control_changed);
    while (!writer_release[index])
        pthread_cond_wait(&control_changed, &control_lock);
    pthread_mutex_unlock(&control_lock);
    rwlock_write_unlock(&contention_lock);
    return NULL;
}

TEST_FUNC(test_rwlock_excludes_conflicting_holder)
{
    rwlock_t lock;
    rwlock_init(&lock);

    assert_true(rwlock_try_read_lock(&lock));
    assert_false(rwlock_try_write_lock(&lock));
    rwlock_read_unlock(&lock);

    assert_true(rwlock_try_write_lock(&lock));
    assert_false(rwlock_try_read_lock(&lock));
    rwlock_write_unlock(&lock);
}

TEST_FUNC(test_seqlock_retries_only_after_a_write)
{
    seqlock_t lock;
    seqlock_init(&lock);

    uint64_t sequence = seqlock_read_begin(&lock);
    assert_false(seqlock_read_retry(&lock, sequence));

    seqlock_write_lock(&lock);
    seqlock_write_unlock(&lock);

    assert_true(seqlock_read_retry(&lock, sequence));
    sequence = seqlock_read_begin(&lock);
    assert_false(seqlock_read_retry(&lock, sequence));
}

TEST_FUNC(test_pending_writer_blocks_new_readers)
{
    pthread_t writers[2];
    rwlock_init(&contention_lock);
    reset_writer_control();

    rwlock_read_lock(&contention_lock);
    assert_eq(0, pthread_create(&writers[0], NULL, contending_writer,
                                (void *)(uintptr_t)0));
    assert_eq(0, pthread_create(&writers[1], NULL, contending_writer,
                                (void *)(uintptr_t)1));

    for (int i = 0; i < 1000000 && queued_writer_count() < 2; i++)
        sched_yield();
    assert_eq(2, queued_writer_count());

    int reader_blocked = 0;
    for (int i = 0; i < 1000000; i++) {
        if (!rwlock_try_read_lock(&contention_lock)) {
            reader_blocked = 1;
            break;
        }
        rwlock_read_unlock(&contention_lock);
        sched_yield();
    }
    assert_true(reader_blocked);

    rwlock_read_unlock(&contention_lock);
    wait_for_writers(1);
    assert_false(rwlock_try_read_lock(&contention_lock));

    release_writer(acquired_writer(0));
    int reader_barged = 0;
    while (current_writer_count() < 2) {
        if (rwlock_try_read_lock(&contention_lock)) {
            reader_barged = 1;
            rwlock_read_unlock(&contention_lock);
            break;
        }
        sched_yield();
    }
    assert_false(reader_barged);
    wait_for_writers(2);
    assert_false(rwlock_try_read_lock(&contention_lock));

    release_writer(acquired_writer(1));
    assert_eq(0, pthread_join(writers[0], NULL));
    assert_eq(0, pthread_join(writers[1], NULL));
    assert_true(rwlock_try_read_lock(&contention_lock));
    rwlock_read_unlock(&contention_lock);
}

static seqlock_t seqlock_contention;
static int seqlock_writer_count;
static int seqlock_writer_started;
static int seqlock_writer_order[2];
static int seqlock_writer_release[2];

static void *contending_seqlock_writer(void *arg)
{
    int index = (int)(uintptr_t)arg;
    pthread_mutex_lock(&control_lock);
    seqlock_writer_started++;
    pthread_cond_broadcast(&control_changed);
    pthread_mutex_unlock(&control_lock);
    seqlock_write_lock(&seqlock_contention);
    pthread_mutex_lock(&control_lock);
    seqlock_writer_order[seqlock_writer_count++] = index;
    pthread_cond_broadcast(&control_changed);
    while (!seqlock_writer_release[index])
        pthread_cond_wait(&control_changed, &control_lock);
    pthread_mutex_unlock(&control_lock);
    seqlock_write_unlock(&seqlock_contention);
    return NULL;
}

TEST_FUNC(test_seqlock_writer_is_exclusive_and_invalidates_reader)
{
    pthread_t writers[2];
    seqlock_init(&seqlock_contention);
    pthread_mutex_lock(&control_lock);
    seqlock_writer_count = 0;
    seqlock_writer_started = 0;
    seqlock_writer_release[0] = 0;
    seqlock_writer_release[1] = 0;
    pthread_mutex_unlock(&control_lock);

    uint64_t sequence = seqlock_read_begin(&seqlock_contention);
    assert_eq(0, pthread_create(&writers[0], NULL, contending_seqlock_writer,
                                (void *)(uintptr_t)0));
    assert_eq(0, pthread_create(&writers[1], NULL, contending_seqlock_writer,
                                (void *)(uintptr_t)1));

    pthread_mutex_lock(&control_lock);
    while (seqlock_writer_started < 2)
        pthread_cond_wait(&control_changed, &control_lock);
    while (seqlock_writer_count < 1)
        pthread_cond_wait(&control_changed, &control_lock);
    assert_eq(1, seqlock_writer_count);
    pthread_mutex_unlock(&control_lock);
    assert_true(seqlock_read_retry(&seqlock_contention, sequence));

    pthread_mutex_lock(&control_lock);
    seqlock_writer_release[seqlock_writer_order[0]] = 1;
    pthread_cond_broadcast(&control_changed);
    while (seqlock_writer_count < 2)
        pthread_cond_wait(&control_changed, &control_lock);
    seqlock_writer_release[seqlock_writer_order[1]] = 1;
    pthread_cond_broadcast(&control_changed);
    pthread_mutex_unlock(&control_lock);

    assert_eq(0, pthread_join(writers[0], NULL));
    assert_eq(0, pthread_join(writers[1], NULL));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_rwlock_excludes_conflicting_holder),
    TEST_ENTRY(test_seqlock_retries_only_after_a_write),
    TEST_ENTRY(test_pending_writer_blocks_new_readers),
    TEST_ENTRY(test_seqlock_writer_is_exclusive_and_invalidates_reader),
TEST_LIST_END

int main(void)
{
    TEST_SUITE("rwlock/seqlock primitives");
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}
