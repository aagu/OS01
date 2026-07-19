// kernel/fs/tmpfs.c — Memory-backed filesystem (tmpfs) with full VFS ops.
//
// All data lives in 4KB pages allocated via the PMM subpage pool.
// Files grow on demand: writes that exceed the current block count allocate
// new pages.  Directories store child pointers in dynamic arrays (kmalloc).
//
// VFS root-node convention: vfs_mount() sets the root vfs_node's fs_data to
// NULL and stores the filesystem root in mount->fs_data.  Every ops function
// uses tmpfs_get_node() to resolve the internal tmpfs_node_t pointer, falling
// back to mount->fs_data when node->fs_data is NULL (i.e. for the root).

#include <fs/tmpfs.h>
#include <fs/vfs.h>
#include <kernel/debug.h>
#include <kernel/pmm.h>      // alloc_4k_page, free_4k_page
#include <kernel/memory.h>    // Phy_To_Virt, Virt_To_Phy
#include <kernel/slab.h>      // kmalloc, kfree
#include <string.h>
#include <stdlib.h>          // calloc

#define TMPFS_CHILDREN_INIT_CAP 8

// ── Forward declaration — create/mkdir reference ops table ───────
static struct vfs_ops tmpfs_vfs_ops;

// ── Block metadata (kmalloc'd, singly-linked list) ──────────────
// Each block represents one 4KB page at a given block index.
typedef struct tmpfs_block_ptr {
    struct tmpfs_block_ptr *next;
    uint64_t                blk_idx;
    void                   *page;   // Phy_To_Virt(alloc_4k_page()) → 4096 bytes
} tmpfs_block_ptr_t;

// ── tmpfs inode (internal) ──────────────────────────────────────
typedef struct tmpfs_node {
    char               name[256];
    uint8_t            type;
    uint64_t           size;
    struct tmpfs_node *parent;

    // File: block chain
    tmpfs_block_ptr_t *first_block;
    tmpfs_block_ptr_t *last_block;

    // Directory: children array
    struct tmpfs_node **children;
    int                 child_count;
    int                 child_cap;
} tmpfs_node_t;

// ── Resolve tmpfs_node from vfs_node ────────────────────────────
// vfs_mount() stores the root tmpfs_node_t in mount->fs_data and sets
// node->fs_data = NULL for the root vfs_node.  Subdirectory/file nodes
// get their tmpfs_node_t pointer in node->fs_data via create/mkdir or
// vfs_lookup → entry.ino.  This helper handles both cases.

static inline tmpfs_node_t *tmpfs_get_node(struct vfs_node *node)
{
    if (!node) return NULL;
    if (node->fs_data)
        return (tmpfs_node_t *)node->fs_data;
    if (node->mount)
        return (tmpfs_node_t *)node->mount->fs_data;
    return NULL;
}

// ═══════════════════════════════════════════════════════════════════
//  Helper functions
// ═══════════════════════════════════════════════════════════════════

static tmpfs_block_ptr_t *tmpfs_find_block(tmpfs_node_t *tn, uint64_t blk_idx)
{
    tmpfs_block_ptr_t *bp = tn->first_block;
    while (bp) {
        if (bp->blk_idx == blk_idx) return bp;
        bp = bp->next;
    }
    return NULL;
}

static int tmpfs_grow_to(tmpfs_node_t *tn, uint64_t last_blk)
{
    for (uint64_t i = 0; i <= last_blk; i++) {
        if (tmpfs_find_block(tn, i)) continue;

        tmpfs_block_ptr_t *bp = kmalloc(sizeof(tmpfs_block_ptr_t));
        if (!bp) return -1;
        bp->blk_idx = i;
        uint64_t phys = alloc_4k_page();  // returns uint64_t physical address
        if (!phys) { kfree(bp); return -1; }
        bp->page = (void *)Phy_To_Virt(phys);
        memset(bp->page, 0, 4096);
        bp->next = NULL;

        if (!tn->first_block) {
            tn->first_block = bp;
            tn->last_block  = bp;
        } else {
            tn->last_block->next = bp;
            tn->last_block       = bp;
        }
    }
    return 0;
}

static void tmpfs_free_blocks(tmpfs_node_t *tn)
{
    tmpfs_block_ptr_t *bp = tn->first_block;
    while (bp) {
        tmpfs_block_ptr_t *next = bp->next;
        if (bp->page) free_4k_page(Virt_To_Phy(bp->page));
        kfree(bp);
        bp = next;
    }
    tn->first_block = NULL;
    tn->last_block  = NULL;
}

static void tmpfs_truncate_blocks(tmpfs_node_t *tn, uint64_t new_size)
{
    uint64_t last_keep = (new_size > 0) ? ((new_size - 1) / 4096) : 0;
    tmpfs_block_ptr_t *prev = NULL;
    tmpfs_block_ptr_t *bp   = tn->first_block;

    while (bp) {
        if (bp->blk_idx > last_keep) {
            if (bp->page) free_4k_page(Virt_To_Phy(bp->page));
            tmpfs_block_ptr_t *next = bp->next;
            kfree(bp);
            if (prev) prev->next = next;
            else tn->first_block = next;
            if (bp == tn->last_block) tn->last_block = prev;
            bp = next;
        } else {
            prev = bp;
            bp   = bp->next;
        }
    }
}

static int tmpfs_find_child(tmpfs_node_t *dir, const char *name)
{
    for (int i = 0; i < dir->child_count; i++) {
        if (strcmp(dir->children[i]->name, name) == 0)
            return i;
    }
    return -1;
}

// ═══════════════════════════════════════════════════════════════════
//  VFS ops
// ═══════════════════════════════════════════════════════════════════

static int tmpfs_vfs_read(struct vfs_node *node, uint64_t offset,
                           uint64_t size, void *buffer)
{
    tmpfs_node_t *tn = tmpfs_get_node(node);
    if (!tn || !buffer || size == 0) return 0;
    if (offset >= tn->size) return 0;
    if (offset + size > tn->size) size = tn->size - offset;

    uint8_t *out = (uint8_t *)buffer;
    uint64_t remaining = size;
    uint64_t pos = offset;

    while (remaining > 0) {
        uint64_t blk_idx = pos / 4096;
        uint32_t blk_off = (uint32_t)(pos % 4096);
        tmpfs_block_ptr_t *bp = tmpfs_find_block(tn, blk_idx);
        if (!bp) return -1;

        uint32_t chunk = 4096 - blk_off;
        if (chunk > remaining) chunk = (uint32_t)remaining;
        memcpy(out, (uint8_t *)bp->page + blk_off, chunk);
        out       += chunk;
        pos       += chunk;
        remaining -= chunk;
    }
    return (int)size;
}

static int tmpfs_vfs_write(struct vfs_node *node, uint64_t offset,
                            uint64_t size, void *buffer)
{
    tmpfs_node_t *tn = tmpfs_get_node(node);
    if (!tn || !buffer || size == 0) return 0;

    uint64_t end_pos = offset + size;
    uint64_t last_blk = (end_pos > 0) ? ((end_pos - 1) / 4096) : 0;
    if (tmpfs_grow_to(tn, last_blk) != 0) return -1;
    if (end_pos > tn->size) tn->size = end_pos;

    uint8_t *in = (uint8_t *)buffer;
    uint64_t remaining = size;
    uint64_t pos = offset;

    while (remaining > 0) {
        uint64_t blk_idx = pos / 4096;
        uint32_t blk_off = (uint32_t)(pos % 4096);
        tmpfs_block_ptr_t *bp = tmpfs_find_block(tn, blk_idx);
        if (!bp) return -1;

        uint32_t chunk = 4096 - blk_off;
        if (chunk > remaining) chunk = (uint32_t)remaining;
        memcpy((uint8_t *)bp->page + blk_off, in, chunk);
        in        += chunk;
        pos       += chunk;
        remaining -= chunk;
    }
    return (int)size;
}

static int tmpfs_vfs_readdir(struct vfs_node *node, uint64_t index,
                              struct vfs_dirent *entry)
{
    tmpfs_node_t *d = tmpfs_get_node(node);
    if (!d || d->type != VFS_DIR) return -1;

    if (index == 0) {
        strcpy(entry->name, ".");
        entry->type = VFS_DIR;
        entry->ino  = (uint64_t)(uintptr_t)d;
        return 0;
    }
    if (index == 1) {
        strcpy(entry->name, "..");
        entry->type = VFS_DIR;
        entry->ino  = (uint64_t)(uintptr_t)(d->parent ? d->parent : d);
        return 0;
    }

    uint64_t child_idx = index - 2;
    if (child_idx >= (uint64_t)d->child_count) {
        entry->name[0] = '\0';
        return 0;
    }

    tmpfs_node_t *child = d->children[child_idx];
    size_t nlen = strlen(child->name);
    if (nlen >= 256) nlen = 255;
    memcpy(entry->name, child->name, nlen);
    entry->name[nlen] = '\0';
    entry->type = child->type;
    entry->size = child->size;
    entry->ino  = (uint64_t)(uintptr_t)child;
    return 0;
}

static struct vfs_node *tmpfs_vfs_create(struct vfs_node *dir,
                                          const char *name)
{
    tmpfs_node_t *d = tmpfs_get_node(dir);
    if (!d || d->type != VFS_DIR) return NULL;
    if (tmpfs_find_child(d, name) >= 0) return NULL;

    tmpfs_node_t *new_node = calloc(1, sizeof(tmpfs_node_t));
    if (!new_node) return NULL;
    strcpy(new_node->name, name);
    new_node->type   = VFS_FILE;
    new_node->parent = d;

    // Expand children array: kmalloc+memcpy+kfree (NEVER realloc)
    if (d->child_count >= d->child_cap) {
        int new_cap = d->child_cap * 2;
        tmpfs_node_t **new_arr = kmalloc(new_cap * sizeof(tmpfs_node_t *));
        if (!new_arr) { kfree(new_node); return NULL; }
        memcpy(new_arr, d->children, d->child_count * sizeof(tmpfs_node_t *));
        memset(new_arr + d->child_count, 0,
               (new_cap - d->child_count) * sizeof(tmpfs_node_t *));
        kfree(d->children);
        d->children  = new_arr;
        d->child_cap = new_cap;
    }
    d->children[d->child_count++] = new_node;

    struct vfs_node *vn = calloc(1, sizeof(struct vfs_node));
    if (!vn) { d->children[--d->child_count] = NULL; kfree(new_node); return NULL; }
    vn->type    = VFS_FILE;
    vn->size    = 0;
    vn->fs_data = new_node;
    vn->ops     = &tmpfs_vfs_ops;
    vn->mount   = dir->mount;
    vn->parent  = dir;
    vn->refcount = 1;
    // vn->name must be set last — strdup may fail
    vn->name = strdup(name);
    if (!vn->name) {
        vn->refcount = 0;
        d->children[--d->child_count] = NULL;
        kfree(new_node);
        free(vn);
        return NULL;
    }
    return vn;
}

static struct vfs_node *tmpfs_vfs_mkdir(struct vfs_node *dir,
                                         const char *name)
{
    tmpfs_node_t *d = tmpfs_get_node(dir);
    if (!d || d->type != VFS_DIR) return NULL;
    if (tmpfs_find_child(d, name) >= 0) return NULL;

    tmpfs_node_t *new_node = calloc(1, sizeof(tmpfs_node_t));
    if (!new_node) return NULL;
    strcpy(new_node->name, name);
    new_node->type   = VFS_DIR;
    new_node->parent = d;

    if (d->child_count >= d->child_cap) {
        int new_cap = d->child_cap * 2;
        tmpfs_node_t **new_arr = kmalloc(new_cap * sizeof(tmpfs_node_t *));
        if (!new_arr) { kfree(new_node); return NULL; }
        memcpy(new_arr, d->children, d->child_count * sizeof(tmpfs_node_t *));
        kfree(d->children);
        d->children  = new_arr;
        d->child_cap = new_cap;
    }
    d->children[d->child_count++] = new_node;

    struct vfs_node *vn = calloc(1, sizeof(struct vfs_node));
    if (!vn) { d->children[--d->child_count] = NULL; kfree(new_node); return NULL; }
    vn->type    = VFS_DIR;
    vn->size    = 0;
    vn->fs_data = new_node;
    vn->ops     = &tmpfs_vfs_ops;
    vn->mount   = dir->mount;
    vn->parent  = dir;
    vn->refcount = 1;
    vn->name = strdup(name);
    if (!vn->name) {
        vn->refcount = 0;
        d->children[--d->child_count] = NULL;
        kfree(new_node);
        free(vn);
        return NULL;
    }
    return vn;
}

static int tmpfs_vfs_unlink(struct vfs_node *dir, const char *name)
{
    tmpfs_node_t *d = tmpfs_get_node(dir);
    if (!d || d->type != VFS_DIR) return -1;
    int idx = tmpfs_find_child(d, name);
    if (idx < 0) return -1;

    tmpfs_node_t *child = d->children[idx];
    tmpfs_free_blocks(child);
    kfree(child);
    d->children[idx] = d->children[d->child_count - 1];
    d->child_count--;
    return 0;
}

static int tmpfs_vfs_rmdir(struct vfs_node *dir, const char *name)
{
    tmpfs_node_t *d = tmpfs_get_node(dir);
    if (!d) return -1;
    int idx = tmpfs_find_child(d, name);
    if (idx < 0) return -1;
    tmpfs_node_t *child = d->children[idx];
    if (child->child_count > 0) return -1;  // not empty
    kfree(child);
    d->children[idx] = d->children[d->child_count - 1];
    d->child_count--;
    return 0;
}

static int tmpfs_vfs_rename(struct vfs_node *olddir, const char *oldname,
                             struct vfs_node *newdir, const char *newname)
{
    tmpfs_node_t *od = tmpfs_get_node(olddir);
    tmpfs_node_t *nd = tmpfs_get_node(newdir);
    if (!od || !nd) return -1;
    int idx = tmpfs_find_child(od, oldname);
    if (idx < 0) return -1;
    if (tmpfs_find_child(nd, newname) >= 0) return -1;

    tmpfs_node_t *child = od->children[idx];
    strcpy(child->name, newname);
    child->parent = nd;

    od->children[idx] = od->children[od->child_count - 1];
    od->child_count--;

    if (nd->child_count >= nd->child_cap) {
        int new_cap = nd->child_cap * 2;
        tmpfs_node_t **new_arr = kmalloc(new_cap * sizeof(tmpfs_node_t *));
        if (!new_arr) return -1;
        memcpy(new_arr, nd->children, nd->child_count * sizeof(tmpfs_node_t *));
        kfree(nd->children);
        nd->children  = new_arr;
        nd->child_cap = new_cap;
    }
    nd->children[nd->child_count++] = child;
    return 0;
}

static int tmpfs_vfs_truncate(struct vfs_node *node, uint64_t new_size)
{
    tmpfs_node_t *tn = tmpfs_get_node(node);
    if (!tn) return -1;
    tmpfs_truncate_blocks(tn, new_size);
    tn->size = new_size;
    return 0;
}

static struct vfs_ops tmpfs_vfs_ops = {
    .flags    = 0,  // case-sensitive
    .read     = tmpfs_vfs_read,
    .write    = tmpfs_vfs_write,
    .readdir  = tmpfs_vfs_readdir,
    .create   = tmpfs_vfs_create,
    .unlink   = tmpfs_vfs_unlink,
    .mkdir    = tmpfs_vfs_mkdir,
    .rmdir    = tmpfs_vfs_rmdir,
    .rename   = tmpfs_vfs_rename,
    .truncate = tmpfs_vfs_truncate,
};

// ═══════════════════════════════════════════════════════════════════
//  Initialization
// ═══════════════════════════════════════════════════════════════════

void tmpfs_init(void)
{
    tmpfs_node_t *root = calloc(1, sizeof(tmpfs_node_t));
    if (!root) {
        debug_fs("tmpfs: root allocation failed\n");
        return;
    }
    root->type = VFS_DIR;
    strcpy(root->name, "/");
    root->children  = kmalloc(TMPFS_CHILDREN_INIT_CAP * sizeof(tmpfs_node_t *));
    if (!root->children) {
        kfree(root);
        debug_fs("tmpfs: children array allocation failed\n");
        return;
    }
    root->child_cap = TMPFS_CHILDREN_INIT_CAP;
    root->parent    = NULL;

    vfs_mount("/tmp", NULL, &tmpfs_vfs_ops, root);
    debug_fs("tmpfs: mounted at /tmp\n");
}

#ifdef OS01_SELFTEST
// Test registered by selftest_run_all() via forward declaration in selftest.c

int tmpfs_selftest_mounted(void)
{
    struct vfs_node *tmp = vfs_lookup("/tmp");
    if (!tmp) return -1;
    if (tmp->type != VFS_DIR) { vfs_node_put(tmp); return -1; }
    vfs_node_put(tmp);
    return 0;
}
#endif
