# ARCH-5 Phase 2B (`active_nodes` + `evict_node` + `tmpfs`/`ext2`/`fat32` 孤儿延迟回收与 `fat_file_object`) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:subagent-driven-development` (recommended) or `superpowers:executing-plans` to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the second half of ARCH-5 Phase 2 (Spec §4.3, §4.4, §5.2.2, §5.2.3, §5.5, §7.1): mount-level `active_nodes` registration under backend lifecycle locks, `i_ops->evict_node` sole-reclaimer protocol, `tmpfs` node reference counting, `ext2` deferred orphan inode/block reclamation, `fat32` shared `fat_file_object` lifecycle with monotonic `object_id` and lock-protected cluster access, active-node live size synchronization, and the complete Phase 2B host unit test suite.

**Architecture:**
- `vfs_mount_t.active_nodes` tracks all live non-root `vfs_node_t` instances (including `refcount == 0` nodes awaiting `evict_node`) protected by `mp->active_lock`.
- Lock ordering is strictly: `vfs_namespace_lock` (mutex) $\rightarrow$ backend lifecycle lock (`tmpfs_lock` / `ext2_fs_t.lock` / `fat32_fs_t.fat_lifecycle_lock`) $\rightarrow$ `mp->active_lock`. Memory allocations, disk I/O, and recursive `vfs_node_put(parent)` NEVER run while holding `mp->active_lock`.
- Every backend node constructor (`lookup`, `create`, `mkdir`) pre-allocates candidate memory outside the backend spinlock, then inside the backend spinlock locates/publishes the directory entry, acquires/initializes the underlying object reference, and registers the `vfs_node_t` in `mp->active_nodes` before releasing the backend spinlock.
- When `unlink`/`rmdir`/`rename`-overwrite removes the last link of a file/dir, it marks all matching active `vfs_node_t` instances with `VFS_NODE_ORPHANED` and `nlink = 0` under ` backend_lock -> mp->active_lock`. If no active registrations exist (including zero-ref pending-evict nodes), `unlink` is the **sole reclaimer** and frees the underlying storage immediately; otherwise reclamation is deferred until the last active registration is removed in `i_ops->evict_node`.
- In `fat32`, regular files use a backend-private shared `fat_file_object_t` stored in `node->fs_data` with a monotonic `object_id >= (1ULL << 32)` as `node->ino` (directories continue to use their cluster number). Multiple handles to the same file share one `fat_file_object_t`; `fat_read`, `fat_write`, `fat_truncate`, and `fat_iop_getattr` hold `fs->fat_lifecycle_lock` across the entire operation through the final cluster access.

**Tech Stack:** Freestanding C11 (`clang`, `lld`), OS01 VFS (`kernel/fs/`), Host unit test harness (`hosttests/`), QEMU x86_64 & aarch64 verification suites.

**Spec Reference:** [`docs/superpowers/specs/2026-10-24-arch5-vfs-abstraction-design.md`](../specs/2026-10-24-arch5-vfs-abstraction-design.md) (§4.3, §4.4, §5.2.2, §5.2.3, §5.5, §6, §7).

---

## File Map (Phase 2B Scope)

| File | Action | Responsibility |
|------|--------|----------------|
| `kernel/include/fs/vfs.h` | Modify | Declare `vfs_active_node_*` helpers and `vfs_node_live_size` |
| `hosttests/include/fs/vfs.h` | Modify | Sync `vfs_mount_t` (`active_lock`, `active_nodes`), `vfs_node_t` (`mnt_active_next`, `VFS_NODE_*`), and Phase 2B helper declarations |
| `kernel/fs/vfs.c` | Modify | Implement `vfs_active_node_*` helpers, `vfs_node_live_size`, register nodes in `vfs_generic_lookup_by_readdir`, and update `vfs_node_put` eviction dispatch |
| `kernel/syscall/sys_fs.c` | Modify | Use `vfs_node_live_size(f->node)` in `SYS_lseek` (`SEEK_END`) |
| `kernel/fs/elf.c` | Modify | Use `vfs_node_live_size(node)` in `elf_load` |
| `kernel/fs/tmpfs.c` | Modify | Add `tmpfs_lock` (`spinlock_T`), `tmpfs_node_t.refcount`, atomic lookup/create/mkdir registration, orphan handling, size sync, and `tmpfs_iop_evict_node` |
| `kernel/include/fs/ext2.h` | Modify | Add in-memory orphan tracking fields to `ext2_fs_t` |
| `kernel/fs/ext2.c` | Modify | Atomic `active_nodes` registration under `fs->lock`, deferred orphan reclamation in `unlink`/`rmdir`/`rename` and `ext2_iop_evict_node`, and active-node size sync |
| `kernel/include/fs/fat.h` | Modify | Define `fat_file_object_t` and add `fat_lifecycle_lock`, `next_object_id`, `file_objects` to `fat32_fs_t` |
| `kernel/fs/fat.c` | Modify | Migrate regular files to shared `fat_file_object_t` under `fat_lifecycle_lock`, implement `fat_iop_evict_node`, deferred orphan cluster reclamation, and active-node size sync |
| `hosttests/mock/vfs_test_runtime.h` | Modify | Track `spinlock_T.lock` state (`1` unlocked, `0` locked) and provide weak observation hooks for deterministic lock/race tests |
| `hosttests/cases/test_vfs_iops_fops.c` | Modify | Add in-memory `ext2` and `vfat` block device fixtures and all Phase 2B lifecycle/orphan/race/FAT-object tests |

---

## Task 1: VFS Active Node Registry, `vfs_node_live_size`, and `tmpfs` Refcounted Lifecycle

**Files:**
- Modify: `kernel/include/fs/vfs.h`
- Modify: `hosttests/include/fs/vfs.h`
- Modify: `kernel/fs/vfs.c`
- Modify: `kernel/syscall/sys_fs.c`
- Modify: `kernel/fs/elf.c`
- Modify: `kernel/fs/tmpfs.c`

- [ ] **Step 1: Add `vfs_active_node_*` and `vfs_node_live_size` declarations to `kernel/include/fs/vfs.h` and `hosttests/include/fs/vfs.h`**

In `kernel/include/fs/vfs.h`, add `<stdbool.h>` at the top (after `<stddef.h>`) and add the following declarations right after `vfs_symlink` (around line 188):

```c
// Active node tracking & live metadata helpers (Spec §4.4, §5.2.2, §5.2.3)
void     vfs_active_node_register(struct vfs_node *node);
void     vfs_active_node_unregister_locked(struct vfs_mount *mp, struct vfs_node *node);
void     vfs_active_node_unregister(struct vfs_node *node);
bool     vfs_active_nodes_has_ino_locked(struct vfs_mount *mp, uint64_t ino,
                                         const struct vfs_node *exclude);
void     vfs_active_nodes_mark_orphaned_locked(struct vfs_mount *mp, uint64_t ino);
void     vfs_active_nodes_sync_size_locked(struct vfs_mount *mp, uint64_t ino,
                                           uint64_t new_size, uint64_t new_mtime);
uint64_t vfs_node_live_size(struct vfs_node *node);
```

In `hosttests/include/fs/vfs.h`, sync `vfs_mount_t`, `vfs_node_t`, `VFS_NODE_*`, `vfs_node_stable_ino`, and the new helper declarations so `hosttests/include/fs/vfs.h` has:

```c
#include <stdbool.h>
```
and inside `typedef struct vfs_mount`:
```c
typedef struct vfs_mount {
    block_device_t            *dev;
    const struct vfs_fs_type  *fs_type;
    uint32_t                   mnt_flags;
    const char                *path;
    char                       mount_name[VFS_NAME_MAX];
    struct vfs_node           *root;
    struct vfs_node           *mountpoint;
    struct vfs_mount          *parent_mount;
    uint64_t                   parent_dir_ino;
    const vfs_inode_ops_t     *i_ops;
    const vfs_file_ops_t      *f_ops;
    void                      *fs_data;
    spinlock_T                 active_lock;
    struct vfs_node           *active_nodes;
    struct vfs_mount          *next;
} vfs_mount_t;
```
and inside `typedef struct vfs_node`:
```c
#define VFS_NODE_NO_DCACHE   (1u << 0)
#define VFS_NODE_MOUNT_STUB  (1u << 1)
#define VFS_NODE_ORPHANED    (1u << 2)
#define VFS_NODE_STALE       (1u << 3)

typedef struct vfs_node {
    char                  name[VFS_NAME_MAX];
    uint64_t              ino;
    uint64_t              size;
    uint32_t              mode;
    uint32_t              uid;
    uint32_t              gid;
    uint32_t              nlink;
    uint64_t              rdev;
    uint64_t              atime;
    uint64_t              mtime;
    uint64_t              ctime;
    uint8_t               type;
    uint8_t               node_flags;
    struct vfs_mount     *mount;
    struct vfs_mount     *mounted_here;
    void                 *fs_data;
    struct vfs_node      *parent;
    const vfs_inode_ops_t *i_ops;
    const vfs_file_ops_t  *f_ops;
    uint32_t              refcount;
    struct vfs_node      *mnt_active_next;
} vfs_node_t;

#define VFS_SYNTH_ROOT_INO       1ULL
#define VFS_MOUNT_STUB_INO_BASE  0x8000000000000000ULL

static inline uint64_t vfs_node_stable_ino(const vfs_node_t *node)
{
    if (!node) return 0;
    if (node->ino != 0) return node->ino;
    if (node->mount && node == node->mount->root) return VFS_SYNTH_ROOT_INO;
    return (uint64_t)(uintptr_t)node->fs_data;
}
```
plus the declarations of `vfs_active_node_register`, `vfs_active_node_unregister_locked`, `vfs_active_node_unregister`, `vfs_active_nodes_has_ino_locked`, `vfs_active_nodes_mark_orphaned_locked`, `vfs_active_nodes_sync_size_locked`, and `vfs_node_live_size`.

- [ ] **Step 2: Implement `vfs_active_node_*`, `vfs_node_live_size`, and update `vfs_node_put` in `kernel/fs/vfs.c`**

In `kernel/fs/vfs.c`:

1. Add weak test observation hook declaration near the top of `kernel/fs/vfs.c` (under `#ifdef OS01_HOST_TEST`):

```c
#ifdef OS01_HOST_TEST
void vfs_test_on_active_register(vfs_node_t *node) __attribute__((weak));
#endif
```

2. Implement the active node registry helpers right before `vfs_node_get` (around line 1158):

```c
// ── Active node registry (Spec §4.4, §5.2.2, §5.2.3) ─────
void vfs_active_node_register(vfs_node_t *node)
{
    if (!node || !node->mount) return;
    vfs_mount_t *mp = node->mount;
    if (node == mp->root) return;

#ifdef OS01_HOST_TEST
    if (vfs_test_on_active_register) {
        vfs_test_on_active_register(node);
    }
#endif

    spin_lock(&mp->active_lock);
    // Guard against duplicate registration of the same pointer
    for (vfs_node_t *cur = mp->active_nodes; cur; cur = cur->mnt_active_next) {
        if (cur == node) {
            spin_unlock(&mp->active_lock);
            return;
        }
    }
    node->mnt_active_next = mp->active_nodes;
    mp->active_nodes = node;
    spin_unlock(&mp->active_lock);
}

void vfs_active_node_unregister_locked(vfs_mount_t *mp, vfs_node_t *node)
{
    if (!mp || !node) return;
    vfs_node_t **pp = &mp->active_nodes;
    while (*pp) {
        if (*pp == node) {
            *pp = node->mnt_active_next;
            node->mnt_active_next = NULL;
            return;
        }
        pp = &(*pp)->mnt_active_next;
    }
}

void vfs_active_node_unregister(vfs_node_t *node)
{
    if (!node || !node->mount) return;
    vfs_mount_t *mp = node->mount;
    spin_lock(&mp->active_lock);
    vfs_active_node_unregister_locked(mp, node);
    spin_unlock(&mp->active_lock);
}

bool vfs_active_nodes_has_ino_locked(vfs_mount_t *mp, uint64_t ino,
                                     const vfs_node_t *exclude)
{
    if (!mp || ino == 0) return false;
    for (vfs_node_t *cur = mp->active_nodes; cur; cur = cur->mnt_active_next) {
        if (cur == exclude) continue;
        if (vfs_node_stable_ino(cur) == ino) return true;
    }
    return false;
}

void vfs_active_nodes_mark_orphaned_locked(vfs_mount_t *mp, uint64_t ino)
{
    if (!mp || ino == 0) return;
    for (vfs_node_t *cur = mp->active_nodes; cur; cur = cur->mnt_active_next) {
        if (vfs_node_stable_ino(cur) == ino) {
            cur->node_flags |= VFS_NODE_ORPHANED;
            cur->nlink = 0;
        }
    }
}

void vfs_active_nodes_sync_size_locked(vfs_mount_t *mp, uint64_t ino,
                                       uint64_t new_size, uint64_t new_mtime)
{
    if (!mp || ino == 0) return;
    if (mp->root && vfs_node_stable_ino(mp->root) == ino) {
        mp->root->size = new_size;
        if (new_mtime != 0) mp->root->mtime = new_mtime;
    }
    for (vfs_node_t *cur = mp->active_nodes; cur; cur = cur->mnt_active_next) {
        if (vfs_node_stable_ino(cur) == ino) {
            cur->size = new_size;
            if (new_mtime != 0) cur->mtime = new_mtime;
        }
    }
}

uint64_t vfs_node_live_size(vfs_node_t *node)
{
    if (!node) return 0;
    struct stat st;
    if (vfs_stat(node, &st) == 0) {
        return (uint64_t)st.st_size;
    }
    return node->size;
}
```

3. In `vfs_generic_lookup_by_readdir` (`kernel/fs/vfs.c` around line 612), register `child` before returning:

```c
    vfs_active_node_register(child);
    *out = child;
    return 0;
```

4. In `__vfs_lookup_raw` (`kernel/fs/vfs.c` around line 774), ensure `vfs_active_node_register(child)` is called if a custom test stub lookup did not already register `child` (idempotent because `vfs_active_node_register` checks for duplicates):

```c
        if (!child->mount) child->mount = mp;
        if (!child->parent) {
            child->parent = cur;
            __sync_add_and_fetch(&cur->refcount, 1);
        }
        if (!child->i_ops) child->i_ops = cur->i_ops;
        if (!child->f_ops) child->f_ops = cur->f_ops;
        vfs_active_node_register(child);
```

5. In `vfs_node_put` (`kernel/fs/vfs.c` lines 1165-1187), update the eviction dispatch per Spec §5.2.2 (never call disk `evict_node` on synthetic mount stubs; backends with `evict_node` unregister inside their own lock critical section; backends without `evict_node` use generic `vfs_active_node_unregister`):

```c
void vfs_node_put(vfs_node_t *node)
{
    if (!node) return;
    if (__sync_sub_and_fetch(&node->refcount, 1) == 0) {
        if (node->node_flags & VFS_NODE_MOUNT_STUB) {
            vfs_active_node_unregister(node);
        } else if (node->i_ops && vfs_is_valid_kptr(node->i_ops) &&
                   node->i_ops->evict_node && vfs_is_valid_kptr(node->i_ops->evict_node)) {
            node->i_ops->evict_node(node);
        } else {
            vfs_active_node_unregister(node);
        }
        vfs_node_t *parent = node->parent;
        // Poison to catch use-after-free: any stale reference
        // will hit a null-pointer check (unlike kernel-address
        // guards which the optimizer may elide).
        node->i_ops   = NULL;
        node->f_ops   = NULL;
        node->parent  = NULL;
        node->mount   = NULL;
        node->fs_data = NULL;
        if (node->name) kfree(node->name);
        node->name = NULL;
        free(node);
        vfs_node_put(parent);  // release parent ref after child is gone
    }
}
```

- [ ] **Step 3: Update `SYS_lseek` (`kernel/syscall/sys_fs.c`) and `elf_load` (`kernel/fs/elf.c`) to use `vfs_node_live_size`**

In `kernel/syscall/sys_fs.c` around line 600:
```c
        case SEEK_END:
            new_offset = (int64_t)vfs_node_live_size(f->node) + offset;
            break;
```

In `kernel/fs/elf.c` around line 153:
```c
    uint64_t file_size = vfs_node_live_size(node);
```

- [ ] **Step 4: Implement `tmpfs_lock`, `tmpfs_node_t.refcount`, `active_nodes` registration, and `tmpfs_iop_evict_node` in `kernel/fs/tmpfs.c`**

Update `kernel/fs/tmpfs.c`:

1. Add `uint32_t refcount;` to `typedef struct tmpfs_node` and define `static spinlock_T tmpfs_lock = { 1 };` (plus a test counter `uint64_t tmpfs_test_freed_nodes` under `#ifdef OS01_HOST_TEST` if useful for verification):

```c
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
```

2. Add reference counting and node destruction helpers in `kernel/fs/tmpfs.c`:

```c
static inline void tmpfs_node_get_locked(tmpfs_node_t *tn)
{
    if (tn) __sync_add_and_fetch(&tn->refcount, 1);
}

static inline uint32_t tmpfs_node_put_locked(tmpfs_node_t *tn)
{
    if (!tn) return 1;
    return __sync_sub_and_fetch(&tn->refcount, 1);
}

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
```

3. Update `tmpfs_vfs_read`, `tmpfs_vfs_write`, `tmpfs_vfs_readdir`, `tmpfs_vfs_create`, `tmpfs_vfs_mkdir`, `tmpfs_vfs_unlink`, `tmpfs_vfs_rmdir`, `tmpfs_vfs_rename`, `tmpfs_vfs_truncate`, `tmpfs_iop_lookup`, `tmpfs_iop_getattr`, `tmpfs_iop_evict_node`, and `tmpfs_fs_mount`:
   - **`tmpfs_vfs_read`**: guard block lookup & copy under `spin_lock(&tmpfs_lock)` / `spin_unlock(&tmpfs_lock)`.
   - **`tmpfs_vfs_write`**: under `spin_lock(&tmpfs_lock)`, grow blocks, copy data, update `tn->size`, set `node->size = tn->size`, and if `node->mount` is non-NULL, acquire `spin_lock(&node->mount->active_lock)` and call `vfs_active_nodes_sync_size_locked(node->mount, (uint64_t)(uintptr_t)tn, tn->size, tn->mtime)` then `spin_unlock(&node->mount->active_lock)`, then `spin_unlock(&tmpfs_lock)`.
   - **`tmpfs_vfs_truncate`**: under `spin_lock(&tmpfs_lock)`, truncate blocks, set `tn->size = new_size; node->size = new_size;`, sync active nodes under `node->mount->active_lock`, then `spin_unlock(&tmpfs_lock)`.
   - **`tmpfs_vfs_readdir`**: guard directory read under `spin_lock(&tmpfs_lock)`.
   - **`tmpfs_vfs_create` & `tmpfs_vfs_mkdir`**:
     - Pre-allocate `tmpfs_node_t *new_node = calloc(1, sizeof(tmpfs_node_t))`, `struct vfs_node *vn = calloc(1, sizeof(struct vfs_node))`, and `char *vn_name = strdup(name)` BEFORE acquiring `tmpfs_lock` (if any fails, free all and return `NULL`).
     - Acquire `spin_lock(&tmpfs_lock)`:
       - Check `if (!d || d->type != VFS_DIR || tmpfs_find_child(d, name) >= 0)` $\rightarrow$ `spin_unlock(&tmpfs_lock)`, free `vn_name`, `vn`, `new_node`, return `NULL`.
       - Expand `d->children` if `d->child_count >= d->child_cap` (if `kmalloc` fails, unlock, free `vn_name`, `vn`, `new_node`, return `NULL`).
       - Initialize `new_node`: `new_node->refcount = 2` (1 for `d->children[]` + 1 for `vn`), `new_node->type`, `new_node->mode`, `new_node->nlink` (`1` for file, `2` for dir), `new_node->parent = d`.
       - `d->children[d->child_count++] = new_node;`
       - Initialize `vn`: `vn->name = vn_name`, `vn->type`, `vn->size = 0`, `vn->mode = new_node->mode`, `vn->nlink = new_node->nlink`, `vn->fs_data = new_node`, `vn->i_ops = &tmpfs_iops`, `vn->f_ops = &tmpfs_fops`, `vn->ino = (uint64_t)(uintptr_t)new_node`, `vn->mount = dir->mount`, `vn->parent = vfs_node_get(dir)`, `vn->refcount = 1`.
       - Call `vfs_active_node_register(vn)` while still holding `tmpfs_lock`!
     - `spin_unlock(&tmpfs_lock); return vn;`
   - **`tmpfs_iop_lookup`**:
     - Pre-allocate `struct vfs_node *vn = calloc(1, sizeof(struct vfs_node))` and `char *vn_name = kmalloc(VFS_NAME_MAX)` BEFORE acquiring `tmpfs_lock`.
     - Acquire `spin_lock(&tmpfs_lock)`:
       - `int idx = tmpfs_find_child(d, name);`
       - If `idx < 0`: `spin_unlock(&tmpfs_lock); kfree(vn_name); free(vn); return -ENOENT;`
       - `tmpfs_node_t *tn = d->children[idx];`
       - `tmpfs_node_get_locked(tn);` (atomic ref acquisition under `tmpfs_lock`!)
       - Copy `tn->name` into `vn_name`, set `vn->name = vn_name`, populate `vn` fields from `tn`, set `vn->parent = vfs_node_get(dir)`, `vn->refcount = 1`.
       - Call `vfs_active_node_register(vn)` while still holding `tmpfs_lock`!
     - `spin_unlock(&tmpfs_lock); *out = vn; return 0;`
   - **`tmpfs_vfs_unlink` & `tmpfs_vfs_rmdir`**:
     - Acquire `spin_lock(&tmpfs_lock)`.
     - Locate `idx = tmpfs_find_child(d, name)`. If `idx < 0`, unlock and return `-1` (or `-ENOENT`).
     - In `rmdir`: if `child->child_count > 0`, unlock and return `-1`.
     - Remove `child` from `d->children[]`.
     - `child->nlink = 0; child->parent = NULL;`
     - If `dir->mount`:
       - `spin_lock(&dir->mount->active_lock);`
       - `vfs_active_nodes_mark_orphaned_locked(dir->mount, (uint64_t)(uintptr_t)child);`
       - `spin_unlock(&dir->mount->active_lock);`
     - `uint32_t rem = tmpfs_node_put_locked(child);`
     - `spin_unlock(&tmpfs_lock);`
     - If `rem == 0`: `tmpfs_destroy_node_unlocked(child);`
     - Return `0`.
   - **`tmpfs_iop_evict_node`**:
     ```c
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
     ```
   - Wire `.evict_node = tmpfs_iop_evict_node` into `tmpfs_iops`.
   - In `tmpfs_fs_mount`: call `spin_init(&tmpfs_lock);` and set `root->refcount = 1;`.

- [ ] **Step 5: Build and run `make test-host` to verify Task 1 compiles and passes**

Run:
```bash
make clean && make test-host
```
Expected: All host test binaries build cleanly and pass.

- [ ] **Step 6: Commit Task 1**

```bash
git add kernel/include/fs/vfs.h hosttests/include/fs/vfs.h kernel/fs/vfs.c kernel/syscall/sys_fs.c kernel/fs/elf.c kernel/fs/tmpfs.c
git commit -m "feat(vfs,tmpfs): add active_nodes registry, evict_node dispatch, and tmpfs refcount lifecycle (ARCH-5 Phase 2B)"
```

---

## Task 2: `ext2` Deferred Orphan Reclamation & `fat32` Shared `fat_file_object` Lifecycle

**Files:**
- Modify: `kernel/include/fs/ext2.h`
- Modify: `kernel/fs/ext2.c`
- Modify: `kernel/include/fs/fat.h`
- Modify: `kernel/fs/fat.c`

- [ ] **Step 1: Add orphan tracking fields to `ext2_fs_t` in `kernel/include/fs/ext2.h` and implement `ext2` deferred orphan reclamation in `kernel/fs/ext2.c`**

In `kernel/include/fs/ext2.h`, add orphan tracking capacity to `ext2_fs_t`:

```c
#define EXT2_MAX_ORPHANS 64

typedef struct {
    uint32_t ino;
    uint8_t  state; // 0 = unused, 1 = orphaned_pending, 2 = reclaiming
} ext2_orphan_slot_t;
```
and add `ext2_orphan_slot_t orphans[EXT2_MAX_ORPHANS];` to `ext2_fs_t` (and under `#ifdef OS01_HOST_TEST`, add `uint32_t test_reclaim_count;` to `ext2_fs_t` so host tests can assert exact sole-reclaimer execution counts!).

In `kernel/fs/ext2.c`:

1. Add helper functions (called with `fs->lock` held):
   - `ext2_mark_orphan_locked(ext2_fs_t *fs, uint32_t ino)`: records `ino` with `state = 1` in `fs->orphans[]` if not already present.
   - `ext2_is_orphan_locked(ext2_fs_t *fs, uint32_t ino)`: returns `true` if `ino` is in `fs->orphans[]` with `state == 1`.
   - `ext2_claim_orphan_reclaim_locked(ext2_fs_t *fs, uint32_t ino)`: finds `ino` in `fs->orphans[]`; if `state == 1`, transitions to `state = 2` (`reclaiming`) and returns `true`; if `state == 2`, returns `false` (another caller already claimed reclamation).
   - `ext2_clear_orphan_locked(ext2_fs_t *fs, uint32_t ino)`: clears the slot for `ino` (`ino = 0, state = 0`).
   - `ext2_reclaim_orphan_inode_locked(ext2_fs_t *fs, uint32_t target_ino)`:
     - Reads `ext2_inode_t inode` for `target_ino`. If `ext2_read_inode` fails, reverts orphan slot from `state = 2` back to `state = 1` (retryable) and returns `-EIO`.
     - Frees all blocks of `inode` (fast symlink vs long symlink vs regular/dir 12 direct + single indirect block `i_block[12]`). Zeroes each `inode.i_block[i]` entry as it is freed; if reading `i_block[12]` fails, writes back the partially cleaned `inode` and reverts orphan state to `1` without freeing the inode bitmap bit.
     - Sets `inode.i_blocks = 0; inode.i_size = 0;`, calls `ext2_write_inode(fs, target_ino, &inode)`, calls `free_inode(fs, target_ino)`, and calls `ext2_clear_orphan_locked(fs, target_ino)`.
     - Under `#ifdef OS01_HOST_TEST`, increments `fs->test_reclaim_count++`.

2. Update `ext2_iop_lookup`, `ext2_vfs_create`, and `ext2_vfs_mkdir` in `kernel/fs/ext2.c`:
   - **Pre-allocate** `vfs_node_t *node = calloc(1, sizeof(vfs_node_t))` and `node->name = strndup(name, VFS_NAME_MAX - 1)` **before** `spin_lock(&fs->lock)`.
   - Inside `spin_lock(&fs->lock)`:
     - Perform directory lookup / inode allocation / `dirent_add`. On any error, roll back disk allocations, `spin_unlock(&fs->lock)`, free `node->name` and `node`, and return error/`NULL`.
     - Populate `node` fields (`ext2_fill_vfs_node`, `node->mount = dir->mount`, `node->parent = vfs_node_get(dir)`, `node->fs_data = (void *)(uintptr_t)ino`, `node->i_ops = &ext2_iops`, `node->f_ops = &ext2_fops`, `node->refcount = 1`).
     - Call `vfs_active_node_register(node)` **before** `spin_unlock(&fs->lock)`!

3. Update `ext2_vfs_unlink`, `ext2_vfs_rmdir`, and `ext2_vfs_rename` (overwrite target branch) in `kernel/fs/ext2.c`:
   - When `i_links_count` drops to 0 (or in `rmdir` after removing the directory entry and decrementing parent link counts):
     - Write updated `inode` (`i_links_count = 0`, preserving `inode.i_size` and `inode.i_block[]`!) to disk via `ext2_write_inode(fs, target_ino, &inode)`.
     - Under `spin_lock(&dir->mount->active_lock)`:
       - `vfs_active_nodes_mark_orphaned_locked(dir->mount, target_ino);`
       - `bool has_active = vfs_active_nodes_has_ino_locked(dir->mount, target_ino, NULL);`
       - If `has_active`: `ext2_mark_orphan_locked(fs, target_ino);`
     - `spin_unlock(&dir->mount->active_lock);`
     - If `!has_active`:
       - Immediately call `ext2_reclaim_orphan_inode_locked(fs, target_ino);` while still holding `fs->lock`!

4. Implement `ext2_iop_evict_node` and wire `.evict_node = ext2_iop_evict_node` into `ext2_iops`:
   ```c
   static void ext2_iop_evict_node(struct vfs_node *node)
   {
       if (!node || !node->mount || !node->mount->fs_data) return;
       if (node == node->mount->root || (node->node_flags & VFS_NODE_MOUNT_STUB)) {
           vfs_active_node_unregister(node);
           return;
       }

       ext2_fs_t *fs = (ext2_fs_t *)node->mount->fs_data;
       uint32_t ino = ext2_node_ino(node);

       spin_lock(&fs->lock);
       spin_lock(&node->mount->active_lock);

       vfs_active_node_unregister_locked(node->mount, node);
       bool still_active = vfs_active_nodes_has_ino_locked(node->mount, ino, NULL);
       bool should_reclaim = false;
       if (!still_active &&
           ((node->node_flags & VFS_NODE_ORPHANED) || ext2_is_orphan_locked(fs, ino))) {
           if (!ext2_is_orphan_locked(fs, ino)) {
               ext2_mark_orphan_locked(fs, ino);
           }
           should_reclaim = ext2_claim_orphan_reclaim_locked(fs, ino);
       }

       spin_unlock(&node->mount->active_lock);

       if (should_reclaim) {
           ext2_reclaim_orphan_inode_locked(fs, ino);
       }

       spin_unlock(&fs->lock);
   }
   ```

5. In `ext2_vfs_write` and `ext2_vfs_truncate`, while holding `fs->lock`, after writing the updated inode to disk, synchronize active nodes:
   ```c
   node->size = inode.i_size;
   if (node->mount) {
       spin_lock(&node->mount->active_lock);
       vfs_active_nodes_sync_size_locked(node->mount, ino, inode.i_size, inode.i_mtime);
       spin_unlock(&node->mount->active_lock);
   }
   ```

- [ ] **Step 2: Define `fat_file_object_t` and update `fat32_fs_t` in `kernel/include/fs/fat.h`**

In `kernel/include/fs/fat.h`, include `<stdbool.h>` and `<arch/spinlock.h>`, and define `fat_file_object_t` and the updated `fat32_fs_t`:

```c
#define FAT_OBJECT_ID_BASE (1ULL << 32)

typedef struct fat_file_object {
    uint64_t                object_id;
    uint32_t                first_cluster;
    uint32_t                size;
    uint32_t                parent_cluster;
    uint64_t                dirent_lba;
    uint32_t                dirent_offset;
    uint8_t                 attr;
    bool                    linked;
    bool                    orphaned;
    bool                    reclaiming;
    uint32_t                handle_count;
    struct fat_file_object *next;
} fat_file_object_t;

typedef struct {
    block_device_t    *dev;
    FAT32_BPB          bpb;
    uint32_t           fat_start_sector;     // absolute LBA of first FAT
    uint32_t           first_data_sector;    // absolute LBA of cluster 2
    uint32_t           sectors_per_cluster;
    uint16_t           bytes_per_sector;
    uint32_t           root_cluster;
    spinlock_T         fat_lifecycle_lock;
    uint64_t           next_object_id;
    fat_file_object_t *file_objects;
#ifdef OS01_HOST_TEST
    uint32_t           test_reclaim_count;
#endif
} fat32_fs_t;
```

- [ ] **Step 3: Migrate `kernel/fs/fat.c` to `fat_file_object_t`, `fat_lifecycle_lock`, and `fat_iop_evict_node`**

In `kernel/fs/fat.c`:

1. Initialize `fat_lifecycle_lock` and `next_object_id` in `fat32_init`:
   ```c
   spin_init(&fs->fat_lifecycle_lock);
   fs->next_object_id = FAT_OBJECT_ID_BASE;
   fs->file_objects = NULL;
   ```

2. Add helpers for resolving directory clusters vs regular file objects:
   ```c
   static inline uint32_t fat_dir_cluster(fat32_fs_t *fs, vfs_node_t *dir)
   {
       if (!dir || dir->type != VFS_DIR) return fs->root_cluster;
       return dir->fs_data ? (uint32_t)(uintptr_t)dir->fs_data : fs->root_cluster;
   }

   static inline fat_file_object_t *fat_file_obj(vfs_node_t *node)
   {
       if (!node || node->type != VFS_FILE) return NULL;
       return (fat_file_object_t *)node->fs_data;
   }

   static fat_file_object_t *fat_find_linked_obj_locked(fat32_fs_t *fs,
                                                        uint64_t lba,
                                                        uint32_t off)
   {
       for (fat_file_object_t *cur = fs->file_objects; cur; cur = cur->next) {
           if (cur->linked && !cur->orphaned &&
               cur->dirent_lba == lba && cur->dirent_offset == off) {
               return cur;
           }
       }
       return NULL;
   }

   static void fat_unlink_obj_from_list_locked(fat32_fs_t *fs, fat_file_object_t *obj)
   {
       fat_file_object_t **pp = &fs->file_objects;
       while (*pp) {
           if (*pp == obj) {
               *pp = obj->next;
               obj->next = NULL;
               return;
           }
           pp = &(*pp)->next;
       }
   }

   // Syncs first_cluster and size back to the on-disk dirent ONLY if obj is still linked & not orphaned
   static int fat32_sync_obj_dirent_locked(fat32_fs_t *fs, fat_file_object_t *obj)
   {
       if (!fs || !obj || !obj->linked || obj->orphaned) return 0;
       FAT32_DIRENT dirent;
       if (fat32_read_entry_at(fs, obj->dirent_lba, obj->dirent_offset, &dirent) != 0)
           return -1;
       dirent.first_cluster_lo = (uint16_t)(obj->first_cluster & 0xFFFF);
       dirent.first_cluster_hi = (uint16_t)((obj->first_cluster >> 16) & 0xFFFF);
       dirent.file_size        = obj->size;
       return fat32_write_entry_at(fs, obj->dirent_lba, obj->dirent_offset, &dirent);
   }
   ```

3. Update `fat_iop_lookup`:
   - Pre-allocate `vfs_node_t *child = calloc(1, sizeof(vfs_node_t))`, `char *child_name = strndup(name, VFS_NAME_MAX - 1)`, and `fat_file_object_t *cand_obj = calloc(1, sizeof(fat_file_object_t))` before `spin_lock(&fs->fat_lifecycle_lock)`.
   - Inside `spin_lock(&fs->fat_lifecycle_lock)`:
     - `uint32_t dir_cluster = fat_dir_cluster(fs, dir);`
     - `fat32_find_by_name(fs, dir_cluster, name, &lba, &off)` and `fat32_read_entry_at(fs, lba, off, &dirent)`. On failure, unlock, free `cand_obj`, `child_name`, `child`, and return `-ENOENT` / `-EIO`.
     - If `dirent.attr & FAT_ATTR_DIRECTORY`:
       - `child->type = VFS_DIR; child->size = 0; child->mode = S_IFDIR | 0755; child->nlink = 2; child->ino = first_cl; child->fs_data = (void *)(uintptr_t)first_cl;`
     - Else (regular file):
       - `fat_file_object_t *obj = fat_find_linked_obj_locked(fs, lba, off);`
       - If `obj`:
         - `obj->handle_count++;`
       - Else:
         - Initialize `cand_obj`: `cand_obj->object_id = fs->next_object_id++; cand_obj->first_cluster = first_cl; cand_obj->size = dirent.file_size; cand_obj->parent_cluster = dir_cluster; cand_obj->dirent_lba = lba; cand_obj->dirent_offset = off; cand_obj->attr = dirent.attr; cand_obj->linked = true; cand_obj->orphaned = false; cand_obj->reclaiming = false; cand_obj->handle_count = 1; cand_obj->next = fs->file_objects; fs->file_objects = cand_obj; obj = cand_obj; cand_obj = NULL;`
       - `child->type = VFS_FILE; child->size = obj->size; child->mode = (obj->attr & FAT_ATTR_READ_ONLY) ? (S_IFREG | 0555) : (S_IFREG | 0755); child->nlink = obj->orphaned ? 0 : 1; child->ino = obj->object_id; child->fs_data = obj;`
     - Set `child->name = child_name; child->mount = dir->mount; child->parent = vfs_node_get(dir); child->i_ops = &fat_iops; child->f_ops = &fat_fops; child->refcount = 1;`
     - Call `vfs_active_node_register(child)` before releasing `fs->fat_lifecycle_lock`!
   - Unlock `fs->fat_lifecycle_lock`, free `cand_obj` if non-NULL, set `*out = child; return 0;`.

4. Update `fat_create`:
   - Pre-allocate `vfs_node_t *node`, `node->name`, and `fat_file_object_t *obj` before `spin_lock(&fs->fat_lifecycle_lock)`.
   - Inside `spin_lock(&fs->fat_lifecycle_lock)`:
     - Check duplicate via `fat32_find_by_name(fs, dir_cluster, name, NULL, NULL) >= 0`.
     - Find free slot (`fat32_find_free_slot`), locate `(lba, off)` (`fat32_locate_entry`), write zeroed `FAT32_DIRENT` with `attr = FAT_ATTR_ARCHIVE` (`fat32_write_entry_at`). On failure, unlock, free pre-allocated structs, return `NULL`.
     - Initialize `obj` (`object_id = fs->next_object_id++`, `first_cluster = 0`, `size = 0`, `parent_cluster = dir_cluster`, `dirent_lba = lba`, `dirent_offset = off`, `attr = FAT_ATTR_ARCHIVE`, `linked = true`, `orphaned = false`, `reclaiming = false`, `handle_count = 1`), link into `fs->file_objects`.
     - Initialize `node` (`type = VFS_FILE`, `ino = obj->object_id`, `fs_data = obj`, `size = 0`, `mode = S_IFREG | 0755`, `nlink = 1`, `mount = dir->mount`, `parent = vfs_node_get(dir)`, `i_ops = &fat_iops`, `f_ops = &fat_fops`, `refcount = 1`).
     - Call `vfs_active_node_register(node)` before unlocking `fs->fat_lifecycle_lock`!

5. Update `fat_mkdir`:
   - Pre-allocate `vfs_node_t *node` and `node->name` before `spin_lock(&fs->fat_lifecycle_lock)`.
   - Perform directory creation inside `spin_lock(&fs->fat_lifecycle_lock)` and call `vfs_active_node_register(node)` before unlocking.

6. Update `fat_read`, `fat_write`, `fat_truncate`, `fat_iop_getattr`, `fat_readdir`:
   - **`fat_read`**:
     ```c
     static int fat_read(struct vfs_node *node, uint64_t offset,
                         uint64_t size, void *buffer)
     {
         if (!node || !node->mount || !buffer) return -1;
         if (size == 0) return 0;
         fat32_fs_t *fs = (fat32_fs_t *)node->mount->fs_data;

         spin_lock(&fs->fat_lifecycle_lock);
         fat_file_object_t *obj = fat_file_obj(node);
         if (!obj) {
             spin_unlock(&fs->fat_lifecycle_lock);
             return -1;
         }
         if (offset >= obj->size) {
             spin_unlock(&fs->fat_lifecycle_lock);
             return 0;
         }
         if (size > obj->size - offset) {
             size = obj->size - offset;
         }
         if (obj->first_cluster < 2) {
             spin_unlock(&fs->fat_lifecycle_lock);
             return 0;
         }
         int rc = fat32_read_data(fs, obj->first_cluster, offset, size, buffer);
         spin_unlock(&fs->fat_lifecycle_lock);
         return rc;
     }
     ```
   - **`fat_write`**:
     ```c
     static int fat_write(struct vfs_node *node, uint64_t offset,
                          uint64_t size, void *buffer)
     {
         if (!node || !node->mount || !buffer) return -1;
         if (size == 0) return 0;
         fat32_fs_t *fs = (fat32_fs_t *)node->mount->fs_data;

         spin_lock(&fs->fat_lifecycle_lock);
         fat_file_object_t *obj = fat_file_obj(node);
         if (!obj) {
             spin_unlock(&fs->fat_lifecycle_lock);
             return -1;
         }

         if (obj->first_cluster < 2) {
             uint32_t cl = fat32_alloc_cluster(fs, 0);
             if (cl == 0) {
                 spin_unlock(&fs->fat_lifecycle_lock);
                 return -1;
             }
             obj->first_cluster = cl;
         }

         int written = fat32_write_data(fs, obj->first_cluster, offset, size, buffer);
         if (written > 0 && offset + (uint64_t)written > obj->size) {
             obj->size = (uint32_t)(offset + (uint64_t)written);
         }
         fat32_sync_obj_dirent_locked(fs, obj);
         node->size = obj->size;
         if (node->mount) {
             spin_lock(&node->mount->active_lock);
             vfs_active_nodes_sync_size_locked(node->mount, obj->object_id, obj->size, 0);
             spin_unlock(&node->mount->active_lock);
         }
         spin_unlock(&fs->fat_lifecycle_lock);
         return written;
     }
     ```
   - **`fat_truncate`**:
     - Hold `spin_lock(&fs->fat_lifecycle_lock)` throughout; operate on `fat_file_object_t *obj = fat_file_obj(node)`.
     - If `new_size == 0` and `obj->first_cluster >= 2`: call `fat32_free_cluster_chain(fs, obj->first_cluster); obj->first_cluster = 0; obj->size = 0;`.
     - If shrinking to `new_size > 0` and `obj->first_cluster >= 2`: walk chain to `clusters_needed`, terminate with `FAT32_EOC_MIN`, free tail chain, set `obj->size = (uint32_t)new_size;`.
     - If growing or `obj->first_cluster < 2`: set `obj->size = (uint32_t)new_size;`.
     - Call `fat32_sync_obj_dirent_locked(fs, obj);`, update `node->size = obj->size;`, sync all active nodes under `node->mount->active_lock`, and `spin_unlock(&fs->fat_lifecycle_lock)`.
   - **`fat_iop_getattr`**:
     ```c
     static int fat_iop_getattr(struct vfs_node *node, struct stat *buf)
     {
         if (!node || !buf) return -1;
         if (node->mount && node->mount->fs_data && node->type == VFS_FILE) {
             fat32_fs_t *fs = (fat32_fs_t *)node->mount->fs_data;
             spin_lock(&fs->fat_lifecycle_lock);
             fat_file_object_t *obj = fat_file_obj(node);
             if (obj) {
                 node->size  = obj->size;
                 node->ino   = obj->object_id;
                 node->nlink = obj->orphaned ? 0 : 1;
             }
             spin_unlock(&fs->fat_lifecycle_lock);
         }
         return vfs_generic_getattr(node, buf);
     }
     ```
   - **`fat_readdir`**: guard `fat32_read_entry` under `spin_lock(&fs->fat_lifecycle_lock)`.

7. Update `fat_unlink`, `fat_rmdir`, `fat_rename`, and implement `fat_iop_evict_node`:
   - Helper `fat_orphan_or_reclaim_file_slot_locked(fat32_fs_t *fs, vfs_mount_t *mp, uint64_t lba, uint32_t off, uint32_t disk_first_cl, fat_file_object_t **out_free_obj)`:
     - Caller holds `fs->fat_lifecycle_lock` and has already marked the directory slot `0xE5`.
     - Finds `fat_file_object_t *obj = fat_find_linked_obj_locked(fs, lba, off);`
     - If `obj`:
       - `obj->linked = false;`
       - `obj->orphaned = true;`
       - `spin_lock(&mp->active_lock);`
       - `vfs_active_nodes_mark_orphaned_locked(mp, obj->object_id);`
       - `bool has_active = vfs_active_nodes_has_ino_locked(mp, obj->object_id, NULL) || (obj->handle_count > 0);`
       - `if (!has_active && !obj->reclaiming) obj->reclaiming = true;`
       - `spin_unlock(&mp->active_lock);`
       - If `!has_active`:
         - If `obj->first_cluster >= 2 && obj->first_cluster < FAT32_EOC_MIN`:
           `fat32_free_cluster_chain(fs, obj->first_cluster); obj->first_cluster = 0;`
         - `#ifdef OS01_HOST_TEST` `fs->test_reclaim_count++;` `#endif`
         - `fat_unlink_obj_from_list_locked(fs, obj);`
         - `*out_free_obj = obj;`
     - Else (no live `fat_file_object_t` for that slot):
       - If `disk_first_cl >= 2 && disk_first_cl < FAT32_EOC_MIN`:
         `fat32_free_cluster_chain(fs, disk_first_cl);`
       - `#ifdef OS01_HOST_TEST` `fs->test_reclaim_count++;` `#endif`
   - In **`fat_unlink`**:
     - Hold `spin_lock(&fs->fat_lifecycle_lock)`.
     - Find entry at `(lba, off)`, read `dirent`, reject `FAT_ATTR_DIRECTORY` with `-EISDIR`.
     - Mark entry deleted via `fat32_mark_entry_deleted(fs, lba, off)`.
     - Call `fat_orphan_or_reclaim_file_slot_locked(fs, dir->mount, lba, off, first_cl, &free_obj)`.
     - `spin_unlock(&fs->fat_lifecycle_lock);`
     - If `free_obj` non-NULL, `kfree(free_obj);`.
   - In **`fat_rename`**:
     - Hold `spin_lock(&fs->fat_lifecycle_lock)`.
     - Locate source `(src_lba, src_off)` and optional existing destination `(dst_lba, dst_off)`.
     - If destination exists and is a regular file: mark destination deleted and call `fat_orphan_or_reclaim_file_slot_locked` on `(dst_lba, dst_off)`.
     - Find `fat_file_object_t *src_obj = (!(src_ent.attr & FAT_ATTR_DIRECTORY)) ? fat_find_linked_obj_locked(fs, src_lba, src_off) : NULL;`.
     - Write updated entry at destination `(final_lba, final_off)` (and delete old slot if cross-directory).
     - If `src_obj`: update `src_obj->parent_cluster = dst_dir_cl; src_obj->dirent_lba = final_lba; src_obj->dirent_offset = final_off;`.
     - `spin_unlock(&fs->fat_lifecycle_lock);`
     - Free `free_obj` if non-NULL.
   - In **`fat_iop_evict_node`**:
     ```c
     static void fat_iop_evict_node(struct vfs_node *node)
     {
         if (!node || !node->mount || !node->mount->fs_data) return;
         if (node == node->mount->root || (node->node_flags & VFS_NODE_MOUNT_STUB)) {
             vfs_active_node_unregister(node);
             return;
         }

         fat32_fs_t *fs = (fat32_fs_t *)node->mount->fs_data;
         fat_file_object_t *free_obj = NULL;

         spin_lock(&fs->fat_lifecycle_lock);
         spin_lock(&node->mount->active_lock);

         vfs_active_node_unregister_locked(node->mount, node);

         if (node->type == VFS_FILE && node->fs_data) {
             fat_file_object_t *obj = (fat_file_object_t *)node->fs_data;
             if (obj->handle_count > 0) {
                 obj->handle_count--;
             }
             bool still_active = vfs_active_nodes_has_ino_locked(node->mount, obj->object_id, NULL) ||
                                 (obj->handle_count > 0);
             bool should_reclaim = false;
             if (!still_active) {
                 if (obj->orphaned && !obj->reclaiming) {
                     obj->reclaiming = true;
                     should_reclaim = true;
                     free_obj = obj;
                 } else if (obj->linked && !obj->orphaned) {
                     free_obj = obj;
                 }
             }
             spin_unlock(&node->mount->active_lock);

             if (should_reclaim) {
                 if (obj->first_cluster >= 2 && obj->first_cluster < FAT32_EOC_MIN) {
                     fat32_free_cluster_chain(fs, obj->first_cluster);
                     obj->first_cluster = 0;
                 }
     #ifdef OS01_HOST_TEST
                 fs->test_reclaim_count++;
     #endif
             }
             if (free_obj) {
                 fat_unlink_obj_from_list_locked(fs, free_obj);
             }
             spin_unlock(&fs->fat_lifecycle_lock);

             if (free_obj) {
                 kfree(free_obj);
             }
             return;
         }

         spin_unlock(&node->mount->active_lock);
         spin_unlock(&fs->fat_lifecycle_lock);
     }
     ```
   - Wire `.evict_node = fat_iop_evict_node` into `fat_iops`.

- [ ] **Step 4: Build and run `make test-host` to verify Task 2 compiles and passes**

Run:
```bash
make clean && make test-host
```
Expected: All existing host tests build cleanly and pass.

- [ ] **Step 5: Commit Task 2**

```bash
git add kernel/include/fs/ext2.h kernel/fs/ext2.c kernel/include/fs/fat.h kernel/fs/fat.c
git commit -m "feat(ext2,fat): add deferred orphan reclamation and shared fat_file_object lifecycle (ARCH-5 Phase 2B)"
```

---

## Task 3: Phase 2B Host Unit Tests (`test_vfs_iops_fops.c`) & Full Verification Gates

**Files:**
- Modify: `hosttests/mock/vfs_test_runtime.h`
- Modify: `hosttests/cases/test_vfs_iops_fops.c`

- [ ] **Step 1: Update `hosttests/mock/vfs_test_runtime.h` to track `spinlock_T` state**

In `hosttests/mock/vfs_test_runtime.h`, define `#define OS01_HOST_SPINLOCK_T_PROVIDED` BEFORE `#include "test_platform.h"` so `spinlock_T` tracks `lock = 1` (unlocked) and `lock = 0` (locked):

```c
#ifndef OS01_VFS_TEST_RUNTIME_H
#define OS01_VFS_TEST_RUNTIME_H

#define OS01_HOST_SPINLOCK_T_PROVIDED
typedef struct { unsigned long lock; } spinlock_T;
void poll_test_spinlock_acquire(spinlock_T *lock) __attribute__((weak));
static inline void spin_init(spinlock_T *l) { if (l) l->lock = 1; }
static inline void spin_lock(spinlock_T *l) { if (l) l->lock = 0; }
static inline void spin_unlock(spinlock_T *l) { if (l) l->lock = 1; }
static inline long spin_trylock(spinlock_T *l) {
    if (!l || l->lock == 0) return 0;
    l->lock = 0;
    return 1;
}
static inline uint64_t spin_lock_irqsave(spinlock_T *l)
{
    if (l) l->lock = 0;
    if (poll_test_spinlock_acquire) poll_test_spinlock_acquire(l);
    return 0;
}
static inline void spin_unlock_irqrestore(spinlock_T *l, uint64_t f)
{
    (void)f;
    if (l) l->lock = 1;
}

#include "test_platform.h"
```

- [ ] **Step 2: Expand `hosttests/cases/test_vfs_iops_fops.c` with RAM-disk `ext2`/`vfat` fixtures and all Phase 2B tests**

In `hosttests/cases/test_vfs_iops_fops.c`:

1. Replace the stub `block_device_read`/`block_device_write` with an in-memory block device dispatcher (`dev->ops->read` / `dev->ops->write`), plus an optional read-observer callback `s_on_block_read` so tests can inspect lock states during cluster/block reads.
2. Add two helper formatters:
   - `format_mock_fat32_disk()`: initializes a 256-sector (128 KiB) in-memory FAT32 volume (`bytes_per_sector = 512`, `sectors_per_cluster = 1`, `reserved_sectors = 32`, `num_fats = 2`, `sectors_per_fat_32 = 16`, `root_cluster = 2`, `0x55 0xAA`, `fat[2] = FAT32_EOC_MIN`).
   - `format_mock_ext2_disk()`: initializes a 64-block (4096 B/block, 256 KiB) in-memory ext2 volume (`s_magic = EXT2_MAGIC`, `s_log_block_size = 2`, `s_blocks_per_group = 64`, `s_inodes_per_group = 32`, `s_inode_size = 128`, bgdesc at block 1 with `bg_block_bitmap = 2`, `bg_inode_bitmap = 3`, `bg_inode_table = 4`, root inode 2 initialized as `EXT2_S_IFDIR | 0755` with data block 8 containing `.` and `..`).
3. Implement `vfs_test_on_active_register(vfs_node_t *node)` to verify that whenever any `ext2` or `vfat` node is registered into `mp->active_nodes` (during `lookup`, `create`, or `mkdir`), the corresponding backend lifecycle lock (`ext2_fs_t.lock` or `fat32_fs_t.fat_lifecycle_lock`) is **held (`lock == 0`)**, proving that `unlink` cannot interleave between dirent lookup/publication and active-node registration.
4. Add the following `TEST_FUNC` cases to `hosttests/cases/test_vfs_iops_fops.c` (covering every item in Spec §7.1 Phase 2):
   - `test_tmpfs_orphan_lifecycle_and_evict`:
     - Creates `/tmpfile`, opens two handles `h1` and `h2` via `vfs_lookup`, writes 4096B, calls `vfs_unlink("/tmpfile", "/")`.
     - Verifies `/tmpfile` lookup now returns `NULL` (`-ENOENT`), `h1->node_flags & VFS_NODE_ORPHANED`, `vfs_stat(h1)` reports `st_nlink == 0` and `st_size == 4096`, `vfs_read(h2, ...)` still reads the 4096B data intact, `vfs_write(h1, 4096, 100, ...)` extends the orphan file and `vfs_stat(h2)` sees `st_size == 4196`, and `vfs_node_put(h1)` + `vfs_node_put(h2)` cleanly evicts and frees blocks.
   - `test_ext2_orphan_deferred_reclaim_and_sole_reclaimer`:
     - Mounts `ext2` on `/`, creates `/a.txt`, writes `"EXT2_OLD_DATA"`, looks up a second handle `h2` (`h1` and `h2` both hold `ino_a`).
     - Calls `vfs_unlink("/a.txt", "/")`: verifies `fs->test_reclaim_count == 0` (deferred!), `h1` and `h2` have `VFS_NODE_ORPHANED` and `nlink == 0`.
     - Creates `/b.txt` (`h_b`): verifies `h_b->ino != h1->ino` (inode NOT reused while `h1`/`h2` active!), writes `"EXT2_NEW_B"` to `h_b`, writes `"_APPEND"` to `h1`, verifies reading `h_b` returns `"EXT2_NEW_B"` (uncorrupted!) and reading `h2` returns `"EXT2_OLD_DATA_APPEND"`.
     - Calls `vfs_node_put(h1)`: verifies `fs->test_reclaim_count == 0` (still 1 active handle `h2`).
     - Calls `vfs_node_put(h2)`: verifies `fs->test_reclaim_count == 1` (sole reclaimer executed on last put!).
     - Also tests the **zero-refcount pending-evict vs unlink race**: creates `/c.txt` (`hc`), drops `hc->refcount = 0` while `hc` is still in `mp->active_nodes` (simulating a concurrent `vfs_node_put` that just decremented `refcount` to 0 and is about to enter `ext2_iop_evict_node`), runs `vfs_unlink("/c.txt", "/")` (verifying `unlink` sees `hc` in `active_nodes` and does NOT reclaim yet: `reclaim_count` unchanged), then runs `hc->i_ops->evict_node(hc)` and verifies `reclaim_count` increments by **exactly 1**.
   - `test_fat32_shared_file_object_and_orphan_reclaim`:
     - Mounts `vfat` on `/`, creates two empty files `/e1.txt` and `/e2.txt`: verifies `e1->ino >= FAT_OBJECT_ID_BASE`, `e2->ino >= FAT_OBJECT_ID_BASE`, and `e1->ino != e2->ino` (distinct `object_id`s for empty files!).
     - Opens second handle `e1_dup = vfs_lookup("/e1.txt")`: verifies `e1_dup->ino == e1->ino` and `e1_dup->fs_data == e1->fs_data` (shared `fat_file_object_t`).
     - Writes `"HELLO_FAT"` via `e1`: verifies `e1_dup` immediately sees the allocated `first_cluster` and `vfs_stat(e1_dup)` reports `st_size == 9`, and `vfs_read(e1_dup, ...)` reads `"HELLO_FAT"` while `s_on_block_read` asserts `fs->fat_lifecycle_lock.lock == 0` (lock held during cluster chain read!).
     - Calls `vfs_truncate(e1, 0)`: verifies shared object has `first_cluster == 0` and `size == 0`, and `vfs_read(e1_dup, ...)` returns `0` without touching freed clusters.
     - Tests `open(A) → unlink(A) → create(B) → write(old_A)` on FAT32 (including unlinked empty file write):
       - Unlinks `/e1.txt` while `e1` and `e1_dup` are open (`e1` is currently 0-byte empty orphan!).
       - Creates `/reuse.txt` (`r_node`, which reuses the deleted directory entry slot at `(lba, off)`!): verifies `r_node->ino != e1->ino` (new `object_id`).
       - Writes `"ORPHAN_WRITE"` to the unlinked empty file `e1`: verifies it allocates a cluster and `vfs_read(e1_dup, ...)` reads `"ORPHAN_WRITE"`, while `/reuse.txt` on disk still has `file_size == 0` (the orphaned `e1` did NOT write back to the reused dirent slot!).
       - Drops `e1` (`reclaim_count` still 0 for `e1`), then drops `e1_dup` (`reclaim_count` increments by 1 as `e1`'s cluster chain is reclaimed).
     - Tests `vfs_rename("/e2.txt", "/renamed.txt", "/")` while `e2` is open, then writes `"AFTER_RENAME"` via `e2`, and verifies a fresh lookup of `/renamed.txt` sees the updated size and data.

- [ ] **Step 3: Run full verification gates**

Run:
```bash
make clean
make test-host
make test-static
make PROFILE=x86_64-clang kernel
make PROFILE=aarch64-clang kernel
make test-qemu SUITE=phase-0
make KERNEL_SELFTEST=1 test-kernel-selftest
make OS01_SYSTEST=1 test-syscall
```
Expected:
- `make test-host`: all host test binaries pass (including all new Phase 2B tests in `test_vfs_iops_fops.elf`).
- `make test-static` and both `x86_64-clang` / `aarch64-clang` kernel builds succeed with zero warnings.
- `make test-qemu SUITE=phase-0`, `make KERNEL_SELFTEST=1 test-kernel-selftest`, and `make OS01_SYSTEST=1 test-syscall` (228 syscall tests) all PASS with zero regressions.

- [ ] **Step 4: Commit Task 3**

```bash
git add hosttests/mock/vfs_test_runtime.h hosttests/cases/test_vfs_iops_fops.c
git commit -m "test(vfs): add Phase 2B orphan reclamation, race, and fat_file_object host tests (ARCH-5 Phase 2B)"
```
