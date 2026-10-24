# ARCH-5 VFS Abstraction Enhancement — Phase 4 (Bounded Dentry Cache & Concurrency Verification) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a bounded 256-entry LRU dentry cache (`dcache`) with 64 hash buckets, case-insensitive key normalization for `VFS_FS_CASE_INSENSITIVE` mounts, parent-pointer consistency checks (`entry->node->parent == parent`), a mutation generation barrier (`dcache_mut_gen` / `dcache_mut_active`) preventing concurrent lookup/unlink stale backfill, mount-wide subtree flush on directory `rename`, lock-external `vfs_node_put`, and `VFS_FS_NO_DCACHE` / `VFS_NODE_NO_DCACHE` bypass.

**Architecture:**
- **Spec Reference:** [`docs/superpowers/specs/2026-10-24-arch5-vfs-abstraction-design.md`](../specs/2026-10-24-arch5-vfs-abstraction-design.md) (§4.5, §5.4, §5.5, §6, §7.1 Phase 4).
- **Key & Parent-Chain Invariant:**
  - Entries are keyed by `(parent->mount, vfs_node_stable_ino(parent), normalized_name)` (uppercased when `parent->mount->mnt_flags & VFS_FS_CASE_INSENSITIVE`).
  - On a hash match in `dcache_lookup_begin(parent, name, &out_gen)`, the cache additionally verifies `entry->node->parent == parent && !(entry->node->node_flags & (VFS_NODE_ORPHANED | VFS_NODE_STALE))`. If the parent pointer differs (e.g. `parent` was evicted and reconstructed) or the node is orphaned/stale, the entry is unlinked from `dcache` under `dcache_lock`, `vfs_node_put` is called **after** releasing `dcache_lock`, and the lookup proceeds as a cache miss.
- **Generation Barrier Against Stale Backfill:**
  - `dcache_lookup_begin` records `*out_gen = (dcache_mut_active == 0) ? dcache_mut_gen : 0` on a miss.
  - `vfs_unlink`, `vfs_rmdir`, and `vfs_rename` call `dcache_mutation_begin()` (`dcache_mut_active++; dcache_mut_gen++;`) and pre-invalidate the target entry (or flush the mount on `rename`) **before** invoking `i_ops->unlink/rmdir/rename`, then call `dcache_mutation_end_invalidate()` or `dcache_mutation_end_rename()` (`dcache_mut_gen++; dcache_mut_active--;`) **after** the backend operation returns.
  - `dcache_insert_validated(parent, name, child, lookup_gen)` only inserts when `lookup_gen != 0 && dcache_mut_active == 0 && dcache_mut_gen == lookup_gen`.
- **Lock-Order & Lock-External Release Contract:**
  - `dcache_lock` is a leaf spinlock protecting only in-memory hash/LRU manipulations and `vfs_node_get`. Every eviction, invalidation, or flush collects victim `vfs_node_t *` pointers while holding `dcache_lock`, releases `dcache_lock`, and only then calls `vfs_node_put()`.

**Tech Stack:** C11 (freestanding kernel + host-native unit tests), Clang/LLD, Make, Python QEMU runner.

---

## File Structure & Responsibilities

| File | Action | Responsibility |
|------|--------|----------------|
| `kernel/include/fs/vfs.h` | Modify | Declare `VFS_DCACHE_BUCKETS` (64), `VFS_DCACHE_CAPACITY` (256), and public `dcache_*` API functions |
| `hosttests/include/fs/vfs.h` | Modify | Mirror `VFS_DCACHE_BUCKETS`, `VFS_DCACHE_CAPACITY`, and `dcache_*` declarations for host unit tests |
| `kernel/fs/vfs.c` | Modify | Implement bounded `dcache` engine and wire it into `vfs_init`, `vfs_bind_submount_locked`, `__vfs_lookup_raw`, `vfs_unlink`, `vfs_rmdir`, and `vfs_rename` |
| `kernel/selftest/symlink_selftest.c` | Modify | Route `symlink_ext2_rollback` through `vfs_symlink` / `vfs_unlink` so `dcache` invalidation runs before `ext2` free-block/inode assertions |
| `hosttests/cases/test_vfs_dcache.c` | Create | Host unit test suite covering all 6 Spec §7.1 Phase 4 test cases |
| `hosttests/Makefile` | Modify | Wire `test_vfs_dcache.elf` into `TEST_BINS` |

> [!WARNING]
> Working tree note: `hosttests/Makefile`, `hosttests/cases/test_fb_resolution_hooks.c`, and `hosttests/mock/fb_resolution_runtime.h` have pre-existing unstaged changes from another workstream. Do NOT revert them and do NOT stage unrelated hunks when committing Phase 4 tasks.

---

### Task 1: Bounded 256-Entry `dcache` Engine & Public API (`kernel/include/fs/vfs.h`, `hosttests/include/fs/vfs.h`, `kernel/fs/vfs.c`)

**Files:**
- Modify: `kernel/include/fs/vfs.h`
- Modify: `hosttests/include/fs/vfs.h`
- Modify: `kernel/fs/vfs.c`

- [ ] **Step 1: Declare `dcache` constants and API in `kernel/include/fs/vfs.h` and `hosttests/include/fs/vfs.h`**

In both [`kernel/include/fs/vfs.h`](../../../kernel/include/fs/vfs.h) (right after `vfs_node_live_size` around line 200) and [`hosttests/include/fs/vfs.h`](../../../hosttests/include/fs/vfs.h), add:

```c
// ── Bounded Dentry Cache (Spec §4.5, §5.4) ────────────────
#define VFS_DCACHE_BUCKETS   64
#define VFS_DCACHE_CAPACITY  256

void        dcache_init(void);
vfs_node_t *dcache_lookup_begin(vfs_node_t *parent, const char *name,
                                uint64_t *out_gen);
void        dcache_insert_validated(vfs_node_t *parent, const char *name,
                                    vfs_node_t *child, uint64_t lookup_gen);
void        dcache_mutation_begin(void);
void        dcache_invalidate(vfs_node_t *parent, const char *name);
void        dcache_mutation_end_invalidate(vfs_node_t *parent, const char *name);
void        dcache_mutation_end_rename(vfs_node_t *olddir, const char *oldname,
                                       vfs_node_t *newdir, const char *newname);
void        dcache_flush_mount(vfs_mount_t *mp);
void        dcache_get_stats(uint64_t *hits, uint64_t *misses, uint64_t *evictions);
```

- [ ] **Step 2: Implement the bounded 256-slot `dcache` in `kernel/fs/vfs.c` and initialize it in `vfs_init()`**

In [`kernel/fs/vfs.c`](../../../kernel/fs/vfs.c) near the top (after `fs_type_usage` around line 65), add the `dcache` data structures and implementation:

```c
// ── Bounded Dentry Cache (Spec §5.4) ──────────────────────
typedef struct vfs_dcache_entry {
    vfs_mount_t             *parent_mount;
    uint64_t                 parent_ino;
    char                     name[VFS_NAME_MAX];
    vfs_node_t              *node;
    struct vfs_dcache_entry *hash_next;
    struct vfs_dcache_entry *lru_prev;
    struct vfs_dcache_entry *lru_next;
    bool                     in_use;
} vfs_dcache_entry_t;

static vfs_dcache_entry_t  dcache_pool[VFS_DCACHE_CAPACITY];
static vfs_dcache_entry_t *dcache_buckets[VFS_DCACHE_BUCKETS];
static vfs_dcache_entry_t *dcache_lru_head = NULL;
static vfs_dcache_entry_t *dcache_lru_tail = NULL;
static uint32_t            dcache_count = 0;
static spinlock_T          dcache_lock;
static uint64_t            dcache_mut_gen = 1;
static uint32_t            dcache_mut_active = 0;
static uint64_t            dcache_hits = 0;
static uint64_t            dcache_misses = 0;
static uint64_t            dcache_evictions = 0;
```

Right after `vfs_name_cmp()` in [`kernel/fs/vfs.c`](../../../kernel/fs/vfs.c), add the `dcache` helper and API functions:

```c
static void dcache_normalize_name(const vfs_mount_t *mp, const char *name,
                                  char out[VFS_NAME_MAX])
{
    bool ci = (mp && (mp->mnt_flags & VFS_FS_CASE_INSENSITIVE)) != 0;
    size_t i = 0;
    while (name[i] && i < VFS_NAME_MAX - 1) {
        char c = name[i];
        if (ci && c >= 'a' && c <= 'z') c -= 32;
        out[i++] = c;
    }
    out[i] = '\0';
}

static uint32_t dcache_hash(const vfs_mount_t *mp, uint64_t parent_ino,
                            const char *norm_name)
{
    uint64_t h = 1469598103934665603ULL;
    uintptr_t mp_val = (uintptr_t)mp;
    h ^= (uint64_t)mp_val;
    h *= 1099511628211ULL;
    h ^= parent_ino;
    h *= 1099511628211ULL;
    for (const unsigned char *p = (const unsigned char *)norm_name; *p; p++) {
        h ^= (uint64_t)(*p);
        h *= 1099511628211ULL;
    }
    return (uint32_t)(h & (VFS_DCACHE_BUCKETS - 1));
}

static void dcache_lru_remove_locked(vfs_dcache_entry_t *e)
{
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    else dcache_lru_head = e->lru_next;

    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;
    else dcache_lru_tail = e->lru_prev;

    e->lru_prev = NULL;
    e->lru_next = NULL;
}

static void dcache_lru_insert_head_locked(vfs_dcache_entry_t *e)
{
    e->lru_prev = NULL;
    e->lru_next = dcache_lru_head;
    if (dcache_lru_head) dcache_lru_head->lru_prev = e;
    else dcache_lru_tail = e;
    dcache_lru_head = e;
}

static void dcache_hash_remove_locked(uint32_t bucket, vfs_dcache_entry_t *e)
{
    vfs_dcache_entry_t **pp = &dcache_buckets[bucket];
    while (*pp) {
        if (*pp == e) {
            *pp = e->hash_next;
            e->hash_next = NULL;
            return;
        }
        pp = &(*pp)->hash_next;
    }
}

static void dcache_detach_entry_locked(uint32_t bucket, vfs_dcache_entry_t *e,
                                       vfs_node_t **out_node)
{
    dcache_hash_remove_locked(bucket, e);
    dcache_lru_remove_locked(e);
    if (out_node) *out_node = e->node;
    e->node = NULL;
    e->parent_mount = NULL;
    e->parent_ino = 0;
    e->name[0] = '\0';
    e->in_use = false;
    if (dcache_count > 0) dcache_count--;
}

static void dcache_bump_gen_locked(vfs_node_t **overflow_put, int *overflow_cnt)
{
    dcache_mut_gen++;
    if (dcache_mut_gen == 0) {
        dcache_mut_gen = 1;
        for (int i = 0; i < VFS_DCACHE_CAPACITY; i++) {
            if (!dcache_pool[i].in_use) continue;
            uint32_t b = dcache_hash(dcache_pool[i].parent_mount,
                                     dcache_pool[i].parent_ino,
                                     dcache_pool[i].name);
            vfs_node_t *n = NULL;
            dcache_detach_entry_locked(b, &dcache_pool[i], &n);
            if (n && overflow_put && overflow_cnt && *overflow_cnt < VFS_DCACHE_CAPACITY) {
                overflow_put[(*overflow_cnt)++] = n;
            }
        }
    }
}

void dcache_init(void)
{
    memset(dcache_pool, 0, sizeof(dcache_pool));
    memset(dcache_buckets, 0, sizeof(dcache_buckets));
    dcache_lru_head = NULL;
    dcache_lru_tail = NULL;
    dcache_count = 0;
    spin_init(&dcache_lock);
    dcache_mut_gen = 1;
    dcache_mut_active = 0;
    dcache_hits = 0;
    dcache_misses = 0;
    dcache_evictions = 0;
}

vfs_node_t *dcache_lookup_begin(vfs_node_t *parent, const char *name,
                                uint64_t *out_gen)
{
    if (out_gen) *out_gen = 0;
    if (!parent || !parent->mount || !name || name[0] == '\0') return NULL;
    if ((parent->node_flags & VFS_NODE_NO_DCACHE) ||
        (parent->mount->mnt_flags & VFS_FS_NO_DCACHE)) {
        return NULL;
    }

    char norm_name[VFS_NAME_MAX];
    dcache_normalize_name(parent->mount, name, norm_name);
    uint64_t parent_ino = vfs_node_stable_ino(parent);
    uint32_t bucket = dcache_hash(parent->mount, parent_ino, norm_name);

    vfs_node_t *stale_to_put = NULL;
    vfs_node_t *hit_node = NULL;

    spin_lock(&dcache_lock);
    for (vfs_dcache_entry_t *e = dcache_buckets[bucket]; e; e = e->hash_next) {
        if (e->parent_mount == parent->mount &&
            e->parent_ino == parent_ino &&
            strcmp(e->name, norm_name) == 0) {
            if (e->node &&
                e->node->parent == parent &&
                !(e->node->node_flags & (VFS_NODE_ORPHANED | VFS_NODE_STALE))) {
                dcache_lru_remove_locked(e);
                dcache_lru_insert_head_locked(e);
                hit_node = vfs_node_get(e->node);
                dcache_hits++;
                spin_unlock(&dcache_lock);
                return hit_node;
            }
            // Stale parent chain or orphaned node: evict and treat as miss.
            dcache_detach_entry_locked(bucket, e, &stale_to_put);
            break;
        }
    }

    dcache_misses++;
    if (out_gen) {
        *out_gen = (dcache_mut_active == 0) ? dcache_mut_gen : 0;
    }
    spin_unlock(&dcache_lock);

    if (stale_to_put) {
        vfs_node_put(stale_to_put);
    }
    return NULL;
}

void dcache_insert_validated(vfs_node_t *parent, const char *name,
                             vfs_node_t *child, uint64_t lookup_gen)
{
    if (!parent || !parent->mount || !name || name[0] == '\0' || !child) return;
    if (lookup_gen == 0) return;
    if ((parent->node_flags & VFS_NODE_NO_DCACHE) ||
        (parent->mount->mnt_flags & VFS_FS_NO_DCACHE) ||
        (child->node_flags & (VFS_NODE_NO_DCACHE | VFS_NODE_MOUNT_STUB |
                              VFS_NODE_ORPHANED | VFS_NODE_STALE))) {
        return;
    }
    if (child->mount != parent->mount || child->parent != parent) return;

    char norm_name[VFS_NAME_MAX];
    dcache_normalize_name(parent->mount, name, norm_name);
    uint64_t parent_ino = vfs_node_stable_ino(parent);
    uint32_t bucket = dcache_hash(parent->mount, parent_ino, norm_name);

    vfs_node_t *evicted_to_put = NULL;

    spin_lock(&dcache_lock);
    if (dcache_mut_active != 0 || dcache_mut_gen != lookup_gen ||
        (child->node_flags & (VFS_NODE_ORPHANED | VFS_NODE_STALE))) {
        spin_unlock(&dcache_lock);
        return;
    }

    for (vfs_dcache_entry_t *e = dcache_buckets[bucket]; e; e = e->hash_next) {
        if (e->parent_mount == parent->mount &&
            e->parent_ino == parent_ino &&
            strcmp(e->name, norm_name) == 0) {
            if (e->node != child) {
                evicted_to_put = e->node;
                e->node = vfs_node_get(child);
            }
            dcache_lru_remove_locked(e);
            dcache_lru_insert_head_locked(e);
            spin_unlock(&dcache_lock);
            if (evicted_to_put) vfs_node_put(evicted_to_put);
            return;
        }
    }

    vfs_dcache_entry_t *slot = NULL;
    if (dcache_count >= VFS_DCACHE_CAPACITY && dcache_lru_tail != NULL) {
        vfs_dcache_entry_t *victim = dcache_lru_tail;
        uint32_t vb = dcache_hash(victim->parent_mount, victim->parent_ino, victim->name);
        dcache_detach_entry_locked(vb, victim, &evicted_to_put);
        dcache_evictions++;
        slot = victim;
    } else {
        for (int i = 0; i < VFS_DCACHE_CAPACITY; i++) {
            if (!dcache_pool[i].in_use) {
                slot = &dcache_pool[i];
                break;
            }
        }
    }

    if (slot) {
        slot->parent_mount = parent->mount;
        slot->parent_ino = parent_ino;
        memcpy(slot->name, norm_name, strlen(norm_name) + 1);
        slot->node = vfs_node_get(child);
        slot->in_use = true;
        slot->hash_next = dcache_buckets[bucket];
        dcache_buckets[bucket] = slot;
        dcache_lru_insert_head_locked(slot);
        dcache_count++;
    }
    spin_unlock(&dcache_lock);

    if (evicted_to_put) {
        vfs_node_put(evicted_to_put);
    }
}

void dcache_mutation_begin(void)
{
    vfs_node_t **overflow_put = kmalloc(sizeof(vfs_node_t *) * VFS_DCACHE_CAPACITY);
    int overflow_cnt = 0;

    spin_lock(&dcache_lock);
    dcache_mut_active++;
    dcache_bump_gen_locked(overflow_put, &overflow_cnt);
    spin_unlock(&dcache_lock);

    if (overflow_put) {
        for (int i = 0; i < overflow_cnt; i++) {
            vfs_node_put(overflow_put[i]);
        }
        kfree(overflow_put);
    }
}

void dcache_invalidate(vfs_node_t *parent, const char *name)
{
    if (!parent || !parent->mount || !name || name[0] == '\0') return;

    char norm_name[VFS_NAME_MAX];
    dcache_normalize_name(parent->mount, name, norm_name);
    uint64_t parent_ino = vfs_node_stable_ino(parent);
    uint32_t bucket = dcache_hash(parent->mount, parent_ino, norm_name);

    vfs_node_t *removed = NULL;

    spin_lock(&dcache_lock);
    for (vfs_dcache_entry_t *e = dcache_buckets[bucket]; e; e = e->hash_next) {
        if (e->parent_mount == parent->mount &&
            e->parent_ino == parent_ino &&
            strcmp(e->name, norm_name) == 0) {
            dcache_detach_entry_locked(bucket, e, &removed);
            break;
        }
    }
    spin_unlock(&dcache_lock);

    if (removed) {
        vfs_node_put(removed);
    }
}

void dcache_mutation_end_invalidate(vfs_node_t *parent, const char *name)
{
    vfs_node_t **to_put = kmalloc(sizeof(vfs_node_t *) * (VFS_DCACHE_CAPACITY + 1));
    int put_cnt = 0;
    vfs_node_t *single_removed = NULL;

    char norm_name[VFS_NAME_MAX];
    uint64_t parent_ino = 0;
    uint32_t bucket = 0;
    bool has_target = (parent && parent->mount && name && name[0] != '\0');
    if (has_target) {
        dcache_normalize_name(parent->mount, name, norm_name);
        parent_ino = vfs_node_stable_ino(parent);
        bucket = dcache_hash(parent->mount, parent_ino, norm_name);
    }

    spin_lock(&dcache_lock);
    if (has_target) {
        for (vfs_dcache_entry_t *e = dcache_buckets[bucket]; e; e = e->hash_next) {
            if (e->parent_mount == parent->mount &&
                e->parent_ino == parent_ino &&
                strcmp(e->name, norm_name) == 0) {
                dcache_detach_entry_locked(bucket, e, &single_removed);
                break;
            }
        }
    }
    dcache_bump_gen_locked(to_put, &put_cnt);
    if (dcache_mut_active > 0) dcache_mut_active--;
    spin_unlock(&dcache_lock);

    if (single_removed) {
        vfs_node_put(single_removed);
    }
    if (to_put) {
        for (int i = 0; i < put_cnt; i++) {
            vfs_node_put(to_put[i]);
        }
        kfree(to_put);
    }
}

void dcache_flush_mount(vfs_mount_t *mp)
{
    if (!mp) return;
    vfs_node_t **to_put = kmalloc(sizeof(vfs_node_t *) * VFS_DCACHE_CAPACITY);
    int put_cnt = 0;

    spin_lock(&dcache_lock);
    for (int i = 0; i < VFS_DCACHE_CAPACITY; i++) {
        if (!dcache_pool[i].in_use) continue;
        if (dcache_pool[i].parent_mount == mp ||
            (dcache_pool[i].node && dcache_pool[i].node->mount == mp)) {
            uint32_t b = dcache_hash(dcache_pool[i].parent_mount,
                                     dcache_pool[i].parent_ino,
                                     dcache_pool[i].name);
            vfs_node_t *n = NULL;
            dcache_detach_entry_locked(b, &dcache_pool[i], &n);
            if (n && to_put && put_cnt < VFS_DCACHE_CAPACITY) {
                to_put[put_cnt++] = n;
            }
        }
    }
    spin_unlock(&dcache_lock);

    if (to_put) {
        for (int i = 0; i < put_cnt; i++) {
            vfs_node_put(to_put[i]);
        }
        kfree(to_put);
    }
}

void dcache_mutation_end_rename(vfs_node_t *olddir, const char *oldname,
                                vfs_node_t *newdir, const char *newname)
{
    (void)oldname;
    (void)newname;
    vfs_mount_t *m1 = olddir ? olddir->mount : NULL;
    vfs_mount_t *m2 = newdir ? newdir->mount : NULL;

    vfs_node_t **to_put = kmalloc(sizeof(vfs_node_t *) * VFS_DCACHE_CAPACITY);
    int put_cnt = 0;

    spin_lock(&dcache_lock);
    for (int i = 0; i < VFS_DCACHE_CAPACITY; i++) {
        if (!dcache_pool[i].in_use) continue;
        vfs_mount_t *pm = dcache_pool[i].parent_mount;
        vfs_mount_t *nm = dcache_pool[i].node ? dcache_pool[i].node->mount : NULL;
        if ((m1 && (pm == m1 || nm == m1)) ||
            (m2 && (pm == m2 || nm == m2))) {
            uint32_t b = dcache_hash(dcache_pool[i].parent_mount,
                                     dcache_pool[i].parent_ino,
                                     dcache_pool[i].name);
            vfs_node_t *n = NULL;
            dcache_detach_entry_locked(b, &dcache_pool[i], &n);
            if (n && to_put && put_cnt < VFS_DCACHE_CAPACITY) {
                to_put[put_cnt++] = n;
            }
        }
    }
    dcache_bump_gen_locked(to_put, &put_cnt);
    if (dcache_mut_active > 0) dcache_mut_active--;
    spin_unlock(&dcache_lock);

    if (to_put) {
        for (int i = 0; i < put_cnt; i++) {
            vfs_node_put(to_put[i]);
        }
        kfree(to_put);
    }
}

void dcache_get_stats(uint64_t *hits, uint64_t *misses, uint64_t *evictions)
{
    spin_lock(&dcache_lock);
    if (hits) *hits = dcache_hits;
    if (misses) *misses = dcache_misses;
    if (evictions) *evictions = dcache_evictions;
    spin_unlock(&dcache_lock);
}
```

And in `vfs_init()`, call `dcache_init();` right after `rwlock_init(&fs_type_lock);`.

---

### Task 2: Wire `dcache` into Path Walk (`__vfs_lookup_raw`), Mount Binding, and Namespace Mutations (`kernel/fs/vfs.c`, `kernel/selftest/symlink_selftest.c`)

**Files:**
- Modify: `kernel/fs/vfs.c`
- Modify: `kernel/selftest/symlink_selftest.c`

- [ ] **Step 1: Wire `dcache_invalidate` into `vfs_bind_submount_locked` and `dcache_lookup_begin` / `dcache_insert_validated` into `__vfs_lookup_raw` (`kernel/fs/vfs.c`)**

1. In `vfs_bind_submount_locked()` in [`kernel/fs/vfs.c`](../../../kernel/fs/vfs.c), right before `vfs_node_put(parent_dir);`, invalidate any cached entry for `(parent_dir, mp->mount_name)` so subsequent lookups cannot see a pre-mount cached directory node:
   ```c
   dcache_invalidate(parent_dir, mp->mount_name);
   // mp_node->parent holds a persistent reference to parent_dir.
   vfs_node_put(parent_dir);
   ```

2. In `__vfs_lookup_raw()` in [`kernel/fs/vfs.c`](../../../kernel/fs/vfs.c) (around lines 1053–1083), wrap the per-component lookup in `dcache_lookup_begin` and `dcache_insert_validated`:
   ```c
        vfs_node_t *child = NULL;
        vfs_mount_t *sub_mp = vfs_find_submount(cur, comp);
        if (sub_mp && sub_mp->root) {
            child = vfs_node_get(sub_mp->root);
        } else {
            uint64_t lookup_gen = 0;
            child = dcache_lookup_begin(cur, comp, &lookup_gen);
            if (!child) {
                int lrc;
                if (cur->i_ops && vfs_is_valid_kptr(cur->i_ops) &&
                    cur->i_ops->lookup && vfs_is_valid_kptr(cur->i_ops->lookup)) {
                    lrc = cur->i_ops->lookup(cur, comp, &child);
                } else {
                    lrc = vfs_generic_lookup_by_readdir(cur, comp, &child);
                }
                if (lrc < 0 || !child) {
                    vfs_node_put(cur);
                    ret = (lrc < 0) ? lrc : -ENOENT;
                    goto out;
                }
                if (!child->mount) child->mount = cur->mount;
                if (!child->parent) {
                    child->parent = vfs_node_get(cur);
                }
                if (!child->i_ops) child->i_ops = cur->i_ops;
                if (!child->f_ops) child->f_ops = cur->f_ops;
                vfs_active_node_register(child);

                if (child->mounted_here && child->mounted_here->root) {
                    vfs_node_t *sub_root = vfs_node_get(child->mounted_here->root);
                    vfs_node_put(child);
                    child = sub_root;
                } else {
                    dcache_insert_validated(cur, comp, child, lookup_gen);
                }
            } else if (child->mounted_here && child->mounted_here->root) {
                vfs_node_t *sub_root = vfs_node_get(child->mounted_here->root);
                vfs_node_put(child);
                child = sub_root;
            }
        }
   ```

- [ ] **Step 2: Wire `dcache` mutation barriers into `vfs_unlink()`, `vfs_rmdir()`, and `vfs_rename()` (`kernel/fs/vfs.c`)**

In [`kernel/fs/vfs.c`](../../../kernel/fs/vfs.c):

1. In `vfs_unlink()`:
   ```c
    dcache_mutation_begin();
    dcache_invalidate(parent, name);
    int ret = parent->i_ops->unlink(parent, name);
    dcache_mutation_end_invalidate(parent, name);
    vfs_node_put(parent);
    vfs_ns_unlock();
    return ret;
   ```

2. In `vfs_rmdir()`:
   ```c
    dcache_mutation_begin();
    dcache_invalidate(parent, name);
    int ret = parent->i_ops->rmdir(parent, name);
    dcache_mutation_end_invalidate(parent, name);
    vfs_node_put(parent);
    vfs_ns_unlock();
    return ret;
   ```

3. In `vfs_rename()`:
   ```c
    dcache_mutation_begin();
    dcache_invalidate(olddir, oldname);
    dcache_invalidate(newdir, newname);
    dcache_flush_mount(olddir->mount);
    int ret = olddir->i_ops->rename(olddir, oldname, newdir, newname);
    dcache_mutation_end_rename(olddir, oldname, newdir, newname);
    vfs_node_put(olddir);
    vfs_node_put(newdir);
    vfs_ns_unlock();
    return ret;
   ```

- [ ] **Step 3: Route `symlink_ext2_rollback` through `vfs_symlink` / `vfs_unlink` in `kernel/selftest/symlink_selftest.c`**

In [`kernel/selftest/symlink_selftest.c`](../../../kernel/selftest/symlink_selftest.c), `slt_fault_case` and `slt_success_case` previously called `root->i_ops->symlink(root, name, target)` and `root->i_ops->unlink(root, name)` directly, bypassing `vfs_unlink()` (which would leave the looked-up symlink cached in `dcache` with `refcount = 1` when `ext2_vfs_unlink` runs). Update lines 320–380 of `kernel/selftest/symlink_selftest.c` to invoke `vfs_symlink(target, abs_path, "/")` and `vfs_unlink(abs_path, "/")`:

```c
    ext2_symlink_test_set_fault(fault);
    int rc = vfs_symlink(target, abs_path, "/");
    ext2_symlink_test_set_fault(EXT2_SYMLINK_TEST_NONE);

    if (rc != -EIO) {
        serial_printk("[selftest] symlink_ext2_rollback: fault %d -> %d, expected -EIO\n",
                      (int)fault, rc);
        if (rc == 0) vfs_unlink(abs_path, "/");   // don't leak the dirent
        return -1;
    }
    if (!slt_name_absent(abs_path)) {
        serial_printk("[selftest] symlink_ext2_rollback: fault %d left a dirent\n",
                      (int)fault);
        vfs_unlink(abs_path, "/");
        return -1;
    }
```

And in `slt_success_case`:

```c
    int rc = vfs_symlink(target, abs_path, "/");
    if (rc != 0) {
        serial_printk("[selftest] symlink_ext2_rollback: symlink(%s) -> %d\n",
                      name, rc);
        return -1;
    }

    uint32_t used_blocks = free_blocks - fs->sb_raw.s_free_blocks_count;
    if (used_blocks != want_block_delta) {
        serial_printk("[selftest] symlink_ext2_rollback: %s used %u blocks, expected %u\n",
                      name, used_blocks, want_block_delta);
        vfs_unlink(abs_path, "/");
        return -1;
    }
    if (slt_name_absent(abs_path)) {
        serial_printk("[selftest] symlink_ext2_rollback: %s not found after create\n",
                      name);
        vfs_unlink(abs_path, "/");
        return -1;
    }

    rc = vfs_unlink(abs_path, "/");
    if (rc != 0) {
        serial_printk("[selftest] symlink_ext2_rollback: unlink(%s) -> %d\n",
                      name, rc);
        return -1;
    }
```

- [ ] **Step 4: Run `make test-host` and commit Tasks 1 & 2**

Run:
```bash
make test-host
git add kernel/include/fs/vfs.h hosttests/include/fs/vfs.h kernel/fs/vfs.c kernel/selftest/symlink_selftest.c
git commit -m "feat(vfs): add bounded 256-entry dcache with generation barrier (ARCH-5 Phase 4)"
```

---

### Task 3: Phase 4 Host Unit Tests (`test_vfs_dcache`) & Full Verification Gates

**Files:**
- Create: `hosttests/cases/test_vfs_dcache.c`
- Modify: `hosttests/Makefile`

- [ ] **Step 1: Create `hosttests/cases/test_vfs_dcache.c` covering all Spec §7.1 Phase 4 cases**

Create [`hosttests/cases/test_vfs_dcache.c`](../../../hosttests/cases/test_vfs_dcache.c):

```c
#include "test_framework.h"
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <fs/tmpfs.h>
#include <fs/ext2.h>
#include <fs/fat.h>
#include <block/blockdev.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static task_t s_test_task = {
    .pid = 1,
    .addr_limit = 0x00007fffffffffffULL,
};
task_t *vfs_test_current = &s_test_task;
int g_log_level = 0;
void _log_write(int level, const char *fmt, ...) { (void)level; (void)fmt; }
void _log_err_impl(const char *fmt, ...) { (void)fmt; }
void _log_warn_impl(const char *fmt, ...) { (void)fmt; }

int block_device_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    if (!dev || !dev->ops || !dev->ops->read) return -EIO;
    return dev->ops->read(dev, lba, count, buf);
}

int block_device_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    if (!dev || !dev->ops || !dev->ops->write) return -EIO;
    return dev->ops->write(dev, lba, count, buf);
}

uint64_t alloc_4k_page(void) { return (uint64_t)(uintptr_t)calloc(1, 4096); }
void free_4k_page(uint64_t addr) { free((void *)(uintptr_t)addr); }

void get_random_bytes(void *b, size_t n) { if (b && n) memset(b, 0x5A, n); }
void random_add_entropy(const void *d, size_t n) { (void)d; (void)n; }
int  user_write_range_begin(uint64_t a, size_t l) { (void)a; (void)l; return 0; }
void user_write_range_end(void) {}
int  user_read_range_begin(uint64_t a, size_t l) { (void)a; (void)l; return 0; }
void user_read_range_end(void) {}
void *keyboard_get_tty(void) { return NULL; }
char read_serial(void) { return 0; }
void write_serial(char c) { (void)c; }
tty_t *get_dev_tty(void) { return NULL; }

static int s_proc_lookup_calls = 0;
static int host_proc_lookup(struct vfs_node *dir, const char *name, struct vfs_node **out)
{
    s_proc_lookup_calls++;
    if (strcmp(name, "meminfo") != 0) return -ENOENT;
    vfs_node_t *n = (vfs_node_t *)calloc(1, sizeof(vfs_node_t));
    n->name = strdup("meminfo");
    n->type = VFS_FILE;
    n->ino = 100;
    n->mode = S_IFREG | 0444;
    n->nlink = 1;
    n->mount = dir->mount;
    n->parent = vfs_node_get(dir);
    n->i_ops = dir->i_ops;
    n->f_ops = dir->f_ops;
    n->refcount = 1;
    vfs_active_node_register(n);
    *out = n;
    return 0;
}

static const vfs_inode_ops_t s_ro_proc_iops = {
    .lookup = host_proc_lookup,
};
static const vfs_file_ops_t s_ro_proc_fops = {0};
static int host_procfs_mount(struct vfs_mount *mp, block_device_t *dev, void *data)
{
    (void)dev; (void)data;
    mp->i_ops = &s_ro_proc_iops;
    mp->f_ops = &s_ro_proc_fops;
    mp->root->i_ops = &s_ro_proc_iops;
    mp->root->f_ops = &s_ro_proc_fops;
    mp->root->ino = VFS_SYNTH_ROOT_INO;
    mp->root->mode = S_IFDIR | 0555;
    mp->root->nlink = 2;
    return 0;
}
static vfs_fs_type_t s_host_procfs_type = {
    .name     = "procfs",
    .fs_flags = VFS_FS_NO_DCACHE,
    .mount    = host_procfs_mount,
};
int procfs_register_fs(void) { return vfs_register_filesystem(&s_host_procfs_type); }

#define RAM_DISK_SECTOR_SIZE 512
#define RAM_DISK_SECTOR_COUNT 1024
static uint8_t s_ram_disk[RAM_DISK_SECTOR_SIZE * RAM_DISK_SECTOR_COUNT];

static int ram_disk_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    (void)dev;
    if (lba + count > RAM_DISK_SECTOR_COUNT) return -EIO;
    memcpy(buf, s_ram_disk + lba * RAM_DISK_SECTOR_SIZE, count * RAM_DISK_SECTOR_SIZE);
    return 0;
}

static int ram_disk_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    (void)dev;
    if (lba + count > RAM_DISK_SECTOR_COUNT) return -EIO;
    memcpy(s_ram_disk + lba * RAM_DISK_SECTOR_SIZE, buf, count * RAM_DISK_SECTOR_SIZE);
    return 0;
}

static const struct block_device_ops s_ram_disk_ops = {
    .read = ram_disk_read,
    .write = ram_disk_write,
};

static block_device_t s_ram_block_dev = {
    .name = "ram0",
    .sector_count = RAM_DISK_SECTOR_COUNT,
    .sector_size = RAM_DISK_SECTOR_SIZE,
    .present = 1,
    .kind = BLOCK_DISK,
    .ops = &s_ram_disk_ops,
};

static void format_mock_fat32_disk(void)
{
    memset(s_ram_disk, 0, sizeof(s_ram_disk));
    FAT32_BPB *bpb = (FAT32_BPB *)s_ram_disk;

    bpb->bytes_per_sector = 512;
    bpb->sectors_per_cluster = 1;
    bpb->reserved_sectors = 32;
    bpb->num_fats = 2;
    bpb->sectors_per_fat_32 = 16;
    bpb->root_cluster = 2;
    bpb->total_sectors_32 = 256;
    bpb->media = 0xF8;
    bpb->sectors_per_fat_16 = 0;

    s_ram_disk[510] = 0x55;
    s_ram_disk[511] = 0xAA;

    uint32_t fat_start = bpb->reserved_sectors;
    uint32_t *fat1 = (uint32_t *)(s_ram_disk + fat_start * 512);
    fat1[0] = 0x0FFFFFF8;
    fat1[1] = 0x0FFFFFFF;
    fat1[2] = FAT32_EOC_MIN;

    uint32_t *fat2 = (uint32_t *)(s_ram_disk + (fat_start + bpb->sectors_per_fat_32) * 512);
    fat2[0] = 0x0FFFFFF8;
    fat2[1] = 0x0FFFFFFF;
    fat2[2] = FAT32_EOC_MIN;
}

TEST_FUNC(test_dcache_hit_miss_and_case_insensitive_vfat) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));

    vfs_node_t *f = NULL;
    assert_eq(0, vfs_create_file("/alpha.txt", "/", &f));
    vfs_node_put(f);

    uint64_t hits = 0, misses = 0, evictions = 0;
    dcache_get_stats(&hits, &misses, &evictions);
    assert_eq(0, hits);

    /* First lookup misses dcache and inserts /alpha.txt */
    vfs_node_t *n1 = vfs_lookup("/alpha.txt");
    assert_not_null(n1);
    dcache_get_stats(&hits, &misses, &evictions);
    assert_eq(0, hits);
    assert_true(misses >= 1);

    /* Second lookup hits dcache and returns the exact same cached vfs_node_t */
    uint64_t prev_misses = misses;
    vfs_node_t *n2 = vfs_lookup("/alpha.txt");
    assert_not_null(n2);
    assert_true(n1 == n2);
    dcache_get_stats(&hits, &misses, &evictions);
    assert_eq(1, hits);
    assert_eq(prev_misses, misses);
    vfs_node_put(n2);
    vfs_node_put(n1);

    /* Case-insensitive vfat mount must hit dcache across case variants */
    vfs_init();
    format_mock_fat32_disk();
    assert_eq(0, vfs_mount_fs("vfat", "/", &s_ram_block_dev, NULL));

    vfs_node_t *vf = NULL;
    assert_eq(0, vfs_create_file("/README.TXT", "/", &vf));
    vfs_node_put(vf);

    vfs_node_t *v1 = vfs_lookup("/README.TXT");
    assert_not_null(v1);
    dcache_get_stats(&hits, &misses, &evictions);
    uint64_t hits_after_v1 = hits;

    vfs_node_t *v2 = vfs_lookup("/readme.txt");
    assert_not_null(v2);
    assert_true(v1 == v2);
    dcache_get_stats(&hits, &misses, &evictions);
    assert_eq(hits_after_v1 + 1, hits);

    vfs_node_t *v3 = vfs_lookup("/ReAdMe.TxT");
    assert_not_null(v3);
    assert_true(v1 == v3);
    dcache_get_stats(&hits, &misses, &evictions);
    assert_eq(hits_after_v1 + 2, hits);

    vfs_node_put(v3);
    vfs_node_put(v2);
    vfs_node_put(v1);
}

TEST_FUNC(test_dcache_lru_256_capacity_eviction_and_submount_parent_rebuild) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));
    assert_eq(0, vfs_mkdir("/mnt", "/"));
    assert_eq(0, vfs_mkdir("/mnt/usb", "/"));
    assert_eq(0, vfs_mount_fs("tmpfs", "/mnt/usb", NULL, NULL));

    vfs_node_t *sub_f = NULL;
    assert_eq(0, vfs_create_file("/mnt/usb/payload.txt", "/", &sub_f));
    assert_eq(7, vfs_write(sub_f, 0, 7, "USB_256"));
    vfs_node_put(sub_f);

    /* Create and lookup 300 files in "/" to overflow the 256-entry dcache */
    vfs_node_t *first_cached = NULL;
    for (int i = 0; i < 300; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/lru_%03d", i);
        vfs_node_t *created = NULL;
        assert_eq(0, vfs_create_file(path, "/", &created));
        vfs_node_put(created);

        vfs_node_t *looked = vfs_lookup(path);
        assert_not_null(looked);
        if (i == 0) {
            /* Keep an extra ref on /lru_000 to verify refcount balance after LRU eviction */
            first_cached = vfs_node_get(looked);
            assert_eq(3u, first_cached->refcount); /* dcache(1) + looked(1) + first_cached(1) */
        }
        vfs_node_put(looked);
    }

    uint64_t hits = 0, misses = 0, evictions = 0;
    dcache_get_stats(&hits, &misses, &evictions);
    assert_true(evictions >= 44);

    /* /lru_000 was evicted from dcache, so only first_cached's ref (1) remains */
    assert_eq(1u, first_cached->refcount);
    vfs_node_put(first_cached);

    /* /mnt was also evicted from dcache; looking up /mnt/usb/payload.txt must rebuild /mnt and cross submount */
    vfs_node_t *re_usb = vfs_lookup("/mnt/usb/payload.txt");
    assert_not_null(re_usb);
    char buf[16] = {0};
    assert_eq(7, vfs_read(re_usb, 0, sizeof(buf), buf));
    assert_str_eq("USB_256", buf);
    vfs_node_put(re_usb);
}

TEST_FUNC(test_dcache_concurrent_lookup_unlink_generation_barrier) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));

    vfs_node_t *created = NULL;
    assert_eq(0, vfs_create_file("/race.txt", "/", &created));
    assert_eq(9, vfs_write(created, 0, 9, "RACE_DATA"));
    vfs_node_put(created);

    vfs_node_t *root = vfs_lookup("/");
    assert_not_null(root);

    /* Step 1: CPU A starts lookup, misses dcache, and obtains lookup_gen */
    uint64_t lookup_gen = 0;
    vfs_node_t *cached = dcache_lookup_begin(root, "race.txt", &lookup_gen);
    assert_null(cached);
    assert_true(lookup_gen != 0);

    /* Step 2: CPU A completes backend i_ops->lookup and registers child */
    vfs_node_t *child_a = NULL;
    assert_eq(0, root->i_ops->lookup(root, "race.txt", &child_a));
    assert_not_null(child_a);
    vfs_active_node_register(child_a);

    /* Step 3: CPU B unlinks /race.txt before CPU A calls dcache_insert_validated */
    assert_eq(0, vfs_unlink("/race.txt", "/"));
    assert_true(child_a->node_flags & VFS_NODE_ORPHANED);

    /* Step 4: CPU A attempts dcache_insert_validated with stale lookup_gen -> MUST be rejected */
    dcache_insert_validated(root, "race.txt", child_a, lookup_gen);
    assert_eq(1u, child_a->refcount); /* dcache did NOT take a ref */

    /* Verify /race.txt is NOT in dcache and vfs_lookup returns NULL */
    assert_null(vfs_lookup("/race.txt"));

    /* Verify CPU A can still safely read its open orphan handle before final put */
    char buf[16] = {0};
    assert_eq(9, vfs_read(child_a, 0, sizeof(buf), buf));
    assert_str_eq("RACE_DATA", buf);

    vfs_node_put(child_a);
    vfs_node_put(root);
}

TEST_FUNC(test_dcache_metadata_stat_consistency_across_write_and_truncate) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));

    vfs_node_t *f = NULL;
    assert_eq(0, vfs_create_file("/meta.bin", "/", &f));
    vfs_node_put(f);

    /* Populate dcache by looking up /meta.bin */
    vfs_node_t *cached_h = vfs_lookup("/meta.bin");
    vfs_node_t *open_h = vfs_lookup("/meta.bin");
    assert_not_null(cached_h);
    assert_true(cached_h == open_h);

    struct stat st;
    assert_eq(0, vfs_stat(cached_h, &st));
    assert_eq(0, st.st_size);

    /* Write 200 bytes via open_h */
    char data[200];
    memset(data, 'M', sizeof(data));
    assert_eq(200, vfs_write(open_h, 0, sizeof(data), data));

    struct stat st_cached, st_open, st_rebuilt;
    assert_eq(0, vfs_stat(cached_h, &st_cached));
    assert_eq(0, vfs_stat(open_h, &st_open));

    /* Evict from dcache and lookup a fresh node to verify rebuilt node matches too */
    dcache_flush_mount(cached_h->mount);
    vfs_node_t *rebuilt_h = vfs_lookup("/meta.bin");
    assert_not_null(rebuilt_h);
    assert_eq(0, vfs_stat(rebuilt_h, &st_rebuilt));

    assert_eq(200, st_cached.st_size);
    assert_eq(200, st_open.st_size);
    assert_eq(200, st_rebuilt.st_size);

    /* Truncate to 48 bytes and verify all three again */
    assert_eq(0, vfs_truncate(open_h, 48));
    assert_eq(0, vfs_stat(cached_h, &st_cached));
    assert_eq(0, vfs_stat(open_h, &st_open));
    dcache_flush_mount(cached_h->mount);
    vfs_node_put(rebuilt_h);
    rebuilt_h = vfs_lookup("/meta.bin");
    assert_not_null(rebuilt_h);
    assert_eq(0, vfs_stat(rebuilt_h, &st_rebuilt));

    assert_eq(48, st_cached.st_size);
    assert_eq(48, st_open.st_size);
    assert_eq(48, st_rebuilt.st_size);

    vfs_node_put(rebuilt_h);
    vfs_node_put(open_h);
    vfs_node_put(cached_h);
}

TEST_FUNC(test_dcache_rename_directory_flushes_descendant_parent_chain) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));
    assert_eq(0, vfs_mkdir("/a", "/"));
    assert_eq(0, vfs_mkdir("/a/d", "/"));
    assert_eq(0, vfs_mkdir("/a/d/sub", "/"));
    assert_eq(0, vfs_mkdir("/b", "/"));

    /* Warm dcache with /a/d/sub */
    vfs_node_t *old_sub = vfs_lookup("/a/d/sub");
    assert_not_null(old_sub);
    char path_before[128] = {0};
    assert_true(vfs_resolve_path(old_sub, path_before, sizeof(path_before)) > 0);
    assert_str_eq("/a/d/sub", path_before);
    vfs_node_put(old_sub);

    /* Move /a/d -> /b/d */
    assert_eq(0, vfs_rename("/a/d", "/b/d", "/"));
    assert_null(vfs_lookup("/a/d/sub"));

    /* Lookup /b/d/sub: must NOT have stale parent pointing to /a/d */
    vfs_node_t *new_sub = vfs_lookup("/b/d/sub");
    assert_not_null(new_sub);
    char path_after[128] = {0};
    assert_true(vfs_resolve_path(new_sub, path_after, sizeof(path_after)) > 0);
    assert_str_eq("/b/d/sub", path_after);
    vfs_node_put(new_sub);

    vfs_node_t *up_two = vfs_lookup("/b/d/sub/../..");
    assert_not_null(up_two);
    assert_str_eq("b", up_two->name);
    vfs_node_put(up_two);
}

TEST_FUNC(test_dcache_no_dcache_bypass_devfs_procfs_and_node_flag) {
    vfs_init();
    devfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));
    assert_eq(0, vfs_mount_fs("procfs", "/proc", NULL, NULL));

    uint64_t h0 = 0, m0 = 0, e0 = 0;
    dcache_get_stats(&h0, &m0, &e0);

    s_proc_lookup_calls = 0;
    vfs_node_t *p1 = vfs_lookup("/proc/meminfo");
    assert_not_null(p1);
    vfs_node_t *p2 = vfs_lookup("/proc/meminfo");
    assert_not_null(p2);
    /* procfs has VFS_FS_NO_DCACHE, so host_proc_lookup must be called every time */
    assert_eq(2, s_proc_lookup_calls);
    assert_true(p1 != p2);
    vfs_node_put(p2);
    vfs_node_put(p1);

    /* devfs also has VFS_FS_NO_DCACHE */
    vfs_node_t *d1 = vfs_lookup("/dev/null");
    vfs_node_t *d2 = vfs_lookup("/dev/null");
    assert_not_null(d1);
    assert_not_null(d2);
    assert_true(d1 != d2);
    vfs_node_put(d2);
    vfs_node_put(d1);

    uint64_t h1 = 0, m1 = 0, e1 = 0;
    dcache_get_stats(&h1, &m1, &e1);
    assert_eq(h0, h1);
    assert_eq(m0, m1);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_dcache_hit_miss_and_case_insensitive_vfat),
    TEST_ENTRY(test_dcache_lru_256_capacity_eviction_and_submount_parent_rebuild),
    TEST_ENTRY(test_dcache_concurrent_lookup_unlink_generation_barrier),
    TEST_ENTRY(test_dcache_metadata_stat_consistency_across_write_and_truncate),
    TEST_ENTRY(test_dcache_rename_directory_flushes_descendant_parent_chain),
    TEST_ENTRY(test_dcache_no_dcache_bypass_devfs_procfs_and_node_flag),
TEST_LIST_END

int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
```

- [ ] **Step 2: Wire `test_vfs_dcache.elf` into `hosttests/Makefile`**

In [`hosttests/Makefile`](../../../hosttests/Makefile):
1. Add `$(TEST_BLD)/test_vfs_dcache.elf \` right after `$(TEST_BLD)/test_vfs_mount_tree.elf \` in `TEST_BINS` (around line 74).
2. Add the compile and link rules right after `$(TEST_BLD)/test_vfs_mount_tree.elf` (around line 635):

```makefile
$(TEST_BLD)/test_vfs_dcache.o: $(TEST_CASES)/test_vfs_dcache.c \
		$(TESTS_DIR)/kernel/include/fs/vfs.h \
		$(TEST_MOCK)/vfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VFS_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_vfs_dcache.elf: $(TEST_BLD)/test_vfs_dcache.o \
		$(TEST_BLD)/vfs_production.o \
		$(TEST_BLD)/ext2_production.o \
		$(TEST_BLD)/fat_production.o \
		$(TEST_BLD)/tmpfs_production.o \
		$(TEST_BLD)/devfs_vfs_production.o \
		$(TEST_BLD)/serial_sink_stub.o \
		$(TEST_BLD)/rwlock.o \
		$(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -Wl,--gc-sections -o $@ $^
```

- [ ] **Step 3: Run full verification gates**

Run all required verification gates (including `make clean` on both profiles per AGENTS.md after `vfs.h` changes):
```bash
make PROFILE=x86_64-clang clean
make PROFILE=aarch64-clang clean
make test-host
make test-static
make PROFILE=x86_64-clang kernel
make PROFILE=aarch64-clang kernel
make test-qemu SUITE=phase-0
make KERNEL_SELFTEST=1 test-kernel-selftest
make OS01_SYSTEST=1 test-syscall
```
Expected: All host unit tests (including `test_vfs_dcache`), static audits, both architecture kernel builds, `phase-0`, `test-kernel-selftest` (including `symlink_ext2_rollback`), and `test-syscall` pass with 0 failures.

- [ ] **Step 4: Stage and commit Task 3 (avoiding unrelated `fb_resolution` hunks in `hosttests/Makefile`)**

Stage `hosttests/cases/test_vfs_dcache.c` and only the `test_vfs_dcache` hunks in `hosttests/Makefile`, then commit:
```bash
git add hosttests/cases/test_vfs_dcache.c
# Stage only the test_vfs_dcache hunks in hosttests/Makefile
git commit -m "test(vfs): add host unit tests for Phase 4 bounded dcache"
```
