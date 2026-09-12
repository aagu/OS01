/*
 * test/cases/test_canon.c — canonical line discipline unit tests.
 *
 * Compiles the REAL kernel/tty/canon.c (pure logic — no kernel deps).
 * Exercises: accumulate-from-ring, complete-line extraction,
 * remainder buffering, short user buffers, capacity.
 */
#include "test_framework.h"
#include <kernel/canon.h>
#include <string.h>
#include <stdlib.h>

#define RING_SZ 16

/* Simulated input ring (like tty ring: producer head, consumer tail) */
static void push(char *ring, int *head, char c)
{
    ring[*head] = c;
    *head = (*head + 1) % RING_SZ;
}

TEST_FUNC(test_no_line_no_read) {
    char ring[RING_SZ] = {0};
    int head = 0, tail = 0;
    canon_buf_t cb = {{0}, 0};

    push(ring, &head, 'a');
    push(ring, &head, 'b');
    canon_accumulate(&cb, ring, &head, &tail, RING_SZ);
    assert_eq(2, cb.len);

    char buf[16];
    assert_eq(0, canon_read(&cb, buf, sizeof(buf)));
    assert_eq(2, cb.len);   /* incomplete line stays buffered */
}

TEST_FUNC(test_complete_line) {
    char ring[RING_SZ] = {0};
    int head = 0, tail = 0;
    canon_buf_t cb = {{0}, 0};

    push(ring, &head, 'h');
    push(ring, &head, 'i');
    push(ring, &head, '\n');
    canon_accumulate(&cb, ring, &head, &tail, RING_SZ);

    char buf[16];
    assert_eq(3, canon_read(&cb, buf, sizeof(buf)));
    assert_mem_eq("hi\n", buf, 3);
    assert_eq(0, cb.len);   /* drained */
}

TEST_FUNC(test_multiline_remainder) {
    char ring[RING_SZ] = {0};
    int head = 0, tail = 0;
    canon_buf_t cb = {{0}, 0};

    push(ring, &head, 'a');
    push(ring, &head, '\n');
    push(ring, &head, 'b');          /* second line partial */
    canon_accumulate(&cb, ring, &head, &tail, RING_SZ);

    char buf[16];
    assert_eq(2, canon_read(&cb, buf, sizeof(buf)));
    assert_mem_eq("a\n", buf, 2);
    assert_eq(2, tail);              /* 'b' still in the ring, not consumed */

    assert_eq(0, canon_read(&cb, buf, sizeof(buf)));  /* canon empty — nothing */

    push(ring, &head, '\n');         /* complete the second line */
    canon_accumulate(&cb, ring, &head, &tail, RING_SZ);
    assert_eq(2, canon_read(&cb, buf, sizeof(buf)));
    assert_mem_eq("b\n", buf, 2);
}

TEST_FUNC(test_short_user_buffer) {
    char ring[RING_SZ] = {0};
    int head = 0, tail = 0;
    canon_buf_t cb = {{0}, 0};

    push(ring, &head, 'h');
    push(ring, &head, 'e');
    push(ring, &head, 'l');
    push(ring, &head, 'l');
    push(ring, &head, 'o');
    push(ring, &head, '\n');
    canon_accumulate(&cb, ring, &head, &tail, RING_SZ);

    char buf[3];
    assert_eq(3, canon_read(&cb, buf, sizeof(buf)));
    assert_mem_eq("hel", buf, 3);
    assert_eq(3, cb.len);            /* remainder kept */

    assert_eq(3, canon_read(&cb, buf, sizeof(buf)));
    assert_mem_eq("lo\n", buf, 3);
    assert_eq(0, cb.len);            /* drained */
}

TEST_FUNC(test_canon_buf_full) {
    /* ring must be larger than the canon buffer to fill it */
    #define BIG_RING (CANON_BUF_SIZE * 2)
    char ring[BIG_RING] = {0};
    int head = 0, tail = 0;
    canon_buf_t cb = {{0}, 0};

    for (int i = 0; i < CANON_BUF_SIZE + 10; i++) {
        ring[head] = 'x';              /* no newline — fills canon buffer */
        head = (head + 1) % BIG_RING;
    }
    canon_accumulate(&cb, ring, &head, &tail, BIG_RING);
    assert_eq(CANON_BUF_SIZE, cb.len);
    #undef BIG_RING
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_no_line_no_read),
    TEST_ENTRY(test_complete_line),
    TEST_ENTRY(test_multiline_remainder),
    TEST_ENTRY(test_short_user_buffer),
    TEST_ENTRY(test_canon_buf_full),
TEST_LIST_END

int main() {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
