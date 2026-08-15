/*
 * test_poll_requested.c — requested poll direction policy tests
 */

#include <test_framework.h>
#include <fcntl.h>
#include <kernel/poll.h>

typedef struct {
    int nent;
} model_poll_table_t;

static int model_register_requested(uint32_t requested,
                                    bool can_read, bool can_write,
                                    bool read_ready, bool write_ready,
                                    model_poll_table_t *pt)
{
    if (!pt)
        return 0;

    int before = pt->nent;

    if (can_read && poll_requested_read(requested) && !read_ready)
        pt->nent++;
    if (can_write && poll_requested_write(requested) && !write_ready)
        pt->nent++;

    return pt->nent - before;
}

static void model_cleanup(model_poll_table_t *pt)
{
    pt->nent = 0;
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
    model_poll_table_t pt = { 0 };

    /* Requested, legal, unavailable read direction registers. */
    assert_eq(1, model_register_requested(POLLIN, true, true,
                                          false, true, &pt));
    assert_eq(1, pt.nent);

    model_cleanup(&pt);
    assert_eq(0, pt.nent);

    /* A requested write direction that is already ready does not register. */
    assert_eq(0, model_register_requested(POLLOUT, true, true,
                                          false, true, &pt));
    assert_eq(0, pt.nent);

    /* O_RDONLY cannot register a write direction. */
    assert_eq(0, model_register_requested(POLLOUT, true, false,
                                          true, false, &pt));
    assert_eq(0, pt.nent);

    /* O_WRONLY cannot register a read direction. */
    assert_eq(0, model_register_requested(POLLIN, false, true,
                                          false, true, &pt));
    assert_eq(0, pt.nent);

    /* A null poll table never adds an entry. */
    assert_eq(0, model_register_requested(POLLIN, true, true,
                                          false, true, NULL));
    assert_eq(0, pt.nent);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_direction_policy),
    TEST_ENTRY(test_requested_registration_policy),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
