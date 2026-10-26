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
#include <core/debug.h>
#include <memory/pmm.h>      // alloc_4k_page, free_4k_page
#include <memory/memory.h>    // Phy_To_Virt, Virt_To_Phy
#include <memory/slab.h>      // kmalloc, kfree
#include <string.h>
#include <stdlib.h>          // calloc
#include <errno.h>

#define TMPFS_CHILDREN_INIT_CAP 8

// ── Forward declaration — create/mkdir reference ops table ───────
static const vfs_inode_ops_t tmpfs_iops;
static const vfs_file_ops_t tmpfs_fops;

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
    uint32_t           refcount;
    uint64_t           size;
    uint32_t           mode;
    uint32_t           uid;
    uint32_t           gid;
    uint32_t           nlink;
    uint64_t           atime;
    uint64_t           mtime;
    uint64_t           ctime;
    struct tmpfs_node *parent;

    // File: block chain
    tmpfs_block_ptr_t *first_block;
    tmpfs_block_ptr_t *last_block;

    // Directory: children array
    struct tmpfs_node **children;
    int                 child_count;
    int                 child_cap;
} tmpfs_node_t;

static spinlock_T tmpfs_lock = { 1 };

static inline void tmpfs_node_get_locked(tmpfs_node_t *tn)
{
    if (tn) __sync_add_and_fetch(&tn->refcount, 1);
}

static inline uint32_t tmpfs_node_put_locked(tmpfs_node_t *tn)
{
    if (!tn) return 1;
    return __sync_sub_and_fetch(&tn->refcount, 1);
}

static void tmpfs_free_blocks(tmpfs_node_t *tn);

static void tmpfs_destroy_node_unlocked(tmpfs_node_t *tn)
{
    if (!tn) return;
    tmpfs_free_blocks(tn);
    if (tn->children) {
        kfree(tn->children);
        tn->children = NULL;
    }
    kfree(tn);
}

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
    if (!node || !buffer || size == 0) return 0;
    spin_lock(&tmpfs_lock);
    tmpfs_node_t *tn = tmpfs_get_node(node);
    if (!tn) { spin_unlock(&tmpfs_lock); return 0; }
    if (offset >= tn->size) { spin_unlock(&tmpfs_lock); return 0; }
    if (offset + size > tn->size) size = tn->size - offset;

    uint8_t *out = (uint8_t *)buffer;
    uint64_t remaining = size;
    uint64_t pos = offset;

    while (remaining > 0) {
        uint64_t blk_idx = pos / 4096;
        uint32_t blk_off = (uint32_t)(pos % 4096);
        tmpfs_block_ptr_t *bp = tmpfs_find_block(tn, blk_idx);
        if (!bp) { spin_unlock(&tmpfs_lock); return -1; }

        uint32_t chunk = 4096 - blk_off;
        if (chunk > remaining) chunk = (uint32_t)remaining;
        memcpy(out, (uint8_t *)bp->page + blk_off, chunk);
        out       += chunk;
        pos       += chunk;
        remaining -= chunk;
    }
    spin_unlock(&tmpfs_lock);
    return (int)size;
}

static int tmpfs_vfs_write(struct vfs_node *node, uint64_t offset,
                            uint64_t size, void *buffer)
{
    if (!node || !buffer || size == 0) return 0;
    spin_lock(&tmpfs_lock);
    tmpfs_node_t *tn = tmpfs_get_node(node);
    if (!tn) { spin_unlock(&tmpfs_lock); return 0; }

    uint64_t end_pos = offset + size;
    uint64_t last_blk = (end_pos > 0) ? ((end_pos - 1) / 4096) : 0;
    if (tmpfs_grow_to(tn, last_blk) != 0) { spin_unlock(&tmpfs_lock); return -1; }
    if (end_pos > tn->size) tn->size = end_pos;
    node->size = tn->size;

    uint8_t *in = (uint8_t *)buffer;
    uint64_t remaining = size;
    uint64_t pos = offset;

    while (remaining > 0) {
        uint64_t blk_idx = pos / 4096;
        uint32_t blk_off = (uint32_t)(pos % 4096);
        tmpfs_block_ptr_t *bp = tmpfs_find_block(tn, blk_idx);
        if (!bp) { spin_unlock(&tmpfs_lock); return -1; }

        uint32_t chunk = 4096 - blk_off;
        if (chunk > remaining) chunk = (uint32_t)remaining;
        memcpy((uint8_t *)bp->page + blk_off, in, chunk);
        in        += chunk;
        pos       += chunk;
        remaining -= chunk;
    }
    if (node->mount) {
        spin_lock(&node->mount->active_lock);
        vfs_active_nodes_sync_size_locked(node->mount, (uint64_t)(uintptr_t)tn,
                                          tn->size, tn->mtime);
        spin_unlock(&node->mount->active_lock);
    }
    spin_unlock(&tmpfs_lock);
    return (int)size;
}

static int tmpfs_vfs_readdir(struct vfs_node *node, uint64_t index,
                              struct vfs_dirent *entry)
{
    if (!node || !entry) return -1;
    spin_lock(&tmpfs_lock);
    tmpfs_node_t *d = tmpfs_get_node(node);
    if (!d || d->type != VFS_DIR) { spin_unlock(&tmpfs_lock); return -1; }

    if (index == 0) {
        strcpy(entry->name, ".");
        entry->type = VFS_DIR;
        entry->ino  = (uint64_t)(uintptr_t)d;
        spin_unlock(&tmpfs_lock);
        return 0;
    }
    if (index == 1) {
        strcpy(entry->name, "..");
        entry->type = VFS_DIR;
        entry->ino  = (uint64_t)(uintptr_t)(d->parent ? d->parent : d);
        spin_unlock(&tmpfs_lock);
        return 0;
    }

    uint64_t child_idx = index - 2;
    if (child_idx >= (uint64_t)d->child_count) {
        entry->name[0] = '\0';
        spin_unlock(&tmpfs_lock);
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
    spin_unlock(&tmpfs_lock);
    return 0;
}

static struct vfs_node *tmpfs_vfs_create(struct vfs_node *dir,
                                          const char *name)
{
    if (!dir || !name) return NULL;
    tmpfs_node_t *new_node = calloc(1, sizeof(tmpfs_node_t));
    struct vfs_node *vn = calloc(1, sizeof(struct vfs_node));
    char *vn_name = strdup(name);
    if (!new_node || !vn || !vn_name) {
        if (new_node) kfree(new_node);
        if (vn) free(vn);
        if (vn_name) free(vn_name);
        return NULL;
    }

    spin_lock(&tmpfs_lock);
    tmpfs_node_t *d = tmpfs_get_node(dir);
    if (!d || d->type != VFS_DIR || d->nlink == 0 ||
        (dir->node_flags & VFS_NODE_ORPHANED) || tmpfs_find_child(d, name) >= 0) {
        spin_unlock(&tmpfs_lock);
        kfree(new_node);
        free(vn);
        free(vn_name);
        return NULL;
    }

    strcpy(new_node->name, name);
    new_node->type     = VFS_FILE;
    new_node->refcount = 2; // 1 for d->children[], 1 for vn
    new_node->mode     = S_IFREG | 0755;
    new_node->nlink    = 1;
    new_node->parent   = d;

    // Expand children array: kmalloc+memcpy+kfree (NEVER realloc)
    if (d->child_count >= d->child_cap) {
        int new_cap = d->child_cap > 0 ? d->child_cap * 2 : TMPFS_CHILDREN_INIT_CAP;
        tmpfs_node_t **new_arr = kmalloc(new_cap * sizeof(tmpfs_node_t *));
        if (!new_arr) {
            spin_unlock(&tmpfs_lock);
            kfree(new_node);
            kfree(vn);
            kfree(vn_name);
            return NULL;
        }
        if (d->children && d->child_count > 0)
            memcpy(new_arr, d->children, d->child_count * sizeof(tmpfs_node_t *));
        memset(new_arr + d->child_count, 0,
               (new_cap - d->child_count) * sizeof(tmpfs_node_t *));
        if (d->children)
            kfree(d->children);
        d->children  = new_arr;
        d->child_cap = new_cap;
    }
    d->children[d->child_count++] = new_node;

    vn->name    = vn_name;
    vn->type    = VFS_FILE;
    vn->size    = 0;
    vn->mode    = new_node->mode;
    vn->nlink   = new_node->nlink;
    vn->fs_data = new_node;
    vn->i_ops   = &tmpfs_iops;
    vn->f_ops   = &tmpfs_fops;
    vn->ino     = (uint64_t)(uintptr_t)new_node;
    vn->mount   = dir->mount;
    vn->parent  = vfs_node_get(dir);
    vn->refcount = 1;

    vfs_active_node_register(vn);
    spin_unlock(&tmpfs_lock);
    return vn;
}

static struct vfs_node *tmpfs_vfs_mkdir(struct vfs_node *dir,
                                         const char *name)
{
    if (!dir || !name) return NULL;
    tmpfs_node_t *new_node = calloc(1, sizeof(tmpfs_node_t));
    struct vfs_node *vn = calloc(1, sizeof(struct vfs_node));
    char *vn_name = strdup(name);
    if (!new_node || !vn || !vn_name) {
        if (new_node) kfree(new_node);
        if (vn) kfree(vn);
        if (vn_name) kfree(vn_name);
        return NULL;
    }

    new_node->children = kmalloc(TMPFS_CHILDREN_INIT_CAP * sizeof(tmpfs_node_t *));
    if (!new_node->children) {
        kfree(new_node);
        kfree(vn);
        kfree(vn_name);
        return NULL;
    }
    memset(new_node->children, 0, TMPFS_CHILDREN_INIT_CAP * sizeof(tmpfs_node_t *));
    new_node->child_cap = TMPFS_CHILDREN_INIT_CAP;

    spin_lock(&tmpfs_lock);
    tmpfs_node_t *d = tmpfs_get_node(dir);
    if (!d || d->type != VFS_DIR || d->nlink == 0 ||
        (dir->node_flags & VFS_NODE_ORPHANED) || tmpfs_find_child(d, name) >= 0) {
        spin_unlock(&tmpfs_lock);
        kfree(new_node->children);
        kfree(new_node);
        kfree(vn);
        kfree(vn_name);
        return NULL;
    }

    strcpy(new_node->name, name);
    new_node->type     = VFS_DIR;
    new_node->refcount = 2; // 1 for d->children[], 1 for vn
    new_node->mode     = S_IFDIR | 0755;
    new_node->nlink    = 2;
    new_node->parent   = d;

    if (d->child_count >= d->child_cap) {
        int new_cap = d->child_cap > 0 ? d->child_cap * 2 : TMPFS_CHILDREN_INIT_CAP;
        tmpfs_node_t **new_arr = kmalloc(new_cap * sizeof(tmpfs_node_t *));
        if (!new_arr) {
            spin_unlock(&tmpfs_lock);
            kfree(new_node->children);
            kfree(new_node);
            kfree(vn);
            kfree(vn_name);
            return NULL;
        }
        if (d->children && d->child_count > 0)
            memcpy(new_arr, d->children, d->child_count * sizeof(tmpfs_node_t *));
        memset(new_arr + d->child_count, 0,
               (new_cap - d->child_count) * sizeof(tmpfs_node_t *));
        if (d->children)
            kfree(d->children);
        d->children  = new_arr;
        d->child_cap = new_cap;
    }
    d->children[d->child_count++] = new_node;

    vn->name    = vn_name;
    vn->type    = VFS_DIR;
    vn->size    = 0;
    vn->mode    = new_node->mode;
    vn->nlink   = new_node->nlink;
    vn->fs_data = new_node;
    vn->i_ops   = &tmpfs_iops;
    vn->f_ops   = &tmpfs_fops;
    vn->ino     = (uint64_t)(uintptr_t)new_node;
    vn->mount   = dir->mount;
    vn->parent  = vfs_node_get(dir);
    vn->refcount = 1;

    vfs_active_node_register(vn);
    spin_unlock(&tmpfs_lock);
    return vn;
}

static int tmpfs_vfs_unlink(struct vfs_node *dir, const char *name)
{
    if (!dir || !name) return -1;
    spin_lock(&tmpfs_lock);
    tmpfs_node_t *d = tmpfs_get_node(dir);
    if (!d || d->type != VFS_DIR) { spin_unlock(&tmpfs_lock); return -1; }
    int idx = tmpfs_find_child(d, name);
    if (idx < 0) { spin_unlock(&tmpfs_lock); return -1; }

    tmpfs_node_t *child = d->children[idx];
    d->children[idx] = d->children[d->child_count - 1];
    d->child_count--;
    child->nlink = 0;
    child->parent = NULL;

    if (dir->mount) {
        spin_lock(&dir->mount->active_lock);
        vfs_active_nodes_mark_orphaned_locked(dir->mount, (uint64_t)(uintptr_t)child);
        spin_unlock(&dir->mount->active_lock);
    }

    uint32_t rem = tmpfs_node_put_locked(child);
    spin_unlock(&tmpfs_lock);

    if (rem == 0) {
        tmpfs_destroy_node_unlocked(child);
    }
    return 0;
}

static int tmpfs_vfs_rmdir(struct vfs_node *dir, const char *name)
{
    if (!dir || !name) return -1;
    spin_lock(&tmpfs_lock);
    tmpfs_node_t *d = tmpfs_get_node(dir);
    if (!d || d->type != VFS_DIR) { spin_unlock(&tmpfs_lock); return -1; }
    int idx = tmpfs_find_child(d, name);
    if (idx < 0) { spin_unlock(&tmpfs_lock); return -1; }
    tmpfs_node_t *child = d->children[idx];
    if (child->child_count > 0) { spin_unlock(&tmpfs_lock); return -1; }  // not empty

    d->children[idx] = d->children[d->child_count - 1];
    d->child_count--;
    child->nlink = 0;
    child->parent = NULL;

    if (dir->mount) {
        spin_lock(&dir->mount->active_lock);
        vfs_active_nodes_mark_orphaned_locked(dir->mount, (uint64_t)(uintptr_t)child);
        spin_unlock(&dir->mount->active_lock);
    }

    uint32_t rem = tmpfs_node_put_locked(child);
    spin_unlock(&tmpfs_lock);

    if (rem == 0) {
        tmpfs_destroy_node_unlocked(child);
    }
    return 0;
}

static int tmpfs_vfs_rename(struct vfs_node *olddir, const char *oldname,
                             struct vfs_node *newdir, const char *newname)
{
    if (!olddir || !oldname || !newdir || !newname) return -1;
    spin_lock(&tmpfs_lock);
    tmpfs_node_t *od = tmpfs_get_node(olddir);
    tmpfs_node_t *nd = tmpfs_get_node(newdir);
    if (!od || !nd) { spin_unlock(&tmpfs_lock); return -1; }
    int idx = tmpfs_find_child(od, oldname);
    if (idx < 0) { spin_unlock(&tmpfs_lock); return -1; }

    int dst_idx = tmpfs_find_child(nd, newname);
    tmpfs_node_t *dst_child = (dst_idx >= 0) ? nd->children[dst_idx] : NULL;

    // Ensure capacity in newdir before modifying any node state
    if (!dst_child && nd->child_count >= nd->child_cap) {
        int new_cap = nd->child_cap > 0 ? nd->child_cap * 2 : TMPFS_CHILDREN_INIT_CAP;
        tmpfs_node_t **new_arr = kmalloc(new_cap * sizeof(tmpfs_node_t *));
        if (!new_arr) {
            spin_unlock(&tmpfs_lock);
            return -1;
        }
        if (nd->children && nd->child_count > 0)
            memcpy(new_arr, nd->children, nd->child_count * sizeof(tmpfs_node_t *));
        memset(new_arr + nd->child_count, 0,
               (new_cap - nd->child_count) * sizeof(tmpfs_node_t *));
        if (nd->children)
            kfree(nd->children);
        nd->children  = new_arr;
        nd->child_cap = new_cap;
    }

    if (dst_child) {
        if (dst_child->type == VFS_DIR && dst_child->child_count > 0) {
            spin_unlock(&tmpfs_lock);
            return -1;
        }
        dst_child->nlink = 0;
        dst_child->parent = NULL;
        if (newdir->mount) {
            spin_lock(&newdir->mount->active_lock);
            vfs_active_nodes_mark_orphaned_locked(newdir->mount, (uint64_t)(uintptr_t)dst_child);
            spin_unlock(&newdir->mount->active_lock);
        }
        nd->children[dst_idx] = nd->children[nd->child_count - 1];
        nd->child_count--;
        if (od == nd) idx = tmpfs_find_child(od, oldname);
    }

    tmpfs_node_t *child = od->children[idx];
    strncpy(child->name, newname, sizeof(child->name) - 1);
    child->name[sizeof(child->name) - 1] = '\0';
    child->parent = nd;

    od->children[idx] = od->children[od->child_count - 1];
    od->child_count--;

    nd->children[nd->child_count++] = child;

    uint32_t rem_dst = dst_child ? tmpfs_node_put_locked(dst_child) : 1;
    spin_unlock(&tmpfs_lock);

    if (dst_child && rem_dst == 0) {
        tmpfs_destroy_node_unlocked(dst_child);
    }
    return 0;
}

static int tmpfs_vfs_truncate(struct vfs_node *node, uint64_t new_size)
{
    if (!node) return -1;
    spin_lock(&tmpfs_lock);
    tmpfs_node_t *tn = tmpfs_get_node(node);
    if (!tn) { spin_unlock(&tmpfs_lock); return -1; }
    tmpfs_truncate_blocks(tn, new_size);
    tn->size = new_size;
    node->size = new_size;
    if (node->mount) {
        spin_lock(&node->mount->active_lock);
        vfs_active_nodes_sync_size_locked(node->mount, (uint64_t)(uintptr_t)tn,
                                          tn->size, tn->mtime);
        spin_unlock(&node->mount->active_lock);
    }
    spin_unlock(&tmpfs_lock);
    return 0;
}

static int tmpfs_iop_lookup(struct vfs_node *dir, const char *name,
                            struct vfs_node **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    if (!dir || !name) return -EINVAL;

    struct vfs_node *vn = calloc(1, sizeof(struct vfs_node));
    char *vn_name = kmalloc(VFS_NAME_MAX);
    if (!vn || !vn_name) {
        if (vn) free(vn);
        if (vn_name) kfree(vn_name);
        return -ENOMEM;
    }

    spin_lock(&tmpfs_lock);
    tmpfs_node_t *d = tmpfs_get_node(dir);
    if (!d || d->type != VFS_DIR) {
        spin_unlock(&tmpfs_lock);
        kfree(vn_name);
        free(vn);
        return -ENOTDIR;
    }

    int idx = tmpfs_find_child(d, name);
    if (idx < 0) {
        spin_unlock(&tmpfs_lock);
        kfree(vn_name);
        free(vn);
        return -ENOENT;
    }

    tmpfs_node_t *tn = d->children[idx];
    tmpfs_node_get_locked(tn);

    strncpy(vn_name, tn->name, VFS_NAME_MAX - 1);
    vn_name[VFS_NAME_MAX - 1] = '\0';

    vn->name    = vn_name;
    vn->type    = tn->type;
    vn->ino     = (uint64_t)(uintptr_t)tn;
    vn->size    = tn->size;
    vn->mode    = tn->mode ? tn->mode : ((tn->type == VFS_DIR) ? (S_IFDIR | 0755) : (S_IFREG | 0755));
    vn->uid     = tn->uid;
    vn->gid     = tn->gid;
    vn->nlink   = tn->nlink;
    vn->atime   = tn->atime;
    vn->mtime   = tn->mtime;
    vn->ctime   = tn->ctime;
    vn->fs_data = tn;
    vn->i_ops   = &tmpfs_iops;
    vn->f_ops   = &tmpfs_fops;
    vn->mount   = dir->mount;
    vn->parent  = vfs_node_get(dir);
    vn->refcount = 1;

    vfs_active_node_register(vn);
    spin_unlock(&tmpfs_lock);
    *out = vn;
    return 0;
}

static int tmpfs_iop_getattr(struct vfs_node *node, struct stat *buf)
{
    if (!node || !buf) return -1;
    spin_lock(&tmpfs_lock);
    tmpfs_node_t *tn = tmpfs_get_node(node);
    if (tn) {
        node->size  = tn->size;
        node->mode  = tn->mode ? tn->mode : node->mode;
        node->uid   = tn->uid;
        node->gid   = tn->gid;
        node->nlink = tn->nlink;
        node->atime = tn->atime;
        node->mtime = tn->mtime;
        node->ctime = tn->ctime;
    }
    spin_unlock(&tmpfs_lock);
    return vfs_generic_getattr(node, buf);
}

static void tmpfs_iop_evict_node(struct vfs_node *node)
{
    if (!node || !node->mount) return;
    if (node == node->mount->root) return;

    tmpfs_node_t *tn = (tmpfs_node_t *)node->fs_data;
    spin_lock(&tmpfs_lock);
    spin_lock(&node->mount->active_lock);
    vfs_active_node_unregister_locked(node->mount, node);
    spin_unlock(&node->mount->active_lock);

    uint32_t rem = tn ? tmpfs_node_put_locked(tn) : 1;
    spin_unlock(&tmpfs_lock);

    if (tn && rem == 0) {
        tmpfs_destroy_node_unlocked(tn);
    }
}

static const vfs_inode_ops_t tmpfs_iops = {
    .lookup     = tmpfs_iop_lookup,
    .getattr    = tmpfs_iop_getattr,
    .evict_node = tmpfs_iop_evict_node,
    .create     = tmpfs_vfs_create,
    .unlink     = tmpfs_vfs_unlink,
    .mkdir      = tmpfs_vfs_mkdir,
    .rmdir      = tmpfs_vfs_rmdir,
    .rename     = tmpfs_vfs_rename,
    .truncate   = tmpfs_vfs_truncate,
};

static const vfs_file_ops_t tmpfs_fops = {
    .read     = tmpfs_vfs_read,
    .write    = tmpfs_vfs_write,
    .readdir  = tmpfs_vfs_readdir,
};

// ═══════════════════════════════════════════════════════════════════
//  Initialization
// ═══════════════════════════════════════════════════════════════════

static void tmpfs_free_node(tmpfs_node_t *tn)
{
    if (!tn) return;
    if (tn->children) {
        for (int i = 0; i < tn->child_count; i++) {
            if (tn->children[i]) {
                tmpfs_free_node(tn->children[i]);
                tn->children[i] = NULL;
            }
        }
    }
    tmpfs_destroy_node_unlocked(tn);
}

static void tmpfs_fs_unmount(struct vfs_mount *mp)
{
    if (!mp || !mp->fs_data) return;
    tmpfs_node_t *root = (tmpfs_node_t *)mp->fs_data;
    spin_lock(&tmpfs_lock);
    tmpfs_free_node(root);
    spin_unlock(&tmpfs_lock);
    mp->fs_data = NULL;
}

static int tmpfs_fs_mount(struct vfs_mount *mp, block_device_t *dev, void *data)
{
    (void)dev;
    (void)data;
    tmpfs_node_t *root = calloc(1, sizeof(tmpfs_node_t));
    if (!root) {
        debug_fs("tmpfs: root allocation failed\n");
        return -ENOMEM;
    }
    root->type = VFS_DIR;
    root->refcount = 1;
    root->mode = S_IFDIR | 0755;
    root->nlink = 2;
    strcpy(root->name, "/");
    root->children = kmalloc(TMPFS_CHILDREN_INIT_CAP * sizeof(tmpfs_node_t *));
    if (!root->children) {
        kfree(root);
        debug_fs("tmpfs: children array allocation failed\n");
        return -ENOMEM;
    }
    root->child_cap = TMPFS_CHILDREN_INIT_CAP;
    root->parent    = NULL;

    mp->i_ops = &tmpfs_iops;
    mp->f_ops = &tmpfs_fops;
    mp->fs_data = root;
    mp->root->i_ops = &tmpfs_iops;
    mp->root->f_ops = &tmpfs_fops;
    mp->root->ino = (uint64_t)(uintptr_t)root;
    mp->root->mode = S_IFDIR | 0755;
    mp->root->nlink = 2;
    return 0;
}

static vfs_fs_type_t tmpfs_fs_type = {
    .name     = "tmpfs",
    .fs_flags = 0,
    .mount    = tmpfs_fs_mount,
    .unmount  = tmpfs_fs_unmount,
};

int tmpfs_register_fs(void)
{
    return vfs_register_filesystem(&tmpfs_fs_type);
}

void tmpfs_init(void)
{
    tmpfs_register_fs();
    if (vfs_mount_fs("tmpfs", "/tmp", NULL, NULL) == 0) {
        debug_fs("tmpfs: mounted at /tmp\n");
    }
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
