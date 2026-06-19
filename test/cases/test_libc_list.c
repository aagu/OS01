/*
 * test/cases/test_libc_list.c — Doubly-linked list tests
 */
#include "test_framework.h"
#include <list.h>
#include <stddef.h>

/* Test structure embedded in list */
typedef struct {
    list_t list;
    int value;
} test_item_t;

TEST_FUNC(test_list_init_empty) {
    list_t head;
    list_init(&head);
    assert_true(list_is_empty(&head));
    assert_eq(head.next, &head);
    assert_eq(head.prev, &head);
}

TEST_FUNC(test_list_add_behind) {
    list_t head;
    list_init(&head);
    test_item_t a, b;
    list_init(&a.list);
    list_init(&b.list);

    list_add_to_behind(&head, &a.list);
    assert_false(list_is_empty(&head));
    assert_eq(head.next, &a.list);
    assert_eq(head.prev, &a.list);

    list_add_to_behind(&a.list, &b.list);
    // order: head → a → b → head
    assert_eq(head.next, &a.list);
    assert_eq(a.list.next, &b.list);
    assert_eq(b.list.next, &head);
    assert_eq(b.list.prev, &a.list);
}

TEST_FUNC(test_list_add_before) {
    list_t head;
    list_init(&head);
    test_item_t a, b;
    list_init(&a.list);
    list_init(&b.list);

    list_add_to_before(&head, &a.list);
    assert_eq(head.prev, &a.list);

    list_add_to_before(&a.list, &b.list);
    // order: head → b → a → head
    assert_eq(head.next, &b.list);
    assert_eq(b.list.next, &a.list);
    assert_eq(a.list.next, &head);
}

TEST_FUNC(test_list_del) {
    list_t head;
    list_init(&head);
    test_item_t a, b;
    list_init(&a.list);
    list_init(&b.list);

    list_add_to_behind(&head, &a.list);
    list_add_to_behind(&a.list, &b.list);

    // remove a
    list_del(&a.list);
    assert_eq(head.next, &b.list);
    assert_eq(b.list.prev, &head);
    assert_eq(b.list.next, &head);
    assert_eq(head.prev, &b.list);
}

TEST_FUNC(test_list_next_prev) {
    list_t head;
    list_init(&head);
    test_item_t a, b;
    list_init(&a.list);
    list_init(&b.list);

    list_add_to_behind(&head, &a.list);
    list_add_to_behind(&a.list, &b.list);

    assert_eq(list_next(&head), &a.list);
    assert_eq(list_next(&a.list), &b.list);
    assert_eq(list_next(&b.list), &head);

    assert_eq(list_prev(&head), &b.list);
    assert_eq(list_prev(&b.list), &a.list);
    assert_eq(list_prev(&a.list), &head);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_list_init_empty),
    TEST_ENTRY(test_list_add_behind),
    TEST_ENTRY(test_list_add_before),
    TEST_ENTRY(test_list_del),
    TEST_ENTRY(test_list_next_prev),
TEST_LIST_END

int main() {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
