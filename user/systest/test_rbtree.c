#include "systest.h"

// Test node: embed rbtree_node_t in a small test struct
typedef struct test_rb_node {
    rbtree_node_t node;
    int key;
} test_rb_node_t;

static int test_cmp(rbtree_node_t *a, rbtree_node_t *b)
{
    test_rb_node_t *ta = (test_rb_node_t *)a;
    test_rb_node_t *tb = (test_rb_node_t *)b;
    if (ta->key < tb->key) return -1;
    if (ta->key > tb->key) return 1;
    return 0;
}


// ── rbtree unit tests ────────────────────────────────────
static int test_rbtree_insert_order(void)
{
    rbtree_root_t root;
    rbtree_init(&root);

    test_rb_node_t n[5];
    int keys[] = {30, 10, 50, 20, 40};
    for (int i = 0; i < 5; i++) {
        n[i].key = keys[i];
        rbtree_insert(&root, &n[i].node, test_cmp);
    }

    int prev = -1;
    for (rbtree_node_t *cur = rbtree_first(&root); cur; cur = rbtree_next(cur)) {
        test_rb_node_t *tn = (test_rb_node_t *)cur;
        if (tn->key <= prev) return 1;
        prev = tn->key;
    }
    return 0;
}

static int test_rbtree_erase_middle(void)
{
    rbtree_root_t root;
    rbtree_init(&root);

    test_rb_node_t n[3];
    n[0].key = 10; n[1].key = 20; n[2].key = 30;
    rbtree_insert(&root, &n[0].node, test_cmp);
    rbtree_insert(&root, &n[1].node, test_cmp);
    rbtree_insert(&root, &n[2].node, test_cmp);

    rbtree_erase(&root, &n[1].node);

    rbtree_node_t *first = rbtree_first(&root);
    if (((test_rb_node_t *)first)->key != 10) return 1;
    rbtree_node_t *second = rbtree_next(first);
    if (!second || ((test_rb_node_t *)second)->key != 30) return 1;
    if (rbtree_next(second) != NULL) return 1;
    return 0;
}

static int test_rbtree_stress_100(void)
{
    rbtree_root_t root;
    rbtree_init(&root);

    test_rb_node_t nodes[100];
    for (int i = 0; i < 100; i++) {
        nodes[i].key = (i * 73 + 17) % 1000;
        rbtree_insert(&root, &nodes[i].node, test_cmp);
    }

    int prev = -1, count = 0;
    for (rbtree_node_t *cur = rbtree_first(&root); cur; cur = rbtree_next(cur)) {
        test_rb_node_t *tn = (test_rb_node_t *)cur;
        if (tn->key < prev) return 1;
        prev = tn->key;
        count++;
    }
    if (count != 100) return 1;

    for (int i = 0; i < 100; i++) {
        int idx = (i * 47 + 23) % 100;
        rbtree_erase(&root, &nodes[idx].node);
    }
    if (!rbtree_empty(&root)) return 1;
    return 0;
}

static int test_rbtree_last(void)
{
    rbtree_root_t root;
    rbtree_init(&root);

    test_rb_node_t n[3];
    n[0].key = 10; n[1].key = 20; n[2].key = 30;
    for (int i = 0; i < 3; i++)
        rbtree_insert(&root, &n[i].node, test_cmp);

    rbtree_node_t *last = rbtree_last(&root);
    if (!last || ((test_rb_node_t *)last)->key != 30) return 1;
    return 0;
}

static int test_rbtree_prev_traversal(void)
{
    rbtree_root_t root;
    rbtree_init(&root);

    test_rb_node_t n[5];
    int keys[] = {30, 10, 50, 20, 40};
    for (int i = 0; i < 5; i++) {
        n[i].key = keys[i];
        rbtree_insert(&root, &n[i].node, test_cmp);
    }

    // Reverse traversal: should visit 50, 40, 30, 20, 10
    int expected[] = {50, 40, 30, 20, 10};
    int idx = 0;
    for (rbtree_node_t *cur = rbtree_last(&root); cur; cur = rbtree_prev(cur)) {
        test_rb_node_t *tn = (test_rb_node_t *)cur;
        if (idx >= 5 || tn->key != expected[idx]) return 1;
        idx++;
    }
    if (idx != 5) return 1;
    return 0;
}

static int test_rbtree_prev_null(void)
{
    rbtree_root_t root;
    rbtree_init(&root);

    test_rb_node_t n[2];
    n[0].key = 10; n[1].key = 20;
    rbtree_insert(&root, &n[0].node, test_cmp);
    rbtree_insert(&root, &n[1].node, test_cmp);

    // prev of first should be NULL
    rbtree_node_t *first = rbtree_first(&root);
    if (rbtree_prev(first) != NULL) return 1;

    // last of empty tree should be NULL
    rbtree_root_t empty;
    rbtree_init(&empty);
    if (rbtree_last(&empty) != NULL) return 1;

    return 0;
}

void test_wrap_rbtree_insert_order(void)
{ CHECK3(test_rbtree_insert_order() == 0, "rbtree_insert_order", "inorder traversal sorted"); }

void test_wrap_rbtree_erase_middle(void)
{ CHECK3(test_rbtree_erase_middle() == 0, "rbtree_erase_middle", "remaining nodes correct after erase"); }

void test_wrap_rbtree_stress_100(void)
{ CHECK3(test_rbtree_stress_100() == 0, "rbtree_stress_100", "100 insert/erase stress ok"); }

void test_wrap_rbtree_last(void)
{ CHECK3(test_rbtree_last() == 0, "rbtree_last", "rightmost is max key"); }

void test_wrap_rbtree_prev_traversal(void)
{ CHECK3(test_rbtree_prev_traversal() == 0, "rbtree_prev", "reverse inorder traversal"); }

void test_wrap_rbtree_prev_null(void)
{ CHECK3(test_rbtree_prev_null() == 0, "rbtree_prev_null", "prev of first is NULL, last of empty is NULL"); }

