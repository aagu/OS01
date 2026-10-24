# ARCH-5 VFS Abstraction Enhancement — Phase 3 (Hierarchical Mount Tree) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace flat path-prefix mount matching (`find_mount` / `vfs_is_child_mount`) with a node-attached hierarchical mount tree supporting pre-root deferred submount binding, Synthetic Mountpoint Stubs (`VFS_NODE_MOUNT_STUB`), component-by-component downward submount crossing by stable parent identity `(dir->mount, vfs_node_stable_ino(dir), comp)` and upward `..` crossing via `cur->mount->mountpoint`, `-EBUSY` protection against `rename`/`rmdir` on mountpoints or their ancestors, and `vfs_getdents` direct-child submount injection with deduplication.

**Architecture:**
- **Spec Reference:** [`docs/superpowers/specs/2026-10-24-arch5-vfs-abstraction-design.md`](../specs/2026-10-24-arch5-vfs-abstraction-design.md) (§5.3, §5.5, §6 Phase 3, §7).
- **Mount Tree State (`root_mount` + `mount_list`):**
  - `root_mount` tracks the published `"/"` mount.
  - Submounts mounted before `"/"` (such as `/dev` in `fs_boot_prepare()`) are queued as pending (`mp->parent_mount == NULL`) and bound in ascending path-depth order when `"/"` is published; if any pending submount fails to bind, the root mount publication rolls back atomically.
  - Binding a submount resolves `parent_dir` via `vfs_lookup(parent_path)` and either binds an existing `VFS_DIR` node or allocates a **Synthetic Mountpoint Stub** (`VFS_NODE_MOUNT_STUB`, `ino = VFS_MOUNT_STUB_INO_BASE | id`) when the target entry does not exist on the parent filesystem (e.g., `/dev` or `/proc` on read-only or unpopulated roots). `mp->mountpoint` holds a persistent reference (`refcount >= 1`) that pins the entire ancestor `parent` chain up to `"/"`.
- **Component-Wise Path Walk & Namespace Protection:**
  - `__vfs_lookup_raw` walks component-by-component from `root_mount->root`: `".."` at a submount root steps across `cur->mount->mountpoint` before stepping to `cur->parent`; downward components check `vfs_find_submount(cur, comp)` by `(cur->mount, vfs_node_stable_ino(cur), comp)` (honoring `VFS_FS_CASE_INSENSITIVE` on the parent mount) before invoking `cur->i_ops->lookup`.
  - Under `vfs_namespace_lock`, `vfs_rmdir` and `vfs_rename` call `vfs_is_mount_or_ancestor()` to reject removing or moving any active mountpoint or its ancestor directories with `-EBUSY`.
  - `vfs_getdents` injects direct child submounts matching `(mp->parent_mount == dir->mount && mp->parent_dir_ino == vfs_node_stable_ino(dir))` and deduplicates against entries already returned by the underlying filesystem's `readdir`.

**Tech Stack:** C11 (freestanding kernel + host-native unit tests), Clang/LLD, Make, Python QEMU runner.

---

## File Structure & Responsibilities

| File | Action | Responsibility |
|------|--------|----------------|
| `kernel/fs/vfs.c` | Modify | Add `root_mount`, `next_stub_id`, `vfs_mount_name_eq`, `vfs_find_submount`, `vfs_bind_submount_locked`, `vfs_publish_mount_locked`, component-wise mount crossing in `__vfs_lookup_raw`, `vfs_is_mount_or_ancestor` in `vfs_rmdir`/`vfs_rename`, stable-parent `vfs_getdents` injection + deduplication, cross-mount `vfs_resolve_path`, and remove `find_mount`/`vfs_is_child_mount` |
| `hosttests/cases/test_vfs_mount_tree.c` | Create | Host unit tests covering downward/upward mount crossing, case-insensitive parent submount lookup, `vfs_getdents` injection/deduplication, `-EBUSY` mountpoint & ancestor protection, and pre-root pending submount binding/rollback |
| `hosttests/Makefile` | Modify | Wire `test_vfs_mount_tree.elf` into `TEST_BINS` |

> [!WARNING]
> Working tree note: `hosttests/Makefile`, `hosttests/cases/test_fb_resolution_hooks.c`, and `hosttests/mock/fb_resolution_runtime.h` have pre-existing unstaged changes from another workstream. Do NOT revert them and do NOT stage unrelated hunks when committing Phase 3 tasks.

---

### Task 1: Hierarchical Mount Binding, Synthetic Mountpoint Stubs, and Pre-Root Deferred Binding (`kernel/fs/vfs.c`)

**Files:**
- Modify: `kernel/fs/vfs.c`

- [ ] **Step 1: Add `root_mount` and `next_stub_id` state and reset them in `vfs_init()`**

In [`kernel/fs/vfs.c`](../../../kernel/fs/vfs.c) around lines 43–49, add `root_mount` and `next_stub_id`, plus forward declarations for `normalize_vfs_path` and `vfs_split_parent`:

```c
// ── Global mount list ─────────────────────────────────────
static vfs_mount_t *mount_list = NULL;
static vfs_mount_t *root_mount = NULL;
static uint64_t next_stub_id = 0;
static int mount_count = 0;
static int vfs_initialized = 0;
static rwlock_t mount_lock;
static mutex_t vfs_namespace_lock;
static int vfs_bootstrap = 1;

static int normalize_vfs_path(const char *in, char *out);
static int vfs_split_parent(const char *path, const char *cwd,
                            char *parent_out, const char **name_out);
```

In `vfs_init()` (around line 84), reset `root_mount` and `next_stub_id`:

```c
    mount_list = NULL;
    root_mount = NULL;
    next_stub_id = 0;
    mount_count = 0;
```

- [ ] **Step 2: Implement mount-tree lookup and binding helpers (`vfs_mount_name_eq`, `vfs_find_submount`, `vfs_bind_submount_locked`, `vfs_unbind_submount_locked`, `vfs_publish_mount_locked`)**

In [`kernel/fs/vfs.c`](../../../kernel/fs/vfs.c) right above `vfs_mount()`, add the mount-tree helpers:

```c
static bool vfs_mount_name_eq(const vfs_mount_t *parent_mp, const char *a, const char *b)
{
    if (!a || !b) return false;
    if (parent_mp && (parent_mp->mnt_flags & VFS_FS_CASE_INSENSITIVE)) {
        return vfs_name_cmp(a, b) == 0;
    }
    return strcmp(a, b) == 0;
}

static vfs_mount_t *vfs_find_submount(vfs_node_t *dir, const char *comp)
{
    if (!dir || !dir->mount || !comp || comp[0] == '\0') return NULL;
    uint64_t dir_ino = vfs_node_stable_ino(dir);
    vfs_mount_t *found = NULL;

    rwlock_read_lock(&mount_lock);
    int _iter = 0;
    for (vfs_mount_t *mp = mount_list; mp; mp = mp->next) {
        if (!vfs_is_valid_kptr(mp)) {
            log_err("VFS: find_mount: CORRUPT mp=%p comp=%p iter=%d\n",
                    (void *)mp, (const void *)comp, _iter);
            break;
        }
        _iter++;
        if (mp->parent_mount == dir->mount &&
            mp->parent_dir_ino == dir_ino &&
            vfs_mount_name_eq(dir->mount, mp->mount_name, comp)) {
            found = mp;
            break;
        }
    }
    rwlock_read_unlock(&mount_lock);
    return found;
}

static bool vfs_has_duplicate_mount_path(const char *norm_path)
{
    if (!norm_path) return false;
    bool dup = false;
    rwlock_read_lock(&mount_lock);
    if (strcmp(norm_path, "/") == 0 && root_mount != NULL) {
        dup = true;
    } else {
        for (vfs_mount_t *mp = mount_list; mp; mp = mp->next) {
            if (!vfs_is_valid_kptr(mp)) break;
            if (mp->path && strcmp(mp->path, norm_path) == 0) {
                dup = true;
                break;
            }
        }
    }
    rwlock_read_unlock(&mount_lock);
    return dup;
}

static int vfs_path_depth(const char *norm_path)
{
    if (!norm_path || strcmp(norm_path, "/") == 0) return 0;
    int depth = 0;
    for (const char *p = norm_path; *p; p++) {
        if (*p == '/') depth++;
    }
    return depth;
}

static int vfs_bind_submount_locked(vfs_mount_t *mp)
{
    if (!mp || !mp->path || strcmp(mp->path, "/") == 0) return -EINVAL;

    char *parent_path = kmalloc(VFS_NAME_MAX);
    if (!parent_path) return -ENOMEM;

    const char *base_name = NULL;
    int src = vfs_split_parent(mp->path, "/", parent_path, &base_name);
    if (src < 0 || !base_name || base_name[0] == '\0') {
        kfree(parent_path);
        return (src < 0) ? src : -EINVAL;
    }

    size_t blen = strlen(base_name);
    if (blen >= VFS_NAME_MAX) {
        kfree(parent_path);
        return -ENAMETOOLONG;
    }
    memcpy(mp->mount_name, base_name, blen + 1);

    vfs_node_t *parent_dir = vfs_lookup(parent_path);
    kfree(parent_path);
    if (!parent_dir) return -ENOENT;
    if (parent_dir->type != VFS_DIR) {
        vfs_node_put(parent_dir);
        return -ENOTDIR;
    }

    if (vfs_find_submount(parent_dir, mp->mount_name) != NULL) {
        vfs_node_put(parent_dir);
        return -EBUSY;
    }

    vfs_node_t *target_node = NULL;
    int lrc;
    if (parent_dir->i_ops && vfs_is_valid_kptr(parent_dir->i_ops) &&
        parent_dir->i_ops->lookup && vfs_is_valid_kptr(parent_dir->i_ops->lookup)) {
        lrc = parent_dir->i_ops->lookup(parent_dir, mp->mount_name, &target_node);
    } else {
        lrc = vfs_generic_lookup_by_readdir(parent_dir, mp->mount_name, &target_node);
    }

    vfs_node_t *mp_node = NULL;
    if (lrc == 0 && target_node != NULL) {
        if (target_node->type != VFS_DIR) {
            vfs_node_put(target_node);
            vfs_node_put(parent_dir);
            return -ENOTDIR;
        }
        if (!target_node->mount) target_node->mount = parent_dir->mount;
        if (!target_node->parent) {
            target_node->parent = vfs_node_get(parent_dir);
        }
        if (!target_node->i_ops) target_node->i_ops = parent_dir->i_ops;
        if (!target_node->f_ops) target_node->f_ops = parent_dir->f_ops;
        vfs_active_node_register(target_node);
        if (target_node->mounted_here != NULL) {
            vfs_node_put(target_node);
            vfs_node_put(parent_dir);
            return -EBUSY;
        }
        mp_node = target_node;
    } else if (lrc == -ENOENT) {
        vfs_node_t *stub = (vfs_node_t *)calloc(1, sizeof(vfs_node_t));
        if (!stub) {
            vfs_node_put(parent_dir);
            return -ENOMEM;
        }
        stub->name = strdup(mp->mount_name);
        if (!stub->name) {
            free(stub);
            vfs_node_put(parent_dir);
            return -ENOMEM;
        }
        stub->type = VFS_DIR;
        stub->node_flags = VFS_NODE_MOUNT_STUB;
        stub->ino = VFS_MOUNT_STUB_INO_BASE | (__sync_add_and_fetch(&next_stub_id, 1));
        stub->mode = S_IFDIR | 0755;
        stub->nlink = 2;
        stub->mount = parent_dir->mount;
        stub->parent = vfs_node_get(parent_dir);
        stub->i_ops = parent_dir->i_ops;
        stub->f_ops = parent_dir->f_ops;
        stub->refcount = 1;
        vfs_active_node_register(stub);
        mp_node = stub;
    } else {
        vfs_node_put(parent_dir);
        return (lrc < 0) ? lrc : -ENOENT;
    }

    // mp_node->parent holds a persistent reference to parent_dir.
    vfs_node_put(parent_dir);

    rwlock_write_lock(&mount_lock);
    mp->parent_mount = mp_node->mount;
    mp->parent_dir_ino = vfs_node_stable_ino(mp_node->parent);
    mp->mountpoint = mp_node;
    mp_node->mounted_here = mp;
    rwlock_write_unlock(&mount_lock);

    return 0;
}

static void vfs_unbind_submount_locked(vfs_mount_t *mp)
{
    if (!mp) return;
    vfs_node_t *mp_node = NULL;
    rwlock_write_lock(&mount_lock);
    mp_node = mp->mountpoint;
    if (mp_node && mp_node->mounted_here == mp) {
        mp_node->mounted_here = NULL;
    }
    mp->mountpoint = NULL;
    mp->parent_mount = NULL;
    mp->parent_dir_ino = 0;
    rwlock_write_unlock(&mount_lock);

    if (mp_node) {
        vfs_node_put(mp_node);
    }
}

static int vfs_publish_mount_locked(vfs_mount_t *mp)
{
    if (!mp || !mp->path) return -EINVAL;

    if (strcmp(mp->path, "/") == 0) {
        if (root_mount != NULL) return -EBUSY;

        rwlock_write_lock(&mount_lock);
        root_mount = mp;
        mp->next = mount_list;
        mount_list = mp;
        mount_count++;
        rwlock_write_unlock(&mount_lock);

        // Collect pre-root pending submounts and sort by path depth ascending.
        vfs_mount_t *pending[32];
        int pending_cnt = 0;
        rwlock_read_lock(&mount_lock);
        for (vfs_mount_t *cur = mount_list; cur && pending_cnt < 32; cur = cur->next) {
            if (cur == mp || cur->parent_mount != NULL) continue;
            pending[pending_cnt++] = cur;
        }
        rwlock_read_unlock(&mount_lock);

        for (int i = 0; i < pending_cnt - 1; i++) {
            for (int j = i + 1; j < pending_cnt; j++) {
                if (vfs_path_depth(pending[j]->path) < vfs_path_depth(pending[i]->path)) {
                    vfs_mount_t *tmp = pending[i];
                    pending[i] = pending[j];
                    pending[j] = tmp;
                }
            }
        }

        for (int i = 0; i < pending_cnt; i++) {
            int brc = vfs_bind_submount_locked(pending[i]);
            if (brc < 0) {
                for (int k = i - 1; k >= 0; k--) {
                    vfs_unbind_submount_locked(pending[k]);
                }
                rwlock_write_lock(&mount_lock);
                vfs_mount_t **pp = &mount_list;
                while (*pp) {
                    if (*pp == mp) {
                        *pp = mp->next;
                        mp->next = NULL;
                        mount_count--;
                        break;
                    }
                    pp = &(*pp)->next;
                }
                root_mount = NULL;
                rwlock_write_unlock(&mount_lock);
                return brc;
            }
        }
        return 0;
    }

    if (root_mount == NULL) {
        char *parent_path = kmalloc(VFS_NAME_MAX);
        if (!parent_path) return -ENOMEM;
        const char *base_name = NULL;
        int src = vfs_split_parent(mp->path, "/", parent_path, &base_name);
        if (src < 0 || !base_name || base_name[0] == '\0' || strlen(base_name) >= VFS_NAME_MAX) {
            kfree(parent_path);
            return (src < 0) ? src : -EINVAL;
        }
        memcpy(mp->mount_name, base_name, strlen(base_name) + 1);
        kfree(parent_path);

        mp->parent_mount = NULL;
        mp->parent_dir_ino = 0;
        mp->mountpoint = NULL;

        rwlock_write_lock(&mount_lock);
        mp->next = mount_list;
        mount_list = mp;
        mount_count++;
        rwlock_write_unlock(&mount_lock);
        return 0;
    }

    int brc = vfs_bind_submount_locked(mp);
    if (brc < 0) return brc;

    rwlock_write_lock(&mount_lock);
    mp->next = mount_list;
    mount_list = mp;
    mount_count++;
    rwlock_write_unlock(&mount_lock);
    return 0;
}
```

- [ ] **Step 3: Update `vfs_mount()` and `vfs_mount_fs()` to normalize mount paths, serialize under `vfs_ns_lock()`, check `-EBUSY`, and call `vfs_publish_mount_locked()`**

Note: move `vfs_ns_lock()` / `vfs_ns_unlock()` above `vfs_mount()` (or forward-declare them) so both `vfs_mount()` and `vfs_mount_fs()` can use them.

Update `vfs_mount()` and `vfs_mount_fs()` in [`kernel/fs/vfs.c`](../../../kernel/fs/vfs.c):

```c
int vfs_mount(const char *path, block_device_t *dev,
              const vfs_inode_ops_t *i_ops, const vfs_file_ops_t *f_ops,
              void *fs_data)
{
    if (!path || path[0] != '/') return -EINVAL;
    if (!vfs_initialized) vfs_init();

    char *norm_path = kmalloc(VFS_NAME_MAX);
    if (!norm_path) return -ENOMEM;
    normalize_vfs_path(path, norm_path);

    vfs_ns_lock();
    if (vfs_has_duplicate_mount_path(norm_path)) {
        vfs_ns_unlock();
        kfree(norm_path);
        return -EBUSY;
    }

    vfs_mount_t *mp = (vfs_mount_t *)calloc(1, sizeof(vfs_mount_t));
    if (!mp) {
        vfs_ns_unlock();
        kfree(norm_path);
        return -ENOMEM;
    }

    spin_init(&mp->active_lock);
    mp->active_nodes = NULL;
    mp->dev = dev;
    mp->path = norm_path;
    mp->i_ops = i_ops;
    mp->f_ops = f_ops;
    mp->fs_data = fs_data;
    mp->root = (vfs_node_t *)calloc(1, sizeof(vfs_node_t));
    if (!mp->root) {
        vfs_ns_unlock();
        kfree(norm_path);
        kfree(mp);
        return -ENOMEM;
    }

    mp->root->mount = mp;
    mp->root->type = VFS_DIR;
    mp->root->ino = VFS_SYNTH_ROOT_INO;
    mp->root->mode = S_IFDIR | 0755;
    mp->root->nlink = 2;
    mp->root->i_ops = i_ops;
    mp->root->f_ops = f_ops;
    mp->root->fs_data = NULL;
    mp->root->refcount = 1;
    mp->root->name = strdup("/");
    if (!mp->root->name) {
        vfs_ns_unlock();
        kfree(mp->root);
        kfree(norm_path);
        kfree(mp);
        return -ENOMEM;
    }

    int prc = vfs_publish_mount_locked(mp);
    if (prc < 0) {
        vfs_ns_unlock();
        kfree(mp->root->name);
        kfree(mp->root);
        kfree(norm_path);
        kfree(mp);
        return prc;
    }

    vfs_ns_unlock();
    debug_vfs("VFS: mounted '%s'\n", path);
    return 0;
}
```

And in `vfs_mount_fs()`:
1. After validating `type_name` and `path` (`path[0] == '/'`), normalize `path` into a heap-allocated `char *path_copy = kmalloc(VFS_NAME_MAX)` via `normalize_vfs_path(path, path_copy)`.
2. Reserve `fs_type` in `fs_type_usage` (unchanged).
3. Acquire `vfs_ns_lock()`.
4. Check `if (vfs_has_duplicate_mount_path(path_copy))` $\rightarrow$ `vfs_ns_unlock(); kfree(path_copy); vfs_drop_fs_type_usage(fs_type); return -EBUSY;`.
5. Allocate `mp`, `mp->root`, `mp->root->name = strdup("/")`, and invoke `rc = fs_type->mount(mp, dev, data);` (on failure, `vfs_ns_unlock()` in the error path).
6. Replace the old `rwlock_write_lock(&mount_lock); mp->next = mount_list; ...` block at the end of `vfs_mount_fs()` with:
   ```c
   int prc = vfs_publish_mount_locked(mp);
   if (prc < 0) {
       vfs_ns_unlock();
       if (mp->root) {
           if (mp->root->name) kfree(mp->root->name);
           kfree(mp->root);
       }
       kfree(path_copy);
       kfree(mp);
       vfs_drop_fs_type_usage(fs_type);
       return prc;
   }
   vfs_ns_unlock();
   ```

---

### Task 2: Component-Wise Mount Crossing in `__vfs_lookup_raw`, Mount Ancestor Protection (`-EBUSY`), `vfs_getdents` Injection, and Cross-Mount `vfs_resolve_path` (`kernel/fs/vfs.c`)

**Files:**
- Modify: `kernel/fs/vfs.c`

- [ ] **Step 1: Replace `find_mount()` with `vfs_find_pending_submount_early()` and component-wise mount crossing in `__vfs_lookup_raw()`**

In [`kernel/fs/vfs.c`](../../../kernel/fs/vfs.c), delete `find_mount()` and replace it with a narrow helper used only when `root_mount == NULL` (the early boot window before `/` is mounted):

```c
static vfs_mount_t *vfs_find_pending_submount_early(const char *norm_path, size_t *prefix_len_out)
{
    vfs_mount_t *best = NULL;
    size_t best_len = 0;

    rwlock_read_lock(&mount_lock);
    for (vfs_mount_t *mp = mount_list; mp; mp = mp->next) {
        if (!vfs_is_valid_kptr(mp) || !mp->path) break;
        size_t len = strlen(mp->path);
        if (len > 1 && strncmp(norm_path, mp->path, len) == 0 &&
            (norm_path[len] == '\0' || norm_path[len] == '/')) {
            if (len > best_len) {
                best_len = len;
                best = mp;
            }
        }
    }
    rwlock_read_unlock(&mount_lock);
    if (prefix_len_out) *prefix_len_out = best_len;
    return best;
}
```

Now update `__vfs_lookup_raw()` in [`kernel/fs/vfs.c`](../../../kernel/fs/vfs.c):

```c
    // Handle root
    if (strcmp(norm_path, "/") == 0) {
        rwlock_read_lock(&mount_lock);
        vfs_node_t *rnode = (root_mount && root_mount->root)
                            ? vfs_node_get(root_mount->root) : NULL;
        rwlock_read_unlock(&mount_lock);
        if (!rnode) {
            kfree(bufs);
            return -ENOENT;
        }
        *out_node = rnode;
        if (consumed_out && consumed_size >= 2) {
            consumed_out[0] = '/';
            consumed_out[1] = '\0';
        }
        kfree(bufs);
        return 0;
    }

    memcpy(bufs->path_copy, norm_path, plen + 1);
    char *ptr = bufs->path_copy;
    vfs_node_t *cur = NULL;
    size_t consumed_len = 0;
    if (consumed_out && consumed_size > 0) consumed_out[0] = '\0';

    rwlock_read_lock(&mount_lock);
    vfs_mount_t *rm = root_mount;
    if (rm && rm->root) {
        cur = vfs_node_get(rm->root);
    }
    rwlock_read_unlock(&mount_lock);

    if (!cur) {
        size_t mp_len = 0;
        vfs_mount_t *early_mp = vfs_find_pending_submount_early(norm_path, &mp_len);
        if (!early_mp || !early_mp->root || (!early_mp->root->i_ops && !early_mp->root->f_ops)) {
            kfree(bufs);
            return -ENOENT;
        }
        cur = vfs_node_get(early_mp->root);
        ptr = bufs->path_copy + mp_len;
        while (*ptr == '/') ptr++;
        if (consumed_out && mp_len + 1 > consumed_size) {
            vfs_node_put(cur);
            kfree(bufs);
            return -ENAMETOOLONG;
        }
        if (consumed_out) {
            memcpy(consumed_out, early_mp->path, mp_len);
            consumed_out[mp_len] = '\0';
        }
        consumed_len = mp_len;
    }

    char *comp;
    int ret = 0;
    while ((comp = next_component(&ptr)) != NULL) {
        if (strlen(comp) == 0) continue;
        if (strcmp(comp, ".") == 0) continue;

        if (cur->type != VFS_DIR) {
            vfs_node_put(cur);
            ret = -ENOTDIR;
            goto out;
        }

        // ".." — cross submount roots back to mountpoint, then step to parent
        if (strcmp(comp, "..") == 0) {
            while (cur->mount && cur == cur->mount->root && cur->mount->mountpoint != NULL) {
                vfs_node_t *mp_node = vfs_node_get(cur->mount->mountpoint);
                vfs_node_put(cur);
                cur = mp_node;
            }
            if (cur->parent) {
                vfs_node_t *parent = vfs_node_get(cur->parent);
                vfs_node_put(cur);
                cur = parent;
            }
            if (consumed_out) {
                size_t clen = strlen(consumed_out);
                if (clen > 1) {
                    char *last = consumed_out + clen - 1;
                    while (last > consumed_out && *last != '/') last--;
                    if (last == consumed_out) {
                        last[1] = '\0';
                        consumed_len = 1;
                    } else {
                        *last = '\0';
                        consumed_len = (size_t)(last - consumed_out);
                    }
                }
            }
            continue;
        }

        vfs_node_t *child = NULL;
        vfs_mount_t *sub_mp = vfs_find_submount(cur, comp);
        if (sub_mp && sub_mp->root) {
            child = vfs_node_get(sub_mp->root);
        } else {
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
            }
        }

        // Append "/" + comp to consumed (bounded by VFS_NAME_MAX, same as path).
        if (consumed_out && consumed_size > 0) {
            size_t cl = strlen(comp);
            if (consumed_len + 1 + cl + 1 > consumed_size) {
                vfs_node_put(child);
                vfs_node_put(cur);
                ret = -ENAMETOOLONG;
                goto out;
            }
            consumed_out[consumed_len] = '/';
            memcpy(consumed_out + consumed_len + 1, comp, cl);
            consumed_len += 1 + cl;
            consumed_out[consumed_len] = '\0';
        }

        // Symlink early-return: caller decides whether to follow.
        if (child->type == VFS_SYMLINK) {
            if (remaining_out && remaining_size > 0) {
                size_t rem_len = strlen(ptr);
                if (rem_len == 0) {
                    remaining_out[0] = '\0';
                } else {
                    if (1 + rem_len + 1 > remaining_size) {
                        vfs_node_put(child);
                        vfs_node_put(cur);
                        ret = -ENAMETOOLONG;
                        goto out;
                    }
                    remaining_out[0] = '/';
                    memcpy(remaining_out + 1, ptr, rem_len);
                    remaining_out[1 + rem_len] = '\0';
                }
            }
            vfs_node_put(cur);
            *out_node = child;
            ret = 1;  // symlink hit
            goto out;
        }

        vfs_node_put(cur);
        cur = child;
    }
```

- [ ] **Step 2: Add `vfs_is_mount_or_ancestor()` and enforce `-EBUSY` in `vfs_rmdir()` and `vfs_rename()`**

In [`kernel/fs/vfs.c`](../../../kernel/fs/vfs.c) right above `vfs_unlink()`, add `vfs_is_mount_or_ancestor()`:

```c
static bool vfs_is_mount_or_ancestor(vfs_node_t *dir, const char *name)
{
    if (!dir || !dir->mount || !name || name[0] == '\0') return false;
    uint64_t dir_ino = vfs_node_stable_ino(dir);
    bool hit = false;

    rwlock_read_lock(&mount_lock);
    for (vfs_mount_t *mp = mount_list; mp; mp = mp->next) {
        if (!vfs_is_valid_kptr(mp)) break;
        for (vfs_node_t *anc = mp->mountpoint; anc && anc->parent; anc = anc->parent) {
            if (anc->parent->mount == dir->mount &&
                vfs_node_stable_ino(anc->parent) == dir_ino &&
                vfs_mount_name_eq(dir->mount, anc->name, name)) {
                hit = true;
                break;
            }
        }
        if (hit) break;
    }
    rwlock_read_unlock(&mount_lock);
    return hit;
}
```

In `vfs_rmdir()`, after checking `parent->type != VFS_DIR`:

```c
    if (parent->node_flags & VFS_NODE_ORPHANED) {
        vfs_node_put(parent);
        kfree(parent_path);
        vfs_ns_unlock();
        return -ENOENT;
    }
    if (vfs_is_mount_or_ancestor(parent, name)) {
        vfs_node_put(parent);
        kfree(parent_path);
        vfs_ns_unlock();
        return -EBUSY;
    }
```

In `vfs_rename()`, right after `kfree(parents);` (around line 1802):

```c
    if ((olddir->node_flags & VFS_NODE_ORPHANED) ||
        (newdir->node_flags & VFS_NODE_ORPHANED)) {
        vfs_node_put(olddir);
        vfs_node_put(newdir);
        vfs_ns_unlock();
        return -ENOENT;
    }

    if (vfs_is_mount_or_ancestor(olddir, oldname) ||
        vfs_is_mount_or_ancestor(newdir, newname)) {
        vfs_node_put(olddir);
        vfs_node_put(newdir);
        vfs_ns_unlock();
        return -EBUSY;
    }
```

- [ ] **Step 3: Update `vfs_getdents()` to inject submounts by stable parent identity + deduplicate, and delete `vfs_is_child_mount()`**

In [`kernel/fs/vfs.c`](../../../kernel/fs/vfs.c):
1. Delete `vfs_is_child_mount()`.
2. Replace Phase 2 of `vfs_getdents()` (lines 1426–1449) with:

```c
    // ── Phase 2: Inject direct child sub-mount entries ─────────
    if (dir->mount) {
        uint64_t dir_ino = vfs_node_stable_ino(dir);
        rwlock_read_lock(&mount_lock);
        for (vfs_mount_t *mp = mount_list; mp && total < VFS_GETDENTS_SORT_MAX; mp = mp->next) {
            if (!vfs_is_valid_kptr(mp)) break;
            if (mp->parent_mount != dir->mount ||
                mp->parent_dir_ino != dir_ino ||
                mp->mount_name[0] == '\0') {
                continue;
            }

            bool exists = false;
            for (int k = 0; k < total; k++) {
                if (vfs_mount_name_eq(dir->mount, entries[k].name, mp->mount_name)) {
                    exists = true;
                    break;
                }
            }
            if (exists) continue;

            size_t blen = strlen(mp->mount_name);
            if (blen >= VFS_NAME_MAX) blen = VFS_NAME_MAX - 1;
            memcpy(entries[total].name, mp->mount_name, blen);
            entries[total].name[blen] = '\0';
            entries[total].size = 0;
            entries[total].type = VFS_DIR;
            entries[total].ino  = mp->root ? vfs_node_stable_ino(mp->root)
                                           : (VFS_MOUNT_STUB_INO_BASE | (uint64_t)total);
            total++;
        }
        rwlock_read_unlock(&mount_lock);
    }
```

- [ ] **Step 4: Update `vfs_resolve_path()` to step across `cur->mount->mountpoint` up to the root mount**

In [`kernel/fs/vfs.c`](../../../kernel/fs/vfs.c) in `vfs_resolve_path()` (around lines 1866–1883), update the upward walk so submount nodes resolve their full path across mount boundaries:

```c
    const char *names[32];
    int depth = 0;
    vfs_node_t *cur = node;
    vfs_mount_t *top_mount = node->mount;

    while (cur) {
        while (cur->mount && cur == cur->mount->root && cur->mount->mountpoint != NULL) {
            cur = cur->mount->mountpoint;
        }
        if (cur->mount) {
            top_mount = cur->mount;
        }
        if (depth >= 32)
            return -1;  // path too deep
        names[depth++] = cur->name;
        if (!cur->parent)
            break;  // reached root
        cur = cur->parent;
    }

    size_t written = 0;
    const char *mpath = top_mount ? top_mount->path : node->mount->path;
```

- [ ] **Step 5: Verify existing VFS host tests still pass before adding Phase 3 tests**

Run:
```bash
make test-host
```
Expected: All existing host tests (`test_vfs_basic`, `test_vfs_fs_registry`, `test_vfs_iops_fops`, etc.) compile and pass.

- [ ] **Step 6: Commit Tasks 1 & 2 (`kernel/fs/vfs.c`)**

Run:
```bash
git add kernel/fs/vfs.c
git commit -m "feat(vfs): replace prefix mount matching with hierarchical node mount tree"
```

---

### Task 3: Phase 3 Host Unit Tests (`test_vfs_mount_tree`) & Full Verification Gates

**Files:**
- Create: `hosttests/cases/test_vfs_mount_tree.c`
- Modify: `hosttests/Makefile`

- [ ] **Step 1: Create `hosttests/cases/test_vfs_mount_tree.c` covering all Spec §7.1 Phase 3 cases**

Create [`hosttests/cases/test_vfs_mount_tree.c`](../../../hosttests/cases/test_vfs_mount_tree.c):

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

static const vfs_inode_ops_t s_ro_proc_iops = {0};
static const vfs_file_ops_t  s_ro_proc_fops = {0};
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

TEST_FUNC(test_submount_downward_and_dotdot_upward_crossing) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));
    assert_eq(0, vfs_mkdir("/mnt", "/"));

    /* Mount tmpfs at /mnt/usb without pre-creating /mnt/usb (stub mountpoint) */
    assert_eq(0, vfs_mount_fs("tmpfs", "/mnt/usb", NULL, NULL));

    vfs_node_t *root = vfs_lookup("/");
    vfs_node_t *usb_root = vfs_lookup("/mnt/usb");
    assert_not_null(root);
    assert_not_null(usb_root);
    assert_true(root->mount != usb_root->mount);
    assert_not_null(usb_root->mount->mountpoint);
    assert_true(usb_root->mount->mountpoint->node_flags & VFS_NODE_MOUNT_STUB);

    /* Create file inside submount and read it back via fresh path walk */
    vfs_node_t *f = NULL;
    assert_eq(0, vfs_create_file("/mnt/usb/hello.txt", "/", &f));
    assert_not_null(f);
    assert_eq(11, vfs_write(f, 0, 11, "SUBMOUNT_OK"));

    char resolved[128] = {0};
    assert_true(vfs_resolve_path(f, resolved, sizeof(resolved)) > 0);
    assert_str_eq("/mnt/usb/hello.txt", resolved);
    vfs_node_put(f);

    vfs_node_t *f2 = vfs_lookup("/mnt/usb/hello.txt");
    assert_not_null(f2);
    char buf[32] = {0};
    assert_eq(11, vfs_read(f2, 0, sizeof(buf), buf));
    assert_str_eq("SUBMOUNT_OK", buf);
    vfs_node_put(f2);

    /* Upward ".." crossing from submount root back to /mnt and / */
    vfs_node_t *up_mnt = vfs_lookup("/mnt/usb/..");
    assert_not_null(up_mnt);
    assert_true(up_mnt->mount == root->mount);
    assert_str_eq("mnt", up_mnt->name);
    vfs_node_put(up_mnt);

    vfs_node_t *up_root = vfs_lookup("/mnt/usb/../..");
    assert_not_null(up_root);
    assert_true(up_root == root);
    vfs_node_put(up_root);

    vfs_node_put(usb_root);
    vfs_node_put(root);
}

TEST_FUNC(test_fresh_parent_lookup_and_case_insensitive_vfat_parent) {
    vfs_init();
    format_mock_fat32_disk();
    assert_eq(0, vfs_mount_fs("vfat", "/", &s_ram_block_dev, NULL));
    assert_eq(0, vfs_mkdir("/MNT", "/"));

    /* Mount tmpfs on /mnt/usb (parent is case-insensitive vfat) */
    assert_eq(0, vfs_mount_fs("tmpfs", "/mnt/usb", NULL, NULL));

    vfs_node_t *f = NULL;
    assert_eq(0, vfs_create_file("/mnt/usb/case.txt", "/", &f));
    assert_not_null(f);
    assert_eq(4, vfs_write(f, 0, 4, "VFAT"));
    vfs_node_put(f);

    /* Re-lookup with mixed case on both parent and submount name */
    vfs_node_t *l1 = vfs_lookup("/mnt/USB/case.txt");
    assert_not_null(l1);
    vfs_node_put(l1);

    vfs_node_t *l2 = vfs_lookup("/MNT/usb/case.txt");
    assert_not_null(l2);
    vfs_node_put(l2);

    vfs_node_t *l3 = vfs_lookup("/MNT/USB/..");
    assert_not_null(l3);
    assert_eq(0, strcmp(l3->name, "MNT"));
    vfs_node_put(l3);
}

static int count_dirent_name(vfs_node_t *dir, const char *target)
{
    uint8_t raw[2048];
    uint64_t pos = 0;
    int matches = 0;

    for (;;) {
        int n = vfs_getdents(dir, (struct linux_dirent64 *)raw, sizeof(raw), &pos);
        assert_true(n >= 0);
        if (n == 0) break;
        int off = 0;
        while (off < n) {
            struct linux_dirent64 *de = (struct linux_dirent64 *)(raw + off);
            if (strcmp(de->d_name, target) == 0) {
                matches++;
            }
            off += de->d_reclen;
        }
    }
    return matches;
}

TEST_FUNC(test_getdents_submount_injection_and_deduplication) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));
    assert_eq(0, vfs_mkdir("/mnt", "/"));
    assert_eq(0, vfs_mkdir("/mnt/existing_dir", "/"));

    /* /mnt/usb is stub-backed; /mnt/existing_dir already exists in /mnt */
    assert_eq(0, vfs_mount_fs("tmpfs", "/mnt/usb", NULL, NULL));
    assert_eq(0, vfs_mount_fs("tmpfs", "/mnt/existing_dir", NULL, NULL));

    vfs_node_t *root = vfs_lookup("/");
    assert_not_null(root);
    assert_eq(1, count_dirent_name(root, "mnt"));
    assert_eq(0, count_dirent_name(root, "usb"));
    assert_eq(0, count_dirent_name(root, "existing_dir"));
    vfs_node_put(root);

    vfs_node_t *mnt = vfs_lookup("/mnt");
    assert_not_null(mnt);
    assert_eq(1, count_dirent_name(mnt, "usb"));
    assert_eq(1, count_dirent_name(mnt, "existing_dir"));
    vfs_node_put(mnt);
}

TEST_FUNC(test_mount_protection_ebusy_and_mount_validation) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));
    assert_eq(0, vfs_mkdir("/a", "/"));
    assert_eq(0, vfs_mkdir("/a/d", "/"));
    assert_eq(0, vfs_mkdir("/b", "/"));

    vfs_node_t *reg = NULL;
    assert_eq(0, vfs_create_file("/regular.txt", "/", &reg));
    vfs_node_put(reg);

    /* Mount validation errors */
    assert_eq(-EBUSY, vfs_mount_fs("tmpfs", "/", NULL, NULL));
    assert_eq(-ENOENT, vfs_mount_fs("tmpfs", "/missing_parent/sub", NULL, NULL));
    assert_eq(-ENOTDIR, vfs_mount_fs("tmpfs", "/regular.txt/sub", NULL, NULL));
    assert_eq(-ENOTDIR, vfs_mount_fs("tmpfs", "/regular.txt", NULL, NULL));

    /* Mount at /a/d/usb and verify duplicate mount returns -EBUSY */
    assert_eq(0, vfs_mount_fs("tmpfs", "/a/d/usb", NULL, NULL));
    assert_eq(-EBUSY, vfs_mount_fs("tmpfs", "/a/d/usb", NULL, NULL));

    /* Renaming or removing mountpoint or any ancestor must return -EBUSY */
    assert_eq(-EBUSY, vfs_rename("/a/d", "/b/d", "/"));
    assert_eq(-EBUSY, vfs_rename("/a", "/b/a", "/"));
    assert_eq(-EBUSY, vfs_rename("/a/d/usb", "/a/d/usb2", "/"));
    assert_eq(-EBUSY, vfs_rmdir("/a/d", "/"));
    assert_eq(-EBUSY, vfs_rmdir("/a/d/usb", "/"));

    /* Ancestor chain must remain intact after rejected renames */
    vfs_node_t *anc_a = vfs_lookup("/a/d/usb/../..");
    assert_not_null(anc_a);
    assert_str_eq("a", anc_a->name);
    vfs_node_put(anc_a);
}

static int root_with_file_mount(struct vfs_mount *mp, block_device_t *dev, void *data)
{
    int rc = tmpfs_type.mount(mp, dev, data);
    if (rc < 0) return rc;
    vfs_node_t *f = NULL;
    if (mp->root->i_ops->create(mp->root, "conflict_file", 0644, &f) == 0 && f) {
        vfs_node_put(f);
    }
    return 0;
}

static vfs_fs_type_t s_root_conflict_fs = {
    .name = "root_conflict_tmpfs",
    .fs_flags = 0,
    .mount = root_with_file_mount,
};

TEST_FUNC(test_pre_root_pending_submounts_depth_order_and_rollback) {
    vfs_init();

    /* Queue pending submounts out of depth order before "/" is mounted */
    devfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/mnt/deep/sub", NULL, NULL));
    assert_eq(0, vfs_mount_fs("tmpfs", "/mnt", NULL, NULL));
    assert_eq(0, vfs_mount_fs("tmpfs", "/mnt/deep", NULL, NULL));

    /* Early pre-root lookup on /dev works before "/" is mounted */
    vfs_node_t *early_null = vfs_lookup("/dev/null");
    assert_not_null(early_null);
    vfs_node_put(early_null);

    /* Mount "/" -> binds all pending submounts in depth order */
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));

    vfs_node_t *deep_parent = vfs_lookup("/mnt/deep/sub/..");
    assert_not_null(deep_parent);
    vfs_node_t *mnt_root = vfs_lookup("/mnt/deep/sub/../..");
    assert_not_null(mnt_root);
    vfs_node_t *global_root = vfs_lookup("/mnt/deep/sub/../../..");
    assert_not_null(global_root);
    vfs_node_t *expected_root = vfs_lookup("/");
    assert_true(global_root == expected_root);

    vfs_node_put(expected_root);
    vfs_node_put(global_root);
    vfs_node_put(mnt_root);
    vfs_node_put(deep_parent);

    /* Pending submount rollback when root contains a non-dir at pending mountpoint */
    vfs_init();
    assert_eq(0, vfs_register_filesystem(&s_root_conflict_fs));
    assert_eq(0, vfs_mount_fs("tmpfs", "/conflict_file", NULL, NULL));
    assert_eq(-ENOTDIR, vfs_mount_fs("root_conflict_tmpfs", "/", NULL, NULL));
    assert_null(vfs_lookup("/"));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_submount_downward_and_dotdot_upward_crossing),
    TEST_ENTRY(test_fresh_parent_lookup_and_case_insensitive_vfat_parent),
    TEST_ENTRY(test_getdents_submount_injection_and_deduplication),
    TEST_ENTRY(test_mount_protection_ebusy_and_mount_validation),
    TEST_ENTRY(test_pre_root_pending_submounts_depth_order_and_rollback),
TEST_LIST_END

int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
```

- [ ] **Step 2: Wire `test_vfs_mount_tree.elf` into `hosttests/Makefile`**

In [`hosttests/Makefile`](../../../hosttests/Makefile):
1. Add `$(TEST_BLD)/test_vfs_mount_tree.elf \` right after `$(TEST_BLD)/test_vfs_iops_fops.elf \` in `TEST_BINS` (around line 73).
2. Add the compile and link rules right after `$(TEST_BLD)/test_vfs_iops_fops.elf` (around line 618):

```makefile
$(TEST_BLD)/test_vfs_mount_tree.o: $(TEST_CASES)/test_vfs_mount_tree.c \
		$(TESTS_DIR)/kernel/include/fs/vfs.h \
		$(TEST_MOCK)/vfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VFS_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_vfs_mount_tree.elf: $(TEST_BLD)/test_vfs_mount_tree.o \
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

Run all required verification gates:
```bash
make test-host
make test-static
make PROFILE=x86_64-clang kernel
make PROFILE=aarch64-clang kernel
make test-qemu SUITE=phase-0
make KERNEL_SELFTEST=1 test-kernel-selftest
make OS01_SYSTEST=1 test-syscall
```
Expected: All host unit tests (including `test_vfs_mount_tree`), static audits, both architecture kernel builds, `phase-0`, `test-kernel-selftest`, and `test-syscall` pass with 0 failures.

- [ ] **Step 4: Stage and commit Task 3 (avoiding unrelated `fb_resolution` hunks in `hosttests/Makefile`)**

Stage `hosttests/cases/test_vfs_mount_tree.c` and only the `test_vfs_mount_tree` hunks in `hosttests/Makefile`, then commit:
```bash
git add hosttests/cases/test_vfs_mount_tree.c
# Stage only the test_vfs_mount_tree hunks in hosttests/Makefile
git commit -m "test(vfs): add host unit tests for Phase 3 hierarchical mount tree"
```
