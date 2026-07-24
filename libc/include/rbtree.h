#ifndef _RBTREE_H
#define _RBTREE_H

#include <stddef.h>

typedef struct rbtree_node {
    struct rbtree_node *left;
    struct rbtree_node *right;
    struct rbtree_node *parent;
    unsigned long color;         /* 0 = black, 1 = red */
} rbtree_node_t;

typedef struct rbtree_root {
    rbtree_node_t *rb_node;      /* NULL when tree is empty */
} rbtree_root_t;

static inline void rbtree_init(rbtree_root_t *root)
{
    root->rb_node = NULL;
}

static inline int rbtree_empty(rbtree_root_t *root)
{
    return root->rb_node == NULL;
}

void rbtree_node_init(rbtree_node_t *node);

/*
 * Insert a node into the red-black tree.
 * cmp(a, b) returns <0 if a goes left, >0 if a goes right, 0 if keys conflict.
 * Returns NULL on success, or the conflicting existing node if cmp returned 0.
 */
rbtree_node_t *rbtree_insert(rbtree_root_t *root, rbtree_node_t *node,
                             int (*cmp)(rbtree_node_t *a, rbtree_node_t *b));

/*
 * Erase a node from the red-black tree.
 * node MUST be currently in the tree.
 */
void rbtree_erase(rbtree_root_t *root, rbtree_node_t *node);

/* Return the leftmost (minimum) node, or NULL if tree is empty. */
rbtree_node_t *rbtree_first(rbtree_root_t *root);

/* Return the inorder successor, or NULL if node is the rightmost. */
rbtree_node_t *rbtree_next(rbtree_node_t *node);

#endif /* _RBTREE_H */
