#include <rbtree.h>

/* ── Internal helpers ─────────────────────────────────── */

#define RB_RED    1
#define RB_BLACK  0

static inline int is_red(rbtree_node_t *n)
{
    return n && n->color == RB_RED;
}

static inline void set_black(rbtree_node_t *n)
{
    if (n) n->color = RB_BLACK;
}

static inline void set_red(rbtree_node_t *n)
{
    if (n) n->color = RB_RED;
}

void rbtree_node_init(rbtree_node_t *node)
{
    node->left = NULL;
    node->right = NULL;
    node->parent = NULL;
    node->color = RB_BLACK;
}

/* ── Rotation helpers ─────────────────────────────────── */

static void rotate_left(rbtree_node_t *node, rbtree_root_t *root)
{
    rbtree_node_t *right = node->right;
    rbtree_node_t *parent = node->parent;

    node->right = right->left;
    if (right->left)
        right->left->parent = node;
    right->parent = parent;

    if (!parent)
        root->rb_node = right;
    else if (node == parent->left)
        parent->left = right;
    else
        parent->right = right;

    right->left = node;
    node->parent = right;
}

static void rotate_right(rbtree_node_t *node, rbtree_root_t *root)
{
    rbtree_node_t *left = node->left;
    rbtree_node_t *parent = node->parent;

    node->left = left->right;
    if (left->right)
        left->right->parent = node;
    left->parent = parent;

    if (!parent)
        root->rb_node = left;
    else if (node == parent->right)
        parent->right = left;
    else
        parent->left = left;

    left->right = node;
    node->parent = left;
}

/* ── Insert ───────────────────────────────────────────── */

rbtree_node_t *rbtree_insert(rbtree_root_t *root, rbtree_node_t *node,
                             int (*cmp)(rbtree_node_t *a, rbtree_node_t *b))
{
    rbtree_node_t **link = &root->rb_node;
    rbtree_node_t *parent = NULL;

    while (*link) {
        parent = *link;
        int c = cmp(node, parent);
        if (c < 0)
            link = &parent->left;
        else if (c > 0)
            link = &parent->right;
        else
            return parent;  /* key conflict */
    }

    node->left = NULL;
    node->right = NULL;
    node->parent = parent;
    node->color = RB_RED;
    *link = node;

    /* Fix red-red violations after insertion */
    rbtree_node_t *n = node;
    while (n->parent && n->parent->color == RB_RED) {
        rbtree_node_t *gp = n->parent->parent;
        if (n->parent == gp->left) {
            rbtree_node_t *uncle = gp->right;
            if (is_red(uncle)) {
                set_black(n->parent);
                set_black(uncle);
                set_red(gp);
                n = gp;
            } else {
                if (n == n->parent->right) {
                    n = n->parent;
                    rotate_left(n, root);
                }
                set_black(n->parent);
                set_red(gp);
                rotate_right(gp, root);
            }
        } else {
            rbtree_node_t *uncle = gp->left;
            if (is_red(uncle)) {
                set_black(n->parent);
                set_black(uncle);
                set_red(gp);
                n = gp;
            } else {
                if (n == n->parent->left) {
                    n = n->parent;
                    rotate_right(n, root);
                }
                set_black(n->parent);
                set_red(gp);
                rotate_left(gp, root);
            }
        }
    }
    set_black(root->rb_node);
    return NULL;
}

/* ── Tree minimum ─────────────────────────────────────── */

rbtree_node_t *rbtree_first(rbtree_root_t *root)
{
    rbtree_node_t *n = root->rb_node;
    if (!n) return NULL;
    while (n->left)
        n = n->left;
    return n;
}

/* ── Inorder successor ────────────────────────────────── */

rbtree_node_t *rbtree_next(rbtree_node_t *node)
{
    if (node->right) {
        node = node->right;
        while (node->left)
            node = node->left;
        return node;
    }
    rbtree_node_t *p = node->parent;
    while (p && node == p->right) {
        node = p;
        p = p->parent;
    }
    return p;
}

/* ── Tree maximum ──────────────────────────────────────── */

rbtree_node_t *rbtree_last(rbtree_root_t *root)
{
    rbtree_node_t *n = root->rb_node;
    if (!n) return NULL;
    while (n->right)
        n = n->right;
    return n;
}

/* ── Inorder predecessor ───────────────────────────────── */

rbtree_node_t *rbtree_prev(rbtree_node_t *node)
{
    if (node->left) {
        node = node->left;
        while (node->right)
            node = node->right;
        return node;
    }
    rbtree_node_t *p = node->parent;
    while (p && node == p->left) {
        node = p;
        p = p->parent;
    }
    return p;
}

/* ── Erase ────────────────────────────────────────────── */

static void rbtree_erase_fixup(rbtree_node_t *node, rbtree_node_t *parent,
                               rbtree_root_t *root)
{
    rbtree_node_t *n = node;
    rbtree_node_t *p = parent;

    while ((!n || n->color == RB_BLACK) && n != root->rb_node) {
        if (n == p->left) {
            rbtree_node_t *sibling = p->right;
            if (is_red(sibling)) {
                set_black(sibling);
                set_red(p);
                rotate_left(p, root);
                sibling = p->right;
            }
            if ((!sibling->left || sibling->left->color == RB_BLACK) &&
                (!sibling->right || sibling->right->color == RB_BLACK)) {
                set_red(sibling);
                n = p;
                p = p->parent;
            } else {
                if (!sibling->right || sibling->right->color == RB_BLACK) {
                    set_black(sibling->left);
                    set_red(sibling);
                    rotate_right(sibling, root);
                    sibling = p->right;
                }
                sibling->color = p->color;
                set_black(p);
                set_black(sibling->right);
                rotate_left(p, root);
                n = root->rb_node;
                break;
            }
        } else {
            rbtree_node_t *sibling = p->left;
            if (is_red(sibling)) {
                set_black(sibling);
                set_red(p);
                rotate_right(p, root);
                sibling = p->left;
            }
            if ((!sibling->right || sibling->right->color == RB_BLACK) &&
                (!sibling->left || sibling->left->color == RB_BLACK)) {
                set_red(sibling);
                n = p;
                p = p->parent;
            } else {
                if (!sibling->left || sibling->left->color == RB_BLACK) {
                    set_black(sibling->right);
                    set_red(sibling);
                    rotate_left(sibling, root);
                    sibling = p->left;
                }
                sibling->color = p->color;
                set_black(p);
                set_black(sibling->left);
                rotate_right(p, root);
                n = root->rb_node;
                break;
            }
        }
    }
    if (n) set_black(n);
}

void rbtree_erase(rbtree_root_t *root, rbtree_node_t *node)
{
    rbtree_node_t *child, *parent;
    int color;

    if (node->left && node->right) {
        /* Find inorder successor and swap payload */
        rbtree_node_t *succ = node->right;
        while (succ->left)
            succ = succ->left;

        /* Detach succ from its current position */
        if (succ->parent != node) {
            child = succ->right;
            parent = succ->parent;
            color = succ->color;
            if (parent->left == succ)
                parent->left = succ->right;
            else
                parent->right = succ->right;
            if (succ->right)
                succ->right->parent = parent;

            succ->left = node->left;
            node->left->parent = succ;
            succ->right = node->right;
            node->right->parent = succ;
        } else {
            child = succ->right;
            parent = succ;
            color = succ->color;
            // succ is node's direct right child — still need to adopt left subtree
            succ->left = node->left;
            node->left->parent = succ;
        }

        succ->parent = node->parent;
        succ->color = node->color;

        if (!node->parent)
            root->rb_node = succ;
        else if (node == node->parent->left)
            node->parent->left = succ;
        else
            node->parent->right = succ;

        if (parent == succ)
            parent = succ;

    } else {
        child = node->left ? node->left : node->right;
        parent = node->parent;
        color = node->color;

        if (child)
            child->parent = parent;

        if (!parent)
            root->rb_node = child;
        else if (node == parent->left)
            parent->left = child;
        else
            parent->right = child;
    }

    if (color == RB_BLACK)
        rbtree_erase_fixup(child, parent, root);
}
