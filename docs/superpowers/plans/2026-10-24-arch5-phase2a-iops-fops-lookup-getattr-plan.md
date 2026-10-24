# ARCH-5 Phase 2A: `i_ops`/`f_ops` Split, `lookup`/`getattr` & Node Metadata Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the monolithic `vfs_ops_t` table with `vfs_inode_ops_t` (`i_ops`) + `vfs_file_ops_t` (`f_ops`), enrich `vfs_node_t` with complete metadata (`ino`, `mode`, `uid`, `gid`, `nlink`, `rdev`, `atime`, `mtime`, `ctime`, `node_flags`, `mounted_here`, `mnt_active_next`), introduce `vfs_namespace_lock` (`mutex_t` with bootstrap bypass) and VFS namespace wrappers (`vfs_create_file`, `vfs_symlink`), and implement `i_ops->lookup` + `i_ops->getattr` across all five filesystems (`devfs`, `procfs`, `tmpfs`, `ext2`, `vfat`), eliminating the 256-entry `readdir` lookup cap.

**Architecture:**
Phase 2 of the ARCH-5 VFS specification is split into two focused sub-plans to keep each plan small and verifiable:
- **Phase 2A (this plan)**: Structural split of `vfs_ops_t` → `vfs_inode_ops_t` + `vfs_file_ops_t`, enriched `vfs_node_t` / `vfs_mount_t` structs, `vfs_node_stable_ino()`, `vfs_generic_getattr()`, `vfs_generic_lookup_by_readdir()` fallback, `vfs_namespace_lock` (`mutex_t` + `vfs_end_bootstrap()`), `vfs_create_file()` / `vfs_symlink()` migration in `sys_fs.c`, cross-mount `-EXDEV` check in `vfs_rename()`, and per-backend `i_ops->lookup` + `i_ops->getattr` across `devfs`, `procfs`, `tmpfs`, `ext2`, and `fat32`.
- **Phase 2B (next plan)**: Active-node tracking (`mp->active_lock`, `mp->active_nodes`), `vfs_node_put()` → `i_ops->evict_node(node)`, atomic `lookup`/`create`/`mkdir` registration under backend lifecycle locks, deferred orphan reclamation across `tmpfs`, `ext2`, and `fat32` (`fat_file_object`), and `write`/`truncate` active-node size synchronization.

**Tech Stack:** Freestanding C11 (`clang`, `lld`), OS01 kernel VFS (`kernel/fs/`, `kernel/syscall/sys_fs.c`, `kernel/memory/vma.c`), host unit-test runner (`hosttests/`), QEMU integration harness (`qemutests/`).

**Spec:** [`docs/superpowers/specs/2026-10-24-arch5-vfs-abstraction-design.md`](../specs/2026-10-24-arch5-vfs-abstraction-design.md) (§4.2, §4.3, §4.4, §5.2.1, §5.2.3, §5.5, §6, §7)

## Global Constraints

- **Symmetric header/source layout (`AGENTS.md`)**: Public headers for `kernel/fs/*.c` live in `kernel/include/fs/*.h` and are included as `<fs/foo.h>`.
- **Mandatory clean rebuild after struct changes (`AGENTS.md`)**: Because `vfs_node_t` and `vfs_mount_t` in `kernel/include/fs/vfs.h` change layout, `make clean` MUST be executed before building kernel images.
- **Complete removal of `vfs_ops_t` (Spec §5.5)**: All kernel and hosttest references to `struct vfs_ops` / `vfs_ops_t` / `node->ops` / `mp->ops` are migrated to `i_ops` and `f_ops` in Task 1.
- **`vfs_namespace_lock` bootstrap bypass (Spec §5.5)**: `mutex_lock()` in `kernel/sync/mutex.c` uses `current->pid` as the lock owner (`0` means unlocked). During early boot (`vfs_bootstrap == 1`) or on the idle task (`!current || current->pid <= 0`), `vfs_namespace_lock_acquire()` / `vfs_namespace_lock_release()` MUST be a no-op. `vfs_end_bootstrap()` is called in `task_init()` (`kernel/sched/core.c`) immediately after `this_cpu()->scheduler_ok = 1`.
- **`vfs_generic_getattr` `st_nlink` preservation (Spec §4.3)**: `vfs_generic_getattr()` MUST set `buf->st_nlink = node->nlink` verbatim (including `0` for unlinked/orphaned nodes) — never overwrite `node->nlink == 0` with `1`. Every constructed node (mount roots, `lookup`, `create`, `mkdir`, and `vfs_generic_lookup_by_readdir`) must initialize `node->nlink` (`2` for directories, `1` for files/symlinks/devices, or disk `i_links_count` for `ext2`).
- **Logging policy (`AGENTS.md`)**: Use `log_err`/`log_warn`/`log_info`/`log_debug` or `debug_vfs`/`debug_fs`; never use bare `serial_printk` or `color_printk` (except in existing `kernel/selftest/` protocol markers).

## Review Focus

1. **No remaining `vfs_ops` or `->ops` references on VFS structs**: `grep -rn "vfs_ops" kernel/ hosttests/` must return zero matches after Task 1 (note: `struct devfs_ops` in `devfs.h` is device-driver ops, not `vfs_ops`, and remains unchanged).
2. **Large directory lookup (>256 entries)**: `__vfs_lookup_raw()` must dispatch through `cur->i_ops->lookup(cur, comp, &child)` (falling back to `vfs_generic_lookup_by_readdir()` only when `cur->i_ops == NULL || cur->i_ops->lookup == NULL`), and looking up the 300th file in a 300-entry directory must succeed (`0`), never failing with `-ENOENT` due to the old `max_iter = 256` cap.
3. **Read-only FS error codes (`-EROFS` vs `-EOPNOTSUPP`)**: Missing `create`/`unlink`/`mkdir`/`rmdir`/`rename`/`truncate` in `i_ops` must return `-EROFS`, whereas missing `symlink`/`readlink` in `i_ops` must return `-EOPNOTSUPP` (preserving existing `systest` expectations).
4. **Cross-mount `rename` (`-EXDEV`)**: `vfs_rename()` must check `if (olddir->mount != newdir->mount)` before calling `olddir->i_ops->rename` and return `-EXDEV`.
5. **Real-time `getattr` metadata refresh**: `vfs_stat()` must call `node->i_ops->getattr(node, buf)` when present (`tmpfs_iop_getattr`, `ext2_iop_getattr`, `fat_iop_getattr`), refreshing `node->size` from the underlying storage so `stat()` after `write()`/`truncate()` via another handle sees the updated size.

---

## File Structure Map

| File | Action | Responsibility in Phase 2A |
|------|--------|----------------------------|
| `kernel/include/fs/vfs.h` | Modify | Replace `vfs_ops_t` with `vfs_inode_ops_t` + `vfs_file_ops_t`; enrich `vfs_node_t` and `vfs_mount_t`; define `VFS_NODE_*`, `vfs_node_stable_ino()`, `vfs_generic_getattr()`, `vfs_generic_lookup_by_readdir()`, `vfs_create_file()`, `vfs_symlink()`, `vfs_end_bootstrap()`. |
| `hosttests/include/fs/vfs.h` | Modify | Keep host shadow header synchronized with `vfs_inode_ops_t`, `vfs_file_ops_t`, `vfs_node_t`, and `vfs_mount_t`. |
| `kernel/fs/vfs.c` | Modify | Implement `vfs_namespace_lock` + bootstrap helpers, `vfs_generic_getattr()`, `vfs_generic_lookup_by_readdir()`, `vfs_create_file()`, `vfs_symlink()`, cross-mount `-EXDEV` in `vfs_rename()`, and switch all VFS operations to `i_ops` / `f_ops`. |
| `kernel/sched/core.c` | Modify | Call `vfs_end_bootstrap()` in `task_init()` after `this_cpu()->scheduler_ok = 1`. |
| `kernel/syscall/sys_fs.c` | Modify | Migrate `sys_symlink` to `vfs_symlink()`, `SYS_open(O_CREAT)` to `vfs_create_file()`, `SYS_open(O_TRUNC)` to `vfs_truncate()`, `sys_readlink` to `node->i_ops->readlink`, and `SYS_access(W_OK)` to `node->f_ops->write`. |
| `kernel/fs/file.c` | Modify | Switch `vfs_file_open`, `vfs_file_read`, `vfs_file_release`, `fd_read`, and `fd_write` from `node->ops` to `node->f_ops`. |
| `kernel/memory/vma.c` | Modify | Switch device `mmap` dispatch in `do_mmap` from `file_node->ops->mmap` to `file_node->f_ops->mmap`. |
| `kernel/fs/devfs.c` | Modify | Split `devfs_ops` into `devfs_iops` (`devfs_iop_lookup`) + `devfs_fops` (`read`, `write`, `readdir`, `mmap`, `read_file`). |
| `kernel/fs/procfs.c` | Modify | Split `procfs_ops` into `procfs_iops` (`procfs_iop_lookup`) + `procfs_fops` (`read`, `readdir`, `open_file`, `read_file`, `release_file`). |
| `kernel/fs/tmpfs.c` | Modify | Split `tmpfs_vfs_ops` into `tmpfs_iops` (`tmpfs_iop_lookup`, `tmpfs_iop_getattr`, dir/inode ops) + `tmpfs_fops` (`read`, `write`, `readdir`); add metadata fields to `tmpfs_node_t`. |
| `kernel/include/fs/ext2.h` | Modify | Replace `extern struct vfs_ops ext2_vfs_ops` with `extern const vfs_inode_ops_t ext2_iops` and `extern const vfs_file_ops_t ext2_fops`. |
| `kernel/fs/ext2.c` | Modify | Split `ext2_vfs_ops` into `ext2_iops` (`ext2_iop_lookup`, `ext2_iop_getattr`, dir/inode ops) + `ext2_fops` (`read`, `write`, `readdir`); populate full inode metadata on mount/lookup/create/mkdir. |
| `kernel/include/fs/fat.h` | Modify | Replace `extern struct vfs_ops fat_vfs_ops` with `extern const vfs_inode_ops_t fat_iops` and `extern const vfs_file_ops_t fat_fops`. |
| `kernel/fs/fat.c` | Modify | Split `fat_vfs_ops` into `fat_iops` (`fat_iop_lookup`, `fat_iop_getattr`, dir/inode ops) + `fat_fops` (`read`, `write`, `readdir`); populate metadata on mount/lookup/create/mkdir. |
| `kernel/selftest/symlink_selftest.c` | Modify | Migrate `fake_ops`, `fake_ops_no_readlink`, and `ext2` rollback assertions to `i_ops` / `f_ops`. |
| `hosttests/mock/vfs_test_runtime.h` | Modify | Add no-op `mutex_t` stubs (`_KERNEL_MUTEX_H`) for single-threaded VFS host tests. |
| `hosttests/mock/gfx_test_runtime.h` | Modify | Update `vfs_mount` stub signature to `const vfs_inode_ops_t *i_ops, const vfs_file_ops_t *f_ops`. |
| `hosttests/cases/*.c` | Modify | Update existing host tests (`test_vfs_basic.c`, `test_vfs_fs_registry.c`, `test_gfx_file_lifecycle.c`, `test_pty_winsize.c`, `test_monitor_file.c`, `test_poll_requested.c`, `test_gfx_device.c`, `test_devfs_input_dispatch.c`, `test_monitor_procfs.c`, `test_device_boot.c`) from `vfs_ops_t` to `i_ops`/`f_ops`. |
| `hosttests/cases/test_vfs_iops_fops.c` | Create | New Phase 2A unit test suite covering 300-entry `i_ops->lookup`, custom `getattr` vs `vfs_generic_getattr`, `ext2`/`tmpfs`/`devfs` metadata, `write`/`truncate` `stat` consistency, `-EROFS`/`-EOPNOTSUPP`, and cross-mount `-EXDEV`. |
| `hosttests/Makefile` | Modify | Add `test_vfs_iops_fops.elf` build rule and register in `TEST_BINS`. |

---

### Task 1: Split `vfs_ops_t` into `i_ops`/`f_ops`, Enrich VFS Node/Mount Structs, Add `vfs_namespace_lock`, and Migrate Callers

**Files:**
- Modify: `kernel/include/fs/vfs.h`
- Modify: `hosttests/include/fs/vfs.h`
- Modify: `kernel/fs/vfs.c`
- Modify: `kernel/sched/core.c:584-592`
- Modify: `kernel/syscall/sys_fs.c:75-140,330-396,860-875`
- Modify: `kernel/fs/file.c:660-730,1025-1040`
- Modify: `kernel/memory/vma.c:700-715`
- Modify: `kernel/selftest/symlink_selftest.c:150-180,290-415`
- Modify: `kernel/fs/devfs.c:450-460,610-650`
- Modify: `kernel/fs/procfs.c:515-535`
- Modify: `kernel/fs/tmpfs.c:24-26,280-285,325-332,414-455`
- Modify: `kernel/include/fs/ext2.h:126-129`
- Modify: `kernel/fs/ext2.c:835-842,1145-1152,1536-1551,1630-1636,2070-2106`
- Modify: `kernel/include/fs/fat.h:117-120`
- Modify: `kernel/fs/fat.c:950-957,1245-1268,1355-1361`
- Modify: `hosttests/mock/vfs_test_runtime.h`
- Modify: `hosttests/mock/gfx_test_runtime.h:118-122`
- Modify: `hosttests/cases/test_vfs_basic.c`
- Modify: `hosttests/cases/test_vfs_fs_registry.c`
- Modify: `hosttests/cases/test_gfx_file_lifecycle.c`
- Modify: `hosttests/cases/test_pty_winsize.c`
- Modify: `hosttests/cases/test_monitor_file.c`
- Modify: `hosttests/cases/test_poll_requested.c`
- Modify: `hosttests/cases/test_gfx_device.c`
- Modify: `hosttests/cases/test_devfs_input_dispatch.c`
- Modify: `hosttests/cases/test_monitor_procfs.c`
- Modify: `hosttests/cases/test_device_boot.c`

- [ ] **Step 1: Update `kernel/include/fs/vfs.h` and `hosttests/include/fs/vfs.h`**

In `kernel/include/fs/vfs.h`, add `#include <arch/spinlock.h>` at the top (after `<sys/stat.h>`), and replace lines 39-171 (`vfs_ops_t`, `vfs_fs_type_t`, `vfs_mount_t`, `vfs_dirent_t`, `vfs_node_t`, and mount declarations) with:

```c
// ── Inode & File operations tables (Spec §4.2) ────────────
typedef struct vfs_inode_ops {
    // Directory lookup: search `dir` for `name`; on hit return 0 and
    // output a vfs_node_t with refcount >= 1 in *out; on miss return -ENOENT.
    int              (*lookup)(struct vfs_node *dir, const char *name,
                               struct vfs_node **out);

    // Custom attribute query: fill struct stat (returns 0 on success).
    // If NULL, VFS falls back to vfs_generic_getattr().
    int              (*getattr)(struct vfs_node *node, struct stat *buf);

    // Called from vfs_node_put() when refcount drops to 0 before freeing
    // the vfs_node_t wrapper.
    void             (*evict_node)(struct vfs_node *node);

    // Namespace & metadata mutation hooks
    struct vfs_node *(*create)(struct vfs_node *dir, const char *name);
    int              (*unlink)(struct vfs_node *dir, const char *name);
    struct vfs_node *(*mkdir)(struct vfs_node *dir, const char *name);
    int              (*rmdir)(struct vfs_node *dir, const char *name);
    int              (*rename)(struct vfs_node *olddir, const char *oldname,
                               struct vfs_node *newdir, const char *newname);
    int              (*truncate)(struct vfs_node *node, uint64_t new_size);

    // Symbolic link hooks
    int              (*symlink)(struct vfs_node *parent, const char *name,
                                const char *target);
    int              (*readlink)(struct vfs_node *node, char *buf, size_t size);
} vfs_inode_ops_t;

typedef struct vfs_file_ops {
    int     (*read)(struct vfs_node *node, uint64_t offset,
                    uint64_t size, void *buffer);
    int     (*write)(struct vfs_node *node, uint64_t offset,
                     uint64_t size, void *buffer);
    int     (*readdir)(struct vfs_node *node, uint64_t index,
                       struct vfs_dirent *entry);
    int     (*mmap)(struct vfs_node *node, struct vma *vma);

    // Optional per-file snapshot lifecycle hooks (procfs statistics, etc.)
    int     (*open_file)(struct file *f);
    int64_t (*read_file)(struct file *f, uint64_t offset,
                         uint64_t size, void *buffer);
    void    (*release_file)(struct file *f);
} vfs_file_ops_t;

// ── Filesystem type registration (Spec §4.1) ──────────────
#define VFS_FS_REQUIRES_DEV      (1u << 0)
#define VFS_FS_CASE_INSENSITIVE  (1u << 1)
#define VFS_FS_NO_DCACHE         (1u << 2)

typedef struct vfs_fs_type {
    const char         *name;
    uint32_t            fs_flags;
    int               (*mount)(struct vfs_mount *mp, block_device_t *dev, void *data);
    void              (*unmount)(struct vfs_mount *mp);
    struct vfs_fs_type *next;
} vfs_fs_type_t;

// ── A mounted filesystem instance (Spec §4.4) ─────────────
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

// ── Directory entry returned by readdir ───────────────────
typedef struct vfs_dirent {
    char     *name;          // caller-provided buffer (VFS_NAME_MAX bytes)
    uint64_t size;
    uint8_t  type;           // VFS_FILE or VFS_DIR
    uint64_t ino;            // filesystem-specific id (must hold kernel pointer for tmpfs)
} vfs_dirent_t;

// ── VFS node flags & structure (Spec §4.3) ────────────────
#define VFS_NODE_NO_DCACHE   (1u << 0)
#define VFS_NODE_MOUNT_STUB  (1u << 1)
#define VFS_NODE_ORPHANED    (1u << 2)
#define VFS_NODE_STALE       (1u << 3)

typedef struct vfs_node {
    char                  *name;
    uint64_t               ino;
    uint64_t               size;
    uint32_t               mode;
    uint32_t               uid;
    uint32_t               gid;
    uint32_t               nlink;
    uint64_t               rdev;
    uint64_t               atime;
    uint64_t               mtime;
    uint64_t               ctime;
    uint8_t                type;
    uint8_t                node_flags;
    struct vfs_mount      *mount;
    struct vfs_mount      *mounted_here;
    void                  *fs_data;
    struct vfs_node       *parent;
    const vfs_inode_ops_t *i_ops;
    const vfs_file_ops_t  *f_ops;
    uint32_t               refcount;
    struct vfs_node       *mnt_active_next;
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

// ── VFS API ───────────────────────────────────────────────

void vfs_init(void);
void vfs_end_bootstrap(void);

int                 vfs_register_filesystem(vfs_fs_type_t *fs_type);
int                 vfs_unregister_filesystem(const char *name);
const vfs_fs_type_t *vfs_find_filesystem(const char *name);
int                 vfs_mount_fs(const char *type_name, const char *path,
                                 block_device_t *dev, void *data);

// Transitional mount helper using i_ops + f_ops
int vfs_mount(const char *path, block_device_t *dev,
              const vfs_inode_ops_t *i_ops, const vfs_file_ops_t *f_ops,
              void *fs_data);

int vfs_generic_getattr(vfs_node_t *node, struct stat *buf);
int vfs_generic_lookup_by_readdir(vfs_node_t *dir, const char *name,
                                  vfs_node_t **out);
int vfs_create_file(const char *path, const char *cwd, vfs_node_t **out_node);
int vfs_symlink(const char *target, const char *linkpath, const char *cwd);
```

In `hosttests/include/fs/vfs.h`, replace `vfs_ops_t`, `vfs_mount_t`, `vfs_node_t`, and `vfs_mount` with the matching `vfs_inode_ops_t` and `vfs_file_ops_t` definitions (keeping `char name[VFS_NAME_MAX]` on `vfs_dirent_t` and `vfs_node_t` in `hosttests/include/fs/vfs.h` if any legacy self-contained test uses it, or adding `i_ops` and `f_ops` to `vfs_node_t` and `vfs_mount_t`). Specifically, update `hosttests/include/fs/vfs.h` lines 15-115 to:

```c
// ── Forward declarations ──────────────────────────────────
struct vfs_node;
struct vfs_mount;
struct vfs_dirent;
struct vfs_fs_type;
struct vma;
struct file;

typedef struct vfs_inode_ops {
    int              (*lookup)(struct vfs_node *dir, const char *name,
                               struct vfs_node **out);
    int              (*getattr)(struct vfs_node *node, struct stat *buf);
    void             (*evict_node)(struct vfs_node *node);
    struct vfs_node *(*create)(struct vfs_node *dir, const char *name);
    int              (*unlink)(struct vfs_node *dir, const char *name);
    struct vfs_node *(*mkdir)(struct vfs_node *dir, const char *name);
    int              (*rmdir)(struct vfs_node *dir, const char *name);
    int              (*rename)(struct vfs_node *olddir, const char *oldname,
                               struct vfs_node *newdir, const char *newname);
    int              (*truncate)(struct vfs_node *node, uint64_t new_size);
    int              (*symlink)(struct vfs_node *parent, const char *name,
                                const char *target);
    int              (*readlink)(struct vfs_node *node, char *buf, size_t size);
} vfs_inode_ops_t;

typedef struct vfs_file_ops {
    int     (*read)(struct vfs_node *node, uint64_t offset,
                    uint64_t size, void *buffer);
    int     (*write)(struct vfs_node *node, uint64_t offset,
                     uint64_t size, void *buffer);
    int     (*readdir)(struct vfs_node *node, uint64_t index,
                       struct vfs_dirent *entry);
    int     (*mmap)(struct vfs_node *node, struct vma *vma);
    int     (*open_file)(struct file *f);
    int64_t (*read_file)(struct file *f, uint64_t offset,
                         uint64_t size, void *buffer);
    void    (*release_file)(struct file *f);
} vfs_file_ops_t;

// ── Filesystem type registration ──────────────────────────
#define VFS_FS_REQUIRES_DEV      (1u << 0)
#define VFS_FS_CASE_INSENSITIVE  (1u << 1)
#define VFS_FS_NO_DCACHE         (1u << 2)

typedef struct vfs_fs_type {
    const char         *name;
    uint32_t            fs_flags;
    int               (*mount)(struct vfs_mount *mp, block_device_t *dev, void *data);
    void              (*unmount)(struct vfs_mount *mp);
    struct vfs_fs_type *next;
} vfs_fs_type_t;

// ── A mounted filesystem instance ─────────────────────────
typedef struct vfs_mount {
    block_device_t            *dev;
    const char                *path;
    struct vfs_node           *root;
    const vfs_inode_ops_t     *i_ops;
    const vfs_file_ops_t      *f_ops;
    void                      *fs_data;
    const struct vfs_fs_type  *fs_type;
    uint32_t                   mnt_flags;
} vfs_mount_t;

// ── Directory entry returned by readdir ───────────────────
typedef struct vfs_dirent {
    char     name[VFS_NAME_MAX];
    uint64_t size;
    uint8_t  type;
    uint64_t ino;
} vfs_dirent_t;

// ── VFS node (inode) ──────────────────────────────────────
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
} vfs_node_t;

// ── VFS API ───────────────────────────────────────────────

void vfs_init(void);
void vfs_end_bootstrap(void);

int                 vfs_register_filesystem(vfs_fs_type_t *fs_type);
int                 vfs_unregister_filesystem(const char *name);
const vfs_fs_type_t *vfs_find_filesystem(const char *name);
int                 vfs_mount_fs(const char *type_name, const char *path,
                                 block_device_t *dev, void *data);

int vfs_mount(const char *path, block_device_t *dev,
              const vfs_inode_ops_t *i_ops, const vfs_file_ops_t *f_ops,
              void *fs_data);
```

- [ ] **Step 2: Update `hosttests/mock/vfs_test_runtime.h` with `mutex_t` host stubs**

Because `kernel/fs/vfs.c` now includes `<sync/mutex.h>` for `vfs_namespace_lock`, add a lightweight host stub for `mutex_t` in `hosttests/mock/vfs_test_runtime.h`:

```c
#ifndef OS01_VFS_TEST_RUNTIME_H
#define OS01_VFS_TEST_RUNTIME_H

#include "test_platform.h"
#include <list.h>
#include <uapi/time.h>

#define KERNEL_TASK_H
#define _KERNEL_PERCPU_H
#define _ARCH_IRQ_H
#define _ARCH_CPU_H
#define _ARCH_SPINLOCK_H
#define _KERNEL_SLAB_H
#define _KERNEL_MEMORY_H
#define _KERNEL_MUTEX_H

typedef struct {
    volatile int64_t owner;
} mutex_t;

static inline void mutex_init(mutex_t *m) { if (m) m->owner = 0; }
static inline void mutex_lock(mutex_t *m) { if (m) m->owner = 1; }
static inline int  mutex_trylock(mutex_t *m) { if (m && m->owner == 0) { m->owner = 1; return 1; } return 0; }
static inline void mutex_unlock(mutex_t *m) { if (m) m->owner = 0; }
static inline int  mutex_lock_interruptible(mutex_t *m) { mutex_lock(m); return 0; }

struct files_struct;

typedef struct task_struct {
    list_t list;
    volatile int64_t state;
    uint64_t addr_limit;
    int64_t pid;
    pid_t pgrp;
    pid_t session;
    int64_t signal;
    int64_t blocked;
    struct files_struct *files;
    list_t io_wait_node;
    int ctty_type;
    void *ctty;
} task_t;

#include <fs/file.h>

extern task_t *vfs_test_current;
#define current vfs_test_current

void *kmalloc(size_t size);
size_t kfree(void *ptr);

#endif /* OS01_VFS_TEST_RUNTIME_H */
```

- [ ] **Step 3: Update `kernel/fs/vfs.c` (`vfs_namespace_lock`, `i_ops`/`f_ops` dispatch, `vfs_generic_getattr`, `vfs_generic_lookup_by_readdir`, `vfs_create_file`, `vfs_symlink`, `-EXDEV`)**

In `kernel/fs/vfs.c`:
1. Include `<sync/mutex.h>` after `<sync/rwlock.h>`.
2. Add `vfs_namespace_lock` and `vfs_bootstrap` state near `mount_lock`:
   ```c
   static mutex_t vfs_namespace_lock;
   static int vfs_bootstrap = 1;

   void vfs_end_bootstrap(void)
   {
       vfs_bootstrap = 0;
   }

   static inline void vfs_ns_lock(void)
   {
       if (!vfs_bootstrap && current && current->pid > 0) {
           mutex_lock(&vfs_namespace_lock);
       }
   }

   static inline void vfs_ns_unlock(void)
   {
       if (!vfs_bootstrap && current && current->pid > 0) {
           mutex_unlock(&vfs_namespace_lock);
       }
   }
   ```
3. In `vfs_init()`:
   ```c
   mutex_init(&vfs_namespace_lock);
   vfs_bootstrap = 1;
   ```
4. Update `vfs_mount()`:
   ```c
   int vfs_mount(const char *path, block_device_t *dev,
                 const vfs_inode_ops_t *i_ops, const vfs_file_ops_t *f_ops,
                 void *fs_data)
   {
       if (!vfs_initialized) vfs_init();

       vfs_mount_t *mp = (vfs_mount_t *)calloc(1, sizeof(vfs_mount_t));
       if (!mp) {
           debug_vfs("VFS: mount: out of memory\n");
           return -1;
       }

       spin_init(&mp->active_lock);
       mp->active_nodes = NULL;
       mp->dev = dev;
       mp->path = path;
       mp->i_ops = i_ops;
       mp->f_ops = f_ops;
       mp->fs_data = fs_data;
       mp->root = NULL;

       mp->root = (vfs_node_t *)calloc(1, sizeof(vfs_node_t));
       if (!mp->root) {
           debug_vfs("VFS: mount: root node alloc failed\n");
           kfree(mp);
           return -1;
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
           kfree(mp->root);
           kfree(mp);
           return -1;
       }

       rwlock_write_lock(&mount_lock);
       mp->next = mount_list;
       mount_list = mp;
       mount_count++;
       rwlock_write_unlock(&mount_lock);
       debug_vfs("VFS: mounted '%s'\n", path);
       return 0;
   }
   ```
5. In `vfs_mount_fs()`, initialize `spin_init(&mp->active_lock); mp->active_nodes = NULL;` and on `mp->root`:
   ```c
   mp->root->mount = mp;
   mp->root->type = VFS_DIR;
   mp->root->ino = VFS_SYNTH_ROOT_INO;
   mp->root->mode = S_IFDIR | 0755;
   mp->root->nlink = 2;
   mp->root->refcount = 1;
   ```
   And after `fs_type->mount(mp, dev, data)` succeeds, replace the old `mp->ops` sync with:
   ```c
   mp->root->mount = mp;
   if (mp->i_ops && !mp->root->i_ops) {
       mp->root->i_ops = mp->i_ops;
   } else if (!mp->i_ops && mp->root->i_ops) {
       mp->i_ops = mp->root->i_ops;
   }
   if (mp->f_ops && !mp->root->f_ops) {
       mp->root->f_ops = mp->f_ops;
   } else if (!mp->f_ops && mp->root->f_ops) {
       mp->f_ops = mp->root->f_ops;
   }
   if (mp->root->nlink == 0) {
       mp->root->nlink = 2;
   }
   if (mp->root->mode == 0) {
       mp->root->mode = S_IFDIR | 0755;
   }
   ```
6. Implement `vfs_generic_lookup_by_readdir()`:
   ```c
   int vfs_generic_lookup_by_readdir(vfs_node_t *dir, const char *name,
                                     vfs_node_t **out)
   {
       if (!out) return -EINVAL;
       *out = NULL;
       if (!dir || !name) return -EINVAL;
       if (dir->type != VFS_DIR) return -ENOTDIR;

       char *entry_name = kmalloc(VFS_NAME_MAX);
       if (!entry_name) return -ENOMEM;

       vfs_dirent_t entry;
       memset(&entry, 0, sizeof(entry));
       entry.name = entry_name;

       int found = 0;
       uint64_t idx = 0;
       int max_iter = 65536;

       while (max_iter-- > 0) {
           entry_name[0] = '\0';
           int r = vfs_readdir(dir, idx, &entry);
           if (r != 0) {
               kfree(entry_name);
               return -EIO;
           }
           if (entry.name[0] == '\0') break;

           int match;
           if (dir->mount && (dir->mount->mnt_flags & VFS_FS_CASE_INSENSITIVE))
               match = (vfs_name_cmp(entry.name, name) == 0);
           else
               match = (strcmp(entry.name, name) == 0);

           if (match) {
               found = 1;
               break;
           }
           idx++;
       }

       if (!found) {
           kfree(entry_name);
           return -ENOENT;
       }

       vfs_node_t *child = (vfs_node_t *)calloc(1, sizeof(vfs_node_t));
       if (!child) {
           kfree(entry_name);
           return -ENOMEM;
       }

       child->mount = dir->mount;
       child->parent = dir;
       __sync_add_and_fetch(&dir->refcount, 1);
       child->type = entry.type;
       child->size = entry.size;
       child->ino = entry.ino;
       child->nlink = (entry.type == VFS_DIR) ? 2 : 1;
       child->i_ops = dir->i_ops;
       child->f_ops = dir->f_ops;
       child->fs_data = (void *)(uintptr_t)entry.ino;
       child->refcount = 1;
       child->name = strndup(entry.name, VFS_NAME_MAX - 1);
       kfree(entry_name);
       if (!child->name) {
           __sync_sub_and_fetch(&dir->refcount, 1);
           kfree(child);
           return -ENOMEM;
       }

       *out = child;
       return 0;
   }
   ```
7. In `__vfs_lookup_raw()`:
   - Remove `char entry_name[VFS_NAME_MAX];` from `struct vfs_raw_lookup_buffers`.
   - Replace `if (!mp || !mp->root || !mp->root->ops)` with `if (!mp || !mp->root || (!mp->root->i_ops && !mp->root->f_ops))`.
   - Replace the old `max_iter = 256` `vfs_readdir` loop and `child` allocation (lines 637-689) with:
     ```c
     vfs_node_t *child = NULL;
     int lrc;
     if (cur->i_ops && vfs_is_valid_kptr(cur->i_ops) &&
         cur->i_ops->lookup && vfs_is_valid_kptr(cur->i_ops->lookup)) {
         lrc = cur->i_ops->lookup(cur, comp, &child);
     } else {
         lrc = vfs_generic_lookup_by_readdir(cur, comp, &child);
     }
     if (lrc < 0 || !child) {
         __sync_sub_and_fetch(&cur->refcount, 1);
         ret = (lrc < 0) ? lrc : -ENOENT;
         goto out;
     }
     if (!child->mount) child->mount = mp;
     if (!child->parent) {
         child->parent = cur;
         __sync_add_and_fetch(&cur->refcount, 1);
     }
     if (!child->i_ops) child->i_ops = cur->i_ops;
     if (!child->f_ops) child->f_ops = cur->f_ops;
     ```
   - And on error cleanup when `consumed_out` or `remaining_out` overflows (lines 694-696 and 716-718), call `vfs_node_put(child);` instead of manual `free(child->name); free(child);` so `cur->refcount` is balanced!
8. In `vfs_lookup_resolved()` (lines 919-925):
   - Replace `node->ops->readlink` with:
     ```c
     if (!node->i_ops || !vfs_is_valid_kptr(node->i_ops) ||
         !node->i_ops->readlink || !vfs_is_valid_kptr(node->i_ops->readlink)) {
         vfs_node_put(node);
         ret = -EOPNOTSUPP;
         goto out;
     }

     int tlen = node->i_ops->readlink(node, ctx->target, VFS_NAME_MAX - 1);
     ```
9. Update `vfs_read`, `vfs_write`, `vfs_readdir`:
   ```c
   int vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
   {
       if (!node || !node->f_ops || !vfs_is_valid_kptr(node->f_ops))
           return -1;
       if (!node->f_ops->read || !vfs_is_valid_kptr(node->f_ops->read))
           return -1;
       return node->f_ops->read(node, offset, size, buffer);
   }

   int vfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
   {
       if (!node || !node->f_ops || !vfs_is_valid_kptr(node->f_ops))
           return -1;
       if (!node->f_ops->write || !vfs_is_valid_kptr(node->f_ops->write))
           return -1;
       return node->f_ops->write(node, offset, size, buffer);
   }

   int vfs_readdir(vfs_node_t *dir, uint64_t index, vfs_dirent_t *entry)
   {
       if (!dir || !dir->f_ops || !vfs_is_valid_kptr(dir->f_ops))
           return -1;
       if (!dir->f_ops->readdir || !vfs_is_valid_kptr(dir->f_ops->readdir))
           return -1;
       return dir->f_ops->readdir(dir, index, entry);
   }
   ```
10. In `vfs_node_put()`:
    ```c
    void vfs_node_put(vfs_node_t *node)
    {
        if (!node) return;
        if (__sync_sub_and_fetch(&node->refcount, 1) == 0) {
            if (node->i_ops && vfs_is_valid_kptr(node->i_ops) &&
                node->i_ops->evict_node && vfs_is_valid_kptr(node->i_ops->evict_node)) {
                node->i_ops->evict_node(node);
            }
            vfs_node_t *parent = node->parent;
            node->i_ops   = NULL;
            node->f_ops   = NULL;
            node->parent  = NULL;
            node->mount   = NULL;
            node->fs_data = NULL;
            if (node->name) kfree(node->name);
            node->name = NULL;
            free(node);
            vfs_node_put(parent);
        }
    }
    ```
11. Implement `vfs_generic_getattr()` and update `vfs_stat()`:
    ```c
    int vfs_generic_getattr(vfs_node_t *node, struct stat *buf)
    {
        if (!node || !buf) return -1;

        memset(buf, 0, sizeof(struct stat));

        buf->st_ino = vfs_node_stable_ino(node);
        buf->st_size = (int64_t)node->size;

        if (node->mode != 0) {
            uint32_t m = node->mode;
            if ((m & S_IFMT) == 0) {
                switch (node->type) {
                case VFS_FILE:    m |= S_IFREG; break;
                case VFS_DIR:     m |= S_IFDIR; break;
                case VFS_CHRDEV:  m |= S_IFCHR; break;
                case VFS_BLKDEV:  m |= S_IFBLK; break;
                case VFS_SYMLINK: m |= S_IFLNK; break;
                default: break;
                }
            }
            buf->st_mode = m;
        } else {
            switch (node->type) {
            case VFS_FILE:    buf->st_mode = S_IFREG | 0755; break;
            case VFS_DIR:     buf->st_mode = S_IFDIR | 0755; break;
            case VFS_CHRDEV:  buf->st_mode = S_IFCHR | 0600; break;
            case VFS_BLKDEV:  buf->st_mode = S_IFBLK | 0600; break;
            case VFS_SYMLINK: buf->st_mode = S_IFLNK | 0777; break;
            default:          buf->st_mode = 0; break;
            }
        }

        buf->st_uid = node->uid;
        buf->st_gid = node->gid;
        buf->st_nlink = node->nlink;
        buf->st_rdev = node->rdev;
        buf->st_blksize = 512;
        buf->st_blocks = (node->size + 511) / 512;
        buf->st_atime = node->atime;
        buf->st_mtime = node->mtime;
        buf->st_ctime = node->ctime;

        return 0;
    }

    int vfs_stat(vfs_node_t *node, struct stat *buf)
    {
        if (!node || !buf) return -1;
        if (node->i_ops && vfs_is_valid_kptr(node->i_ops) &&
            node->i_ops->getattr && vfs_is_valid_kptr(node->i_ops->getattr)) {
            return node->i_ops->getattr(node, buf);
        }
        return vfs_generic_getattr(node, buf);
    }
    ```
12. In `vfs_getdents()` (line 1234):
    Replace `if ((dir->mount && (dir->mount->mnt_flags & VFS_FS_CASE_INSENSITIVE)) || (dir->ops ...))` with:
    ```c
    if (dir->mount && (dir->mount->mnt_flags & VFS_FS_CASE_INSENSITIVE))
        cmp = vfs_name_cmp(entries[j].name, entries[j + 1].name);
    else
        cmp = strcmp(entries[j].name, entries[j + 1].name);
    ```
13. Update `vfs_unlink`, `vfs_mkdir`, `vfs_rmdir`, `vfs_rename`, `vfs_truncate` and add `vfs_create_file` and `vfs_symlink` (all using `i_ops` and `vfs_ns_lock()` / `vfs_ns_unlock()`):
    ```c
    int vfs_create_file(const char *path, const char *cwd, vfs_node_t **out_node)
    {
        if (!out_node) return -EINVAL;
        *out_node = NULL;
        if (!path) return -EINVAL;

        char *parent_path = kmalloc(VFS_NAME_MAX);
        if (!parent_path) return -ENOMEM;

        const char *name = vfs_split_parent(path, cwd, parent_path);
        if (!name || *name == '\0') {
            kfree(parent_path);
            return -EINVAL;
        }

        vfs_ns_lock();
        vfs_node_t *parent = vfs_lookup_from(parent_path, cwd);
        kfree(parent_path);
        if (!parent) {
            vfs_ns_unlock();
            return -ENOENT;
        }
        if (parent->type != VFS_DIR) {
            vfs_node_put(parent);
            vfs_ns_unlock();
            return -ENOTDIR;
        }
        if (!parent->i_ops || !vfs_is_valid_kptr(parent->i_ops) || !parent->i_ops->create) {
            vfs_node_put(parent);
            vfs_ns_unlock();
            return -EROFS;
        }
        if (!vfs_is_valid_kptr(parent->i_ops->create)) {
            vfs_node_put(parent);
            vfs_ns_unlock();
            return -1;
        }

        vfs_node_t *node = parent->i_ops->create(parent, name);
        vfs_node_put(parent);
        vfs_ns_unlock();
        if (!node) return -EEXIST;
        *out_node = node;
        return 0;
    }

    int vfs_symlink(const char *target, const char *linkpath, const char *cwd)
    {
        if (!target || !linkpath) return -EINVAL;
        if (target[0] == '\0') return -ENOENT;
        if (linkpath[0] == '/' && linkpath[1] == '\0') return -EEXIST;

        char *parent_path = kmalloc(VFS_NAME_MAX);
        if (!parent_path) return -ENOMEM;

        const char *name = vfs_split_parent(linkpath, cwd, parent_path);
        if (!name || *name == '\0') {
            kfree(parent_path);
            return -EINVAL;
        }

        vfs_ns_lock();
        vfs_node_t *parent = vfs_lookup_from(parent_path, cwd);
        kfree(parent_path);
        if (!parent) {
            vfs_ns_unlock();
            return -ENOENT;
        }
        if (parent->type != VFS_DIR) {
            vfs_node_put(parent);
            vfs_ns_unlock();
            return -ENOTDIR;
        }
        if (!parent->i_ops || !vfs_is_valid_kptr(parent->i_ops) ||
            !parent->i_ops->symlink || !vfs_is_valid_kptr(parent->i_ops->symlink)) {
            vfs_node_put(parent);
            vfs_ns_unlock();
            return -EOPNOTSUPP;
        }

        int ret = parent->i_ops->symlink(parent, name, target);
        vfs_node_put(parent);
        vfs_ns_unlock();
        return ret;
    }

    int vfs_unlink(const char *path, const char *cwd)
    {
        if (!path) return -EINVAL;

        char parent_path[VFS_NAME_MAX];
        const char *name = vfs_split_parent(path, cwd, parent_path);
        if (!name || *name == '\0') return -EINVAL;

        vfs_ns_lock();
        vfs_node_t *parent = vfs_lookup_from(parent_path, cwd);
        if (!parent) { vfs_ns_unlock(); return -ENOENT; }
        if (parent->type != VFS_DIR) { vfs_node_put(parent); vfs_ns_unlock(); return -ENOTDIR; }
        if (!parent->i_ops || !vfs_is_valid_kptr(parent->i_ops) || !parent->i_ops->unlink) {
            vfs_node_put(parent);
            vfs_ns_unlock();
            return -EROFS;
        }
        if (!vfs_is_valid_kptr(parent->i_ops->unlink)) {
            vfs_node_put(parent);
            vfs_ns_unlock();
            return -1;
        }

        int ret = parent->i_ops->unlink(parent, name);
        vfs_node_put(parent);
        vfs_ns_unlock();
        return ret;
    }

    int vfs_mkdir(const char *path, const char *cwd)
    {
        if (!path) return -EINVAL;

        char parent_path[VFS_NAME_MAX];
        const char *name = vfs_split_parent(path, cwd, parent_path);
        if (!name || *name == '\0') return -EINVAL;

        vfs_ns_lock();
        vfs_node_t *parent = vfs_lookup_from(parent_path, cwd);
        if (!parent) { vfs_ns_unlock(); return -ENOENT; }
        if (parent->type != VFS_DIR) { vfs_node_put(parent); vfs_ns_unlock(); return -ENOTDIR; }
        if (!parent->i_ops || !vfs_is_valid_kptr(parent->i_ops) || !parent->i_ops->mkdir) {
            vfs_node_put(parent);
            vfs_ns_unlock();
            return -EROFS;
        }
        if (!vfs_is_valid_kptr(parent->i_ops->mkdir)) {
            vfs_node_put(parent);
            vfs_ns_unlock();
            return -1;
        }

        vfs_node_t *newdir = parent->i_ops->mkdir(parent, name);
        if (!newdir) { vfs_node_put(parent); vfs_ns_unlock(); return -EEXIST; }

        vfs_node_put(newdir);
        vfs_node_put(parent);
        vfs_ns_unlock();
        return 0;
    }

    int vfs_rmdir(const char *path, const char *cwd)
    {
        if (!path) return -EINVAL;

        char parent_path[VFS_NAME_MAX];
        const char *name = vfs_split_parent(path, cwd, parent_path);
        if (!name || *name == '\0') return -EINVAL;

        vfs_ns_lock();
        vfs_node_t *parent = vfs_lookup_from(parent_path, cwd);
        if (!parent) { vfs_ns_unlock(); return -ENOENT; }
        if (parent->type != VFS_DIR) { vfs_node_put(parent); vfs_ns_unlock(); return -ENOTDIR; }
        if (!parent->i_ops || !vfs_is_valid_kptr(parent->i_ops) || !parent->i_ops->rmdir) {
            vfs_node_put(parent);
            vfs_ns_unlock();
            return -EROFS;
        }
        if (!vfs_is_valid_kptr(parent->i_ops->rmdir)) {
            vfs_node_put(parent);
            vfs_ns_unlock();
            return -1;
        }

        int ret = parent->i_ops->rmdir(parent, name);
        vfs_node_put(parent);
        vfs_ns_unlock();
        return ret;
    }

    int vfs_rename(const char *oldpath, const char *newpath, const char *cwd)
    {
        if (!oldpath || !newpath) return -EINVAL;

        char *parents = kmalloc(2 * VFS_NAME_MAX);
        if (!parents) return -ENOMEM;
        char *old_parent = parents;
        char *new_parent = parents + VFS_NAME_MAX;

        const char *oldname = vfs_split_parent(oldpath, cwd, old_parent);
        const char *newname = vfs_split_parent(newpath, cwd, new_parent);
        if (!oldname || *oldname == '\0' || !newname || *newname == '\0') {
            kfree(parents);
            return -EINVAL;
        }

        vfs_ns_lock();
        vfs_node_t *olddir = vfs_lookup_from(old_parent, cwd);
        if (!olddir) { kfree(parents); vfs_ns_unlock(); return -ENOENT; }
        if (olddir->type != VFS_DIR) {
            vfs_node_put(olddir);
            kfree(parents);
            vfs_ns_unlock();
            return -ENOTDIR;
        }

        vfs_node_t *newdir = vfs_lookup_from(new_parent, cwd);
        if (!newdir) {
            vfs_node_put(olddir);
            kfree(parents);
            vfs_ns_unlock();
            return -ENOENT;
        }
        if (newdir->type != VFS_DIR) {
            vfs_node_put(olddir);
            vfs_node_put(newdir);
            kfree(parents);
            vfs_ns_unlock();
            return -ENOTDIR;
        }

        kfree(parents);

        if (olddir->mount != newdir->mount) {
            vfs_node_put(olddir);
            vfs_node_put(newdir);
            vfs_ns_unlock();
            return -EXDEV;
        }

        if (!olddir->i_ops || !vfs_is_valid_kptr(olddir->i_ops) || !olddir->i_ops->rename) {
            vfs_node_put(olddir);
            vfs_node_put(newdir);
            vfs_ns_unlock();
            return -EROFS;
        }
        if (!vfs_is_valid_kptr(olddir->i_ops->rename)) {
            vfs_node_put(olddir);
            vfs_node_put(newdir);
            vfs_ns_unlock();
            return -1;
        }

        int ret = olddir->i_ops->rename(olddir, oldname, newdir, newname);
        vfs_node_put(olddir);
        vfs_node_put(newdir);
        vfs_ns_unlock();
        return ret;
    }

    int vfs_truncate(vfs_node_t *node, uint64_t new_size)
    {
        if (!node) return -EINVAL;
        if (node->type != VFS_FILE) return -EISDIR;
        if (!node->i_ops || !vfs_is_valid_kptr(node->i_ops) || !node->i_ops->truncate)
            return -EROFS;
        if (!vfs_is_valid_kptr(node->i_ops->truncate))
            return -1;

        return node->i_ops->truncate(node, new_size);
    }
    ```

- [ ] **Step 4: Call `vfs_end_bootstrap()` in `kernel/sched/core.c` and migrate `kernel/syscall/sys_fs.c`, `kernel/fs/file.c`, `kernel/memory/vma.c`, and `kernel/selftest/symlink_selftest.c`**

1. In `kernel/sched/core.c:586-589`, right after `this_cpu()->scheduler_ok = 1;`, call `vfs_end_bootstrap();`:
   ```c
   current->state = TASK_RUNNING;
   this_cpu()->scheduler_ok = 1;
   vfs_end_bootstrap();
   ```
2. In `kernel/syscall/sys_fs.c`:
   - In `sys_symlink()` (lines 45-96): replace the manual `vfs_split_parent` + `vfs_lookup_at` + `parent->ops->symlink` with `vfs_symlink()`:
     ```c
     static __attribute__((noinline)) int64_t sys_symlink(const char *target, const char *linkpath)
     {
         char *paths = kmalloc(2 * VFS_NAME_MAX);
         if (!paths) return -ENOMEM;
         char *target_copy = paths;
         char *linkpath_copy = paths + VFS_NAME_MAX;

         int64_t ret = 0;
         long tlen = copy_user_path(target_copy, target, VFS_NAME_MAX);
         if (tlen < 0) { ret = tlen; goto out; }
         if (tlen == 0) { ret = -ENOENT; goto out; }

         long llen = copy_user_path(linkpath_copy, linkpath, VFS_NAME_MAX);
         if (llen < 0) { ret = llen; goto out; }

         const char *cwd = current->files ? current->files->cwd : "/";
         ret = vfs_symlink(target_copy, linkpath_copy, cwd);

     out:
         kfree(paths);
         return ret;
     }
     ```
   - In `sys_readlink()` (lines 129-135): replace `node->ops` with `node->i_ops`:
     ```c
     if (!node->i_ops || !node->i_ops->readlink) {
         vfs_node_put(node);
         ret = -EOPNOTSUPP;
         goto out;
     }

     int tlen = node->i_ops->readlink(node, kbuf, VFS_NAME_MAX - 1);
     ```
   - In `SYS_open` (lines 330-395):
     - Replace the manual `O_CREAT` parent lookup and `parent->ops->create` block (lines 330-379) with:
       ```c
       if (lookup_rc == -ENOENT && (flags & O_CREAT)) {
           const char *cwd = current->files ? current->files->cwd : "/";
           int create_rc = vfs_create_file(path_copy, cwd, &node);
           if (create_rc < 0) {
               syscall_result = create_rc;
               goto out_open;
           }
       }
       ```
       (Remove unused `char *parent_path = NULL;` and `vfs_node_t *parent = NULL;` from `SYS_open`.)
     - Replace the `O_TRUNC` block (lines 386-395) with:
       ```c
       if ((flags & O_TRUNC) && node->type == VFS_FILE) {
           if (node->i_ops && (uint64_t)node->i_ops >= 0xffff800000000000ULL &&
               node->i_ops->truncate && (uint64_t)node->i_ops->truncate >= 0xffff800000000000ULL)
               node->i_ops->truncate(node, 0);
           else
               node->size = 0;
       }
       ```
   - In `SYS_access` (lines 864-869): replace `node->ops` with `node->f_ops`:
     ```c
     if (node->type == VFS_CHRDEV || node->type == VFS_BLKDEV) {
         if (!node->f_ops || (uint64_t)node->f_ops < 0xffff800000000000ULL ||
             !node->f_ops->write)
             ok = 0;
     }
     ```
3. In `kernel/fs/file.c`:
   - Lines 664-684 (`vfs_file_open`, `vfs_file_read`, `vfs_file_release`): replace `f->node->ops` with `f->node->f_ops`.
   - Lines 714-725 (`fd_read`): replace `f->node->ops` with `f->node->f_ops`.
   - Lines 1029-1035 (`fd_write`): replace `f->node->ops` with `f->node->f_ops`.
4. In `kernel/memory/vma.c`:
   - Lines 704-709 (`do_mmap`): replace `file_node->ops` with `file_node->f_ops`:
     ```c
     int (*_dev_mmap)(struct vfs_node *, struct vma *) =
         (file_node && file_node->f_ops &&
          (uint64_t)file_node->f_ops >= 0xffff800000000000ULL &&
          file_node->f_ops->mmap &&
          (uint64_t)file_node->f_ops->mmap >= 0xffff800000000000ULL)
         ? file_node->f_ops->mmap : NULL;
     ```
5. In `kernel/selftest/symlink_selftest.c`:
   - Replace `fake_ops` and `fake_ops_no_readlink` (lines 154-179) with:
     ```c
     static const vfs_inode_ops_t fake_iops = {
         .readlink = fake_readlink,
     };
     static const vfs_file_ops_t fake_fops = {
         .readdir  = fake_readdir,
     };
     static const vfs_inode_ops_t fake_iops_no_readlink = {
         .readlink = NULL,
     };

     #define FAKE_MNT      "/symlink-selftest"
     #define FAKE_MNT_NORL "/symlink-selftest-nolink"

     static int fake_mounted = 0;

     static int fake_mount_once(void)
     {
         if (fake_mounted) return 0;
         if (vfs_mount(FAKE_MNT, NULL, &fake_iops, &fake_fops, NULL) != 0) return -1;
         if (vfs_mount(FAKE_MNT_NORL, NULL, &fake_iops_no_readlink, &fake_fops, NULL) != 0) return -1;
         fake_mounted = 1;
         return 0;
     }
     ```
   - In `slt_ext2_root()`, `slt_fault_case()`, `slt_success_case()`, and `symlink_selftest_ext2_rollback()` (lines 293-415): replace `root->ops != &ext2_vfs_ops` with `root->i_ops != &ext2_iops`, and replace `root->ops->symlink` / `root->ops->unlink` with `root->i_ops->symlink` / `root->i_ops->unlink`.

- [ ] **Step 5: Split ops tables in `devfs.c`, `procfs.c`, `tmpfs.c`, `ext2.c`/`ext2.h`, `fat.c`/`fat.h` and update existing host tests**

1. **`kernel/fs/devfs.c`**:
   - Replace `static struct vfs_ops devfs_ops` (lines 452-459) with:
     ```c
     static const vfs_inode_ops_t devfs_iops;
     static const vfs_file_ops_t devfs_fops = {
         .read      = devfs_read,
         .write     = devfs_write,
         .readdir   = devfs_readdir,
         .mmap      = devfs_mmap,
         .read_file = devfs_read_file,
     };
     ```
   - In `tty_magic_open()` (line 612): replace `node->ops = &devfs_ops;` with `node->i_ops = &devfs_iops; node->f_ops = &devfs_fops; node->nlink = 1;`.
   - In `devfs_fs_mount()` (lines 642-650):
     ```c
     static int devfs_fs_mount(struct vfs_mount *mp, block_device_t *dev, void *data)
     {
         (void)dev;
         (void)data;
         mp->i_ops = &devfs_iops;
         mp->f_ops = &devfs_fops;
         mp->fs_data = NULL;
         mp->root->i_ops = &devfs_iops;
         mp->root->f_ops = &devfs_fops;
         mp->root->ino = VFS_SYNTH_ROOT_INO;
         mp->root->mode = S_IFDIR | 0755;
         mp->root->nlink = 2;
         return 0;
     }
     ```
2. **`kernel/fs/procfs.c`**:
   - Replace `static vfs_ops_t procfs_ops` and `procfs_fs_mount` (lines 515-533) with:
     ```c
     static const vfs_inode_ops_t procfs_iops;
     static const vfs_file_ops_t procfs_fops = {
         .read         = procfs_read,
         .write        = NULL,
         .readdir      = procfs_readdir,
         .open_file    = procfs_open_file,
         .read_file    = procfs_read_file,
         .release_file = procfs_release_file,
     };

     static int procfs_fs_mount(struct vfs_mount *mp, block_device_t *dev, void *data)
     {
         (void)dev;
         (void)data;
         mp->i_ops = &procfs_iops;
         mp->f_ops = &procfs_fops;
         mp->fs_data = NULL;
         mp->root->i_ops = &procfs_iops;
         mp->root->f_ops = &procfs_fops;
         mp->root->ino = VFS_SYNTH_ROOT_INO;
         mp->root->mode = S_IFDIR | 0555;
         mp->root->nlink = 2;
         return 0;
     }
     ```
3. **`kernel/fs/tmpfs.c`**:
   - Replace `static struct vfs_ops tmpfs_vfs_ops;` at line 25 with:
     ```c
     static const vfs_inode_ops_t tmpfs_iops;
     static const vfs_file_ops_t tmpfs_fops;
     ```
   - In `tmpfs_vfs_create` (line 282) and `tmpfs_vfs_mkdir` (line 329): replace `vn->ops = &tmpfs_vfs_ops;` with `vn->i_ops = &tmpfs_iops; vn->f_ops = &tmpfs_fops; vn->ino = (uint64_t)(uintptr_t)new_node; vn->nlink = (vn->type == VFS_DIR) ? 2 : 1;`.
   - Replace `tmpfs_vfs_ops` and `tmpfs_fs_mount` (lines 414-455) with:
     ```c
     static const vfs_inode_ops_t tmpfs_iops = {
         .create   = tmpfs_vfs_create,
         .unlink   = tmpfs_vfs_unlink,
         .mkdir    = tmpfs_vfs_mkdir,
         .rmdir    = tmpfs_vfs_rmdir,
         .rename   = tmpfs_vfs_rename,
         .truncate = tmpfs_vfs_truncate,
     };

     static const vfs_file_ops_t tmpfs_fops = {
         .read     = tmpfs_vfs_read,
         .write    = tmpfs_vfs_write,
         .readdir  = tmpfs_vfs_readdir,
     };

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
     ```
4. **`kernel/include/fs/ext2.h` & `kernel/fs/ext2.c`**:
   - In `kernel/include/fs/ext2.h:128`: replace `extern struct vfs_ops ext2_vfs_ops;` with:
     ```c
     extern const vfs_inode_ops_t ext2_iops;
     extern const vfs_file_ops_t  ext2_fops;
     ```
   - In `kernel/fs/ext2.c`:
     - In `ext2_vfs_create` (line 837) and `ext2_vfs_mkdir` (line 1149): replace `node->ops = dir->ops;` with `node->i_ops = dir->i_ops; node->f_ops = dir->f_ops; node->ino = new_ino; node->nlink = (node->type == VFS_DIR) ? 2 : 1;`.
     - Replace `struct vfs_ops ext2_vfs_ops` (lines 1537-1550) with:
       ```c
       const vfs_inode_ops_t ext2_iops = {
           .readlink = ext2_vfs_readlink,
           .symlink  = ext2_vfs_symlink,
           .create   = ext2_vfs_create,
           .unlink   = ext2_vfs_unlink,
           .mkdir    = ext2_vfs_mkdir,
           .rmdir    = ext2_vfs_rmdir,
           .rename   = ext2_vfs_rename,
           .truncate = ext2_vfs_truncate,
       };

       const vfs_file_ops_t ext2_fops = {
           .read     = ext2_vfs_read,
           .write    = ext2_vfs_write,
           .readdir  = ext2_vfs_readdir,
       };
       ```
     - In `ext2_fs_mount` (lines 1632-1634): set `mp->i_ops = &ext2_iops; mp->f_ops = &ext2_fops; mp->fs_data = fs; mp->root->i_ops = &ext2_iops; mp->root->f_ops = &ext2_fops; mp->root->ino = EXT2_ROOT_INO; mp->root->nlink = 2;`.
     - In `ext2_selftest_sparse_read` (lines 2071, 2088, 2104): replace `fake.ops = &ext2_vfs_ops` and `ext2_vfs_ops.read` with `fake.i_ops = &ext2_iops; fake.f_ops = &ext2_fops;` and `ext2_fops.read`.
5. **`kernel/include/fs/fat.h` & `kernel/fs/fat.c`**:
   - In `kernel/include/fs/fat.h:118`: replace `extern struct vfs_ops fat_vfs_ops;` with:
     ```c
     extern const vfs_inode_ops_t fat_iops;
     extern const vfs_file_ops_t  fat_fops;
     ```
   - In `kernel/fs/fat.c`:
     - In `fat_mkdir` (line 954) and `fat_create` (line 1248): replace `node->ops = dir->ops;` with `node->i_ops = dir->i_ops; node->f_ops = dir->f_ops; node->nlink = (node->type == VFS_DIR) ? 2 : 1;`.
     - Replace `struct vfs_ops fat_vfs_ops` (lines 1256-1267) with:
       ```c
       const vfs_inode_ops_t fat_iops = {
           .create   = fat_create,
           .unlink   = fat_unlink,
           .mkdir    = fat_mkdir,
           .rmdir    = fat_rmdir,
           .rename   = fat_rename,
           .truncate = fat_truncate,
       };

       const vfs_file_ops_t fat_fops = {
           .read     = fat_read,
           .write    = fat_write,
           .readdir  = fat_readdir,
       };
       ```
     - In `fat_fs_mount` (lines 1357-1359): set `mp->i_ops = &fat_iops; mp->f_ops = &fat_fops; mp->fs_data = fs; mp->root->i_ops = &fat_iops; mp->root->f_ops = &fat_fops; mp->root->ino = fs->root_cluster; mp->root->mode = S_IFDIR | 0755; mp->root->nlink = 2;`.
6. **Update existing hosttest files**:
   - `hosttests/mock/gfx_test_runtime.h:119-120`:
     ```c
     int  vfs_mount(const char *path, block_device_t *dev,
                    const vfs_inode_ops_t *i_ops, const vfs_file_ops_t *f_ops,
                    void *fs_data);
     ```
   - `hosttests/cases/test_vfs_basic.c`: replace local `vfs_ops_t` with `vfs_inode_ops_t` + `vfs_file_ops_t` (`fake_iops`, `fake_fops`).
   - `hosttests/cases/test_vfs_fs_registry.c`: replace `static vfs_ops_t s_dummy_ops = {0};` with `static const vfs_inode_ops_t s_dummy_iops = {0}; static const vfs_file_ops_t s_dummy_fops = {0};` and set `mp->i_ops = &s_dummy_iops; mp->f_ops = &s_dummy_fops; mp->root->i_ops = &s_dummy_iops; mp->root->f_ops = &s_dummy_fops;`.
   - `hosttests/cases/test_gfx_file_lifecycle.c`: update `vfs_mount` stub signature (line 85), `vfs_read` stub (`node->f_ops->read`, line 92), and `test_read_preserves_cow_allocation_error` (`vfs_file_ops_t fops = {.read = fake_read}; vfs_node_t node = {.f_ops = &fops};`, lines 534-535).
   - `hosttests/cases/test_pty_winsize.c`: update `vfs_mount` stub signature (line 153) and `vfs_read` stub (`node->f_ops->read`, line 156).
   - `hosttests/cases/test_monitor_file.c`: update `vfs_mount` stub signature (line 79), `vfs_read` stub (`node->f_ops->read`, line 86), and `snap_ops` (`static const vfs_file_ops_t snap_fops = { .open_file = snap_open, .read_file = snap_read, .release_file = snap_release };`, `n->f_ops = &snap_fops;`, lines 209-222).
   - `hosttests/cases/test_poll_requested.c`: update `make_vfs_file_with_ops` and `test_fd_read_vfs_path_same_contract` (lines 452-586) from `vfs_ops_t` / `node.ops` to `vfs_file_ops_t` / `node.f_ops`.
   - `hosttests/cases/test_gfx_device.c`: update `vfs_mount` stub signature (line 110).
   - `hosttests/cases/test_devfs_input_dispatch.c`: replace `static vfs_ops_t *devfs_mount_ops;` with `static const vfs_file_ops_t *devfs_mount_fops;`, capture `devfs_mount_fops = dummy_mp.f_ops;` in `vfs_mount_fs` / `vfs_mount` stubs (lines 93-123), and set `n.f_ops = devfs_mount_fops;` in `mock_node` (line 316).
   - `hosttests/cases/test_monitor_procfs.c`: replace `static vfs_ops_t procfs_hook_ops` with `static const vfs_file_ops_t procfs_hook_fops` and set `n->f_ops = &procfs_hook_fops;` (lines 193-205).
   - `hosttests/cases/test_device_boot.c`: replace `struct vfs_ops ext2_vfs_ops` / `fat_vfs_ops` and `vfs_mount` stub (lines 109-146) with `const vfs_inode_ops_t ext2_iops = {0}; const vfs_file_ops_t ext2_fops = {0}; const vfs_inode_ops_t fat_iops = {0}; const vfs_file_ops_t fat_fops = {0};` and updated `vfs_mount` signature.

- [ ] **Step 6: Verify zero remaining `vfs_ops` references and run `make test-host`**

Run:
```bash
! grep -rn "vfs_ops" kernel/ hosttests/
make test-host
```
Expected: `grep` returns 0 matches (exit 0 with `!`), and `make test-host` builds and passes all host test binaries.

- [ ] **Step 7: Commit Task 1**

```bash
git add kernel/include/fs/vfs.h hosttests/include/fs/vfs.h \
        kernel/fs/vfs.c kernel/sched/core.c kernel/syscall/sys_fs.c \
        kernel/fs/file.c kernel/memory/vma.c kernel/selftest/symlink_selftest.c \
        kernel/fs/devfs.c kernel/fs/procfs.c kernel/fs/tmpfs.c \
        kernel/include/fs/ext2.h kernel/fs/ext2.c \
        kernel/include/fs/fat.h kernel/fs/fat.c \
        hosttests/mock/vfs_test_runtime.h hosttests/mock/gfx_test_runtime.h \
        hosttests/cases/test_vfs_basic.c hosttests/cases/test_vfs_fs_registry.c \
        hosttests/cases/test_gfx_file_lifecycle.c hosttests/cases/test_pty_winsize.c \
        hosttests/cases/test_monitor_file.c hosttests/cases/test_poll_requested.c \
        hosttests/cases/test_gfx_device.c hosttests/cases/test_devfs_input_dispatch.c \
        hosttests/cases/test_monitor_procfs.c hosttests/cases/test_device_boot.c
git commit -m "refactor(vfs): split vfs_ops_t into vfs_inode_ops_t and vfs_file_ops_t (ARCH-5 Phase 2A)"
```

---

### Task 2: Implement `i_ops->lookup`, `i_ops->getattr`, and Rich Node Metadata Across All 5 Backends + `test_vfs_iops_fops.c`

**Files:**
- Modify: `kernel/fs/devfs.c:420-465`
- Modify: `kernel/fs/procfs.c:505-540`
- Modify: `kernel/fs/tmpfs.c:35-65,180-345,405-455`
- Modify: `kernel/fs/ext2.c:565-845,1055-1160,1535-1640`
- Modify: `kernel/fs/fat.c:755-965,1160-1270,1345-1365`
- Create: `hosttests/cases/test_vfs_iops_fops.c`
- Modify: `hosttests/Makefile:70-75,580-595`

- [ ] **Step 1: Create `hosttests/cases/test_vfs_iops_fops.c` and register in `hosttests/Makefile` (RED)**

Create `hosttests/cases/test_vfs_iops_fops.c` to test:
1. **300-entry directory lookup without truncation (`test_lookup_300_entries_tmpfs_and_ext2`)**:
   - Mount `tmpfs` at `/tmp`, create 300 files (`file_000` .. `file_299`), look up `/tmp/file_299`, verify `rc == 0`, `node != NULL`, `vfs_node_stable_ino(node) != 0`.
2. **Custom `i_ops->getattr` vs `vfs_generic_getattr` fallback (`test_getattr_custom_and_generic_fallback`)**:
   - Verify `devfs` nodes (`/dev/null`) populate `st_mode == (S_IFCHR | 0666)`, `st_rdev == idx + 1`, `st_ino == idx + 1`, `st_nlink == 1`.
   - Verify `tmpfs` and `ext2` nodes populate `st_ino`, `st_mode`, `st_uid`, `st_gid`, `st_nlink` (including preserving `node->nlink == 0` in `vfs_generic_getattr` when `nlink == 0`), and custom `getattr` overrides `st_blksize`.
3. **Write & truncate metadata (`st_size`) consistency across handles (`test_write_and_truncate_stat_consistency`)**:
   - Open two independent `vfs_node_t` handles `n1 = vfs_lookup("/tmp/testfile")` and `n2 = vfs_lookup("/tmp/testfile")`.
   - Write 128 bytes through `n1`; call `vfs_stat(n1, &st1)`, `vfs_stat(n2, &st2)`, and `vfs_stat(n3, &st3)` (where `n3` is freshly looked up). All three must report `st_size == 128`.
   - Call `vfs_truncate(n1, 32)`; call `vfs_stat` on `n1`, `n2`, and freshly looked-up `n4`. All three must report `st_size == 32`.
4. **Read-only filesystem error codes (`-EROFS` vs `-EOPNOTSUPP`) and cross-mount `rename` (`-EXDEV`) (`test_erofs_eopnotsupp_and_exdev`)**:
   - On `procfs` (`/proc`), `vfs_unlink("/proc/meminfo", "/")`, `vfs_mkdir("/proc/sub", "/")`, `vfs_rmdir("/proc/sub", "/")`, and `vfs_create_file("/proc/new", "/", &out)` all return `-EROFS`, while `vfs_symlink("target", "/proc/lnk", "/")` returns `-EOPNOTSUPP`.
   - `vfs_rename("/tmp/a", "/dev/a", "/")` across two different mounts returns `-EXDEV`.

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

task_t *vfs_test_current = NULL;
int g_log_level = 0;
void _log_write(int level, const char *fmt, ...) { (void)level; (void)fmt; }
void _log_err_impl(const char *fmt, ...) { (void)fmt; }
void _log_warn_impl(const char *fmt, ...) { (void)fmt; }

int block_device_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{ (void)dev; (void)lba; (void)count; (void)buf; return -EIO; }
int block_device_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{ (void)dev; (void)lba; (void)count; (void)buf; return -EIO; }

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

TEST_FUNC(test_lookup_300_entries_tmpfs) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));

    for (int i = 0; i < 300; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/file_%03d", i);
        vfs_node_t *created = NULL;
        assert_eq(0, vfs_create_file(path, "/", &created));
        assert_not_null(created);
        vfs_node_put(created);
    }

    /* Lookup the 300th entry (index 299, well past the old 256 readdir cap) */
    vfs_node_t *n299 = NULL;
    assert_eq(0, vfs_lookup_at(AT_FDCWD, "/file_299", LOOKUP_FOLLOW, &n299));
    assert_not_null(n299);
    assert_str_eq("file_299", n299->name);
    assert_true(vfs_node_stable_ino(n299) != 0);
    vfs_node_put(n299);
}

TEST_FUNC(test_devfs_and_tmpfs_metadata_and_getattr) {
    vfs_init();
    devfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));

    /* Check /dev/null metadata populated by devfs_iop_lookup */
    vfs_node_t *nul = vfs_lookup("/dev/null");
    assert_not_null(nul);
    assert_not_null(nul->i_ops);
    assert_not_null(nul->i_ops->lookup);

    struct stat st;
    assert_eq(0, vfs_stat(nul, &st));
    assert_eq((uint32_t)(S_IFCHR | 0666), (uint32_t)st.st_mode);
    assert_true(st.st_rdev > 0);
    assert_eq(st.st_ino, st.st_rdev);
    assert_eq(1, st.st_nlink);
    vfs_node_put(nul);

    /* Verify vfs_generic_getattr preserves nlink == 0 for orphaned nodes */
    vfs_node_t orphan = {0};
    orphan.type = VFS_FILE;
    orphan.ino = 999;
    orphan.size = 4096;
    orphan.mode = S_IFREG | 0644;
    orphan.nlink = 0;
    orphan.node_flags = VFS_NODE_ORPHANED;
    assert_eq(0, vfs_generic_getattr(&orphan, &st));
    assert_eq(0, st.st_nlink);
    assert_eq(999, st.st_ino);
    assert_eq((uint32_t)(S_IFREG | 0644), (uint32_t)st.st_mode);
}

TEST_FUNC(test_write_and_truncate_stat_consistency) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));

    vfs_node_t *created = NULL;
    assert_eq(0, vfs_create_file("/consistency.bin", "/", &created));
    assert_not_null(created);
    vfs_node_put(created);

    vfs_node_t *h1 = vfs_lookup("/consistency.bin");
    vfs_node_t *h2 = vfs_lookup("/consistency.bin");
    assert_not_null(h1);
    assert_not_null(h2);

    char payload[128];
    memset(payload, 'A', sizeof(payload));
    assert_eq(128, vfs_write(h1, 0, sizeof(payload), payload));

    struct stat st1, st2, st3;
    assert_eq(0, vfs_stat(h1, &st1));
    assert_eq(0, vfs_stat(h2, &st2));
    vfs_node_t *h3 = vfs_lookup("/consistency.bin");
    assert_not_null(h3);
    assert_eq(0, vfs_stat(h3, &st3));

    assert_eq(128, st1.st_size);
    assert_eq(128, st2.st_size);
    assert_eq(128, st3.st_size);
    vfs_node_put(h3);

    /* Truncate via h1 and verify h2 + fresh lookup see new size 32 */
    assert_eq(0, vfs_truncate(h1, 32));
    assert_eq(0, vfs_stat(h1, &st1));
    assert_eq(0, vfs_stat(h2, &st2));
    vfs_node_t *h4 = vfs_lookup("/consistency.bin");
    assert_not_null(h4);
    assert_eq(0, vfs_stat(h4, &st3));

    assert_eq(32, st1.st_size);
    assert_eq(32, st2.st_size);
    assert_eq(32, st3.st_size);

    vfs_node_put(h4);
    vfs_node_put(h2);
    vfs_node_put(h1);
}

TEST_FUNC(test_erofs_eopnotsupp_and_exdev) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));
    assert_eq(0, vfs_mount_fs("procfs", "/proc", NULL, NULL));

    vfs_node_t *out = NULL;
    assert_eq(-EROFS, vfs_create_file("/proc/file", "/", &out));
    assert_null(out);
    assert_eq(-EROFS, vfs_mkdir("/proc/dir", "/"));
    assert_eq(-EROFS, vfs_unlink("/proc/meminfo", "/"));
    assert_eq(-EROFS, vfs_rmdir("/proc/dir", "/"));
    assert_eq(-EOPNOTSUPP, vfs_symlink("/target", "/proc/link", "/"));

    vfs_node_t *f = NULL;
    assert_eq(0, vfs_create_file("/a.txt", "/", &f));
    vfs_node_put(f);
    assert_eq(-EXDEV, vfs_rename("/a.txt", "/proc/a.txt", "/"));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_lookup_300_entries_tmpfs),
    TEST_ENTRY(test_devfs_and_tmpfs_metadata_and_getattr),
    TEST_ENTRY(test_write_and_truncate_stat_consistency),
    TEST_ENTRY(test_erofs_eopnotsupp_and_exdev),
TEST_LIST_END

int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
```

In `hosttests/Makefile`:
- Add `$(TEST_BLD)/test_vfs_iops_fops.elf \` to `TEST_BINS` right after `test_vfs_fs_registry.elf`.
- Add build rules right after `test_vfs_fs_registry.elf`:
```makefile
$(TEST_BLD)/test_vfs_iops_fops.o: $(TEST_CASES)/test_vfs_iops_fops.c \
		$(TESTS_DIR)/kernel/include/fs/vfs.h \
		$(TEST_MOCK)/vfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VFS_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_vfs_iops_fops.elf: $(TEST_BLD)/test_vfs_iops_fops.o \
		$(TEST_BLD)/vfs_production.o \
		$(TEST_BLD)/ext2_production.o \
		$(TEST_BLD)/fat_production.o \
		$(TEST_BLD)/tmpfs_production.o \
		$(TEST_BLD)/gfx_devfs_production.o \
		$(TEST_BLD)/rwlock.o \
		$(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -Wl,--gc-sections -o $@ $^
```

- [ ] **Step 2: Implement `devfs_iop_lookup` in `kernel/fs/devfs.c`**

In `kernel/fs/devfs.c` (right after `devfs_readdir`, around line 447):
```c
static int devfs_iop_lookup(vfs_node_t *dir, const char *name, vfs_node_t **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    if (!dir || !name) return -EINVAL;
    if (dir->type != VFS_DIR) return -ENOTDIR;

    for (int i = 0; i < DEVFS_MAX_DEVICES; i++) {
        if (!devices[i].registered) continue;
        if (strcmp(devices[i].name, name) == 0) {
            vfs_node_t *child = (vfs_node_t *)calloc(1, sizeof(vfs_node_t));
            if (!child) return -ENOMEM;
            child->name = strndup(devices[i].name, VFS_NAME_MAX - 1);
            if (!child->name) {
                kfree(child);
                return -ENOMEM;
            }
            child->type = devices[i].type;
            child->ino = (uint64_t)(i + 1);
            child->rdev = (uint64_t)(i + 1);
            child->mode = (devices[i].type == VFS_CHRDEV ? S_IFCHR : S_IFBLK) | 0666;
            child->nlink = 1;
            child->size = 0;
            child->mount = dir->mount;
            child->parent = vfs_node_get(dir);
            child->fs_data = (void *)(uintptr_t)i;
            child->i_ops = &devfs_iops;
            child->f_ops = &devfs_fops;
            child->refcount = 1;
            *out = child;
            return 0;
        }
    }
    return -ENOENT;
}

static const vfs_inode_ops_t devfs_iops = {
    .lookup = devfs_iop_lookup,
};
```

- [ ] **Step 3: Implement `procfs_iop_lookup` in `kernel/fs/procfs.c`**

In `kernel/fs/procfs.c` (before `procfs_iops`, around line 510), implement `procfs_iop_lookup` (reusing `find_task_by_pid`, `parse_fd`, `task_files_pin_by_pid`, and `task_list_lock` already in `procfs.c`):
```c
static int procfs_parse_uint(const char *s, uint32_t *out_val)
{
    if (!s || s[0] == '\0') return -1;
    if (s[0] == '0' && s[1] != '\0') return -1; // reject leading zeros like "01"
    uint64_t val = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        val = val * 10 + (uint64_t)(*p - '0');
        if (val > 0x7FFFFFULL) return -1;
    }
    *out_val = (uint32_t)val;
    return 0;
}

static int procfs_iop_lookup(vfs_node_t *dir, const char *name, vfs_node_t **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    if (!dir || !name) return -EINVAL;
    if (dir->type != VFS_DIR) return -ENOTDIR;

    int      dir_type = PROCFS_TYPE(dir->fs_data);
    uint32_t pid      = PROCFS_PID(dir->fs_data);

    uint8_t  child_type = 0;
    uint64_t child_size = 0;
    void    *child_fs_data = NULL;

    switch (dir_type) {
    case PROCFS_TYPE_ROOT: {
        if (strcmp(name, "self") == 0) {
            child_type = VFS_DIR;
            child_size = 0;
            child_fs_data = PROCFS_ENCODE(PROCFS_TYPE_SELF_DIR, PROCFS_PID_SELF);
            break;
        }
        if (strcmp(name, "meminfo") == 0) {
            child_type = VFS_FILE;
            child_size = 4096;
            child_fs_data = PROCFS_ENCODE(PROCFS_TYPE_MEMINFO, 0);
            break;
        }
        if (strcmp(name, "stat") == 0) {
            child_type = VFS_FILE;
            child_size = 4096;
            child_fs_data = PROCFS_ENCODE(PROCFS_TYPE_STAT, 0);
            break;
        }
        if (strcmp(name, "thermal") == 0) {
            child_type = VFS_FILE;
            child_size = 4096;
            child_fs_data = PROCFS_ENCODE(PROCFS_TYPE_THERMAL, 0);
            break;
        }
        uint32_t target_pid = 0;
        if (procfs_parse_uint(name, &target_pid) != 0 || target_pid == 0)
            return -ENOENT;
        uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
        task_t *t = find_task_by_pid((int)target_pid);
        int valid = (t && t->pid > 0 && !(t->flags & PF_KTHREAD));
        spin_unlock_irqrestore(&task_list_lock, tl_flags);
        if (!valid) return -ENOENT;
        child_type = VFS_DIR;
        child_size = 0;
        child_fs_data = PROCFS_ENCODE(PROCFS_TYPE_PID_DIR, target_pid);
        break;
    }
    case PROCFS_TYPE_SELF_DIR:
    case PROCFS_TYPE_PID_DIR: {
        if (dir_type == PROCFS_TYPE_PID_DIR) {
            uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
            task_t *t = find_task_by_pid((int)pid);
            int valid = (t && t->pid > 0 && !(t->flags & PF_KTHREAD));
            spin_unlock_irqrestore(&task_list_lock, tl_flags);
            if (!valid) return -ENOENT;
        }
        uint32_t enc_pid = (dir_type == PROCFS_TYPE_SELF_DIR) ? PROCFS_PID_SELF : pid;
        if (strcmp(name, "status") == 0) {
            child_type = VFS_FILE;
            child_size = 4096;
            child_fs_data = PROCFS_ENCODE(PROCFS_TYPE_STATUS, enc_pid);
        } else if (strcmp(name, "maps") == 0) {
            child_type = VFS_FILE;
            child_size = 4096;
            child_fs_data = PROCFS_ENCODE(PROCFS_TYPE_MAPS, enc_pid);
        } else if (strcmp(name, "fd") == 0) {
            child_type = VFS_DIR;
            child_size = 0;
            child_fs_data = PROCFS_ENCODE(PROCFS_TYPE_FD_DIR, enc_pid);
        } else {
            return -ENOENT;
        }
        break;
    }
    case PROCFS_TYPE_FD_DIR: {
        int fd_num = parse_fd(name);
        if (fd_num < 0) return -ENOENT;
        uint32_t p = pid;
        if (p == PROCFS_PID_SELF) {
            if (!current) return -ENOENT;
            p = (uint32_t)current->pid;
        }
        files_t *fs = task_files_pin_by_pid((int)p);
        if (!fs) return -ENOENT;
        uint64_t fl = spin_lock_irqsave(&fs->lock);
        int open_ok = (fs->fd[fd_num] != NULL);
        spin_unlock_irqrestore(&fs->lock, fl);
        files_unpin(fs);
        if (!open_ok) return -ENOENT;
        child_type = VFS_FILE;
        child_size = 4096;
        child_fs_data = PROCFS_ENCODE(PROCFS_TYPE_FD_ENTRY, pid);
        break;
    }
    default:
        return -ENOENT;
    }

    vfs_node_t *child = (vfs_node_t *)calloc(1, sizeof(vfs_node_t));
    if (!child) return -ENOMEM;
    child->name = strndup(name, VFS_NAME_MAX - 1);
    if (!child->name) {
        kfree(child);
        return -ENOMEM;
    }
    child->type = child_type;
    child->size = child_size;
    child->ino = (uint64_t)(uintptr_t)child_fs_data;
    child->mode = (child_type == VFS_DIR) ? (S_IFDIR | 0555) : (S_IFREG | 0444);
    child->nlink = (child_type == VFS_DIR) ? 2 : 1;
    child->mount = dir->mount;
    child->parent = vfs_node_get(dir);
    child->fs_data = child_fs_data;
    child->i_ops = &procfs_iops;
    child->f_ops = &procfs_fops;
    child->refcount = 1;
    *out = child;
    return 0;
}

static const vfs_inode_ops_t procfs_iops = {
    .lookup = procfs_iop_lookup,
};
```

- [ ] **Step 4: Implement `tmpfs_iop_lookup`, `tmpfs_iop_getattr`, and `tmpfs_node_t` metadata in `kernel/fs/tmpfs.c`**

In `kernel/fs/tmpfs.c`:
1. Add metadata fields to `tmpfs_node_t` (around line 36):
   ```c
   typedef struct tmpfs_node {
       char               name[256];
       uint8_t            type;
       uint64_t           size;
       uint32_t           mode;
       uint32_t           uid;
       uint32_t           gid;
       uint32_t           nlink;
       uint64_t           atime;
       uint64_t           mtime;
       uint64_t           ctime;
       struct tmpfs_node *parent;
       ...
   ```
2. In `tmpfs_vfs_write()` (line 192): after `if (end_pos > tn->size) tn->size = end_pos;`, also set `node->size = tn->size;`.
3. In `tmpfs_vfs_truncate()` (line 410): after `tn->size = new_size;`, also set `node->size = new_size;`.
4. In `tmpfs_vfs_create()` and `tmpfs_vfs_mkdir()`: initialize `new_node->mode = (new_node->type == VFS_DIR) ? (S_IFDIR | 0755) : (S_IFREG | 0755); new_node->nlink = (new_node->type == VFS_DIR) ? 2 : 1;`, and copy `vn->ino = (uint64_t)(uintptr_t)new_node; vn->mode = new_node->mode; vn->nlink = new_node->nlink;`.
5. Implement `tmpfs_iop_lookup` and `tmpfs_iop_getattr`:
   ```c
   static int tmpfs_iop_lookup(struct vfs_node *dir, const char *name,
                               struct vfs_node **out)
   {
       if (!out) return -EINVAL;
       *out = NULL;
       if (!dir || !name) return -EINVAL;
       tmpfs_node_t *d = tmpfs_get_node(dir);
       if (!d || d->type != VFS_DIR) return -ENOTDIR;

       int idx = tmpfs_find_child(d, name);
       if (idx < 0) return -ENOENT;

       tmpfs_node_t *tn = d->children[idx];
       struct vfs_node *vn = calloc(1, sizeof(struct vfs_node));
       if (!vn) return -ENOMEM;

       vn->name = strndup(tn->name, VFS_NAME_MAX - 1);
       if (!vn->name) {
           kfree(vn);
           return -ENOMEM;
       }
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
       *out = vn;
       return 0;
   }

   static int tmpfs_iop_getattr(struct vfs_node *node, struct stat *buf)
   {
       if (!node || !buf) return -1;
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
       return vfs_generic_getattr(node, buf);
   }
   ```
   Wire `.lookup = tmpfs_iop_lookup` and `.getattr = tmpfs_iop_getattr` into `tmpfs_iops`, and initialize `root->mode = S_IFDIR | 0755; root->nlink = 2;` in `tmpfs_fs_mount()`.

- [ ] **Step 5: Implement `ext2_iop_lookup`, `ext2_iop_getattr`, and inode metadata population in `kernel/fs/ext2.c`**

In `kernel/fs/ext2.c`:
1. Add helper `ext2_fill_vfs_node(vfs_node_t *node, uint32_t ino, const ext2_inode_t *disk_ino)`:
   ```c
   static void ext2_fill_vfs_node(vfs_node_t *node, uint32_t ino,
                                  const ext2_inode_t *disk_ino)
   {
       node->ino   = ino;
       node->size  = disk_ino->i_size;
       node->mode  = disk_ino->i_mode;
       node->uid   = disk_ino->i_uid;
       node->gid   = disk_ino->i_gid;
       node->nlink = disk_ino->i_links_count;
       node->atime = disk_ino->i_atime;
       node->mtime = disk_ino->i_mtime;
       node->ctime = disk_ino->i_ctime;
   }
   ```
2. Implement `ext2_iop_lookup` and `ext2_iop_getattr`:
   ```c
   static int ext2_iop_lookup(struct vfs_node *dir, const char *name,
                              struct vfs_node **out)
   {
       if (!out) return -EINVAL;
       *out = NULL;
       if (!dir || !dir->mount || !name) return -EINVAL;
       if (dir->type != VFS_DIR) return -ENOTDIR;

       ext2_fs_t *fs = (ext2_fs_t *)dir->mount->fs_data;
       uint32_t dir_ino = ext2_node_ino(dir);

       spin_lock(&fs->lock);

       uint32_t child_ino = 0, blk = 0, off = 0;
       uint8_t ft = 0;
       int rc = ext2_find_dirent(fs, dir_ino, name, &child_ino, &ft, &blk, &off);
       if (rc != 0) {
           spin_unlock(&fs->lock);
           return rc;
       }

       ext2_inode_t disk_ino;
       if (ext2_read_inode(fs, child_ino, &disk_ino) != 0) {
           spin_unlock(&fs->lock);
           return -EIO;
       }

       spin_unlock(&fs->lock);

       vfs_node_t *child = (vfs_node_t *)calloc(1, sizeof(vfs_node_t));
       if (!child) return -ENOMEM;

       child->name = strndup(name, VFS_NAME_MAX - 1);
       if (!child->name) {
           kfree(child);
           return -ENOMEM;
       }

       switch (ft) {
       case EXT2_FT_DIR:     child->type = VFS_DIR;     break;
       case EXT2_FT_SYMLINK: child->type = VFS_SYMLINK; break;
       case EXT2_FT_CHRDEV:  child->type = VFS_CHRDEV;  break;
       case EXT2_FT_BLKDEV:  child->type = VFS_BLKDEV;  break;
       default:
           if ((disk_ino.i_mode & 0xF000) == EXT2_S_IFDIR)
               child->type = VFS_DIR;
           else if ((disk_ino.i_mode & 0xF000) == EXT2_S_IFLNK)
               child->type = VFS_SYMLINK;
           else
               child->type = VFS_FILE;
           break;
       }

       ext2_fill_vfs_node(child, child_ino, &disk_ino);
       child->mount    = dir->mount;
       child->parent   = vfs_node_get(dir);
       child->fs_data  = (void *)(uintptr_t)child_ino;
       child->i_ops    = &ext2_iops;
       child->f_ops    = &ext2_fops;
       child->refcount = 1;
       *out = child;
       return 0;
   }

   static int ext2_iop_getattr(struct vfs_node *node, struct stat *buf)
   {
       if (!node || !node->mount || !buf) return -1;
       ext2_fs_t *fs = (ext2_fs_t *)node->mount->fs_data;
       uint32_t ino = ext2_node_ino(node);

       spin_lock(&fs->lock);
       ext2_inode_t disk_ino;
       int rc = ext2_read_inode(fs, ino, &disk_ino);
       if (rc == 0) {
           ext2_fill_vfs_node(node, ino, &disk_ino);
       }
       spin_unlock(&fs->lock);

       if (rc != 0) return -EIO;
       int grc = vfs_generic_getattr(node, buf);
       if (grc != 0) return grc;
       buf->st_blksize = fs->block_size;
       buf->st_blocks  = disk_ino.i_blocks;
       return 0;
   }
   ```
3. Wire `.lookup = ext2_iop_lookup` and `.getattr = ext2_iop_getattr` into `ext2_iops`.
4. In `ext2_fs_mount()`: read root inode (`EXT2_ROOT_INO = 2`) and call `ext2_fill_vfs_node(mp->root, EXT2_ROOT_INO, &root_ino)`.
5. In `ext2_vfs_create()` and `ext2_vfs_mkdir()`: populate `node->ino = new_ino`, `node->mode = (EXT2_S_IFREG | 0644)` (or `EXT2_S_IFDIR | 0755`), `node->nlink = 1` (or `2`).

- [ ] **Step 6: Implement `fat_iop_lookup` and `fat_iop_getattr` in `kernel/fs/fat.c`**

In `kernel/fs/fat.c`:
```c
static int fat_iop_lookup(struct vfs_node *dir, const char *name,
                          struct vfs_node **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    if (!dir || !dir->mount || !name) return -EINVAL;
    if (dir->type != VFS_DIR) return -ENOTDIR;

    fat32_fs_t *fs = (fat32_fs_t *)dir->mount->fs_data;
    uint32_t dir_cluster = dir->fs_data
        ? (uint32_t)(uintptr_t)dir->fs_data
        : fs->root_cluster;

    uint64_t lba = 0;
    uint32_t off = 0;
    int64_t idx = fat32_find_by_name(fs, dir_cluster, name, &lba, &off);
    if (idx < 0) return -ENOENT;

    FAT32_DIRENT dirent;
    if (fat32_read_entry_at(fs, lba, off, &dirent) != 0)
        return -EIO;

    uint32_t first_cl = (uint32_t)dirent.first_cluster_lo
                      | ((uint32_t)dirent.first_cluster_hi << 16);

    vfs_node_t *child = (vfs_node_t *)calloc(1, sizeof(vfs_node_t));
    if (!child) return -ENOMEM;

    child->name = strndup(name, VFS_NAME_MAX - 1);
    if (!child->name) {
        kfree(child);
        return -ENOMEM;
    }

    if (dirent.attr & FAT_ATTR_DIRECTORY) {
        child->type  = VFS_DIR;
        child->size  = 0;
        child->mode  = S_IFDIR | 0755;
        child->nlink = 2;
        child->ino   = first_cl;
    } else {
        child->type  = VFS_FILE;
        child->size  = dirent.file_size;
        child->mode  = (dirent.attr & FAT_ATTR_READ_ONLY) ? (S_IFREG | 0555) : (S_IFREG | 0755);
        child->nlink = 1;
        child->ino   = first_cl ? first_cl : ((lba << 16) | (off & 0xFFFF));
    }

    child->mount    = dir->mount;
    child->parent   = vfs_node_get(dir);
    child->fs_data  = (void *)(uintptr_t)first_cl;
    child->i_ops    = &fat_iops;
    child->f_ops    = &fat_fops;
    child->refcount = 1;
    *out = child;
    return 0;
}

static int fat_iop_getattr(struct vfs_node *node, struct stat *buf)
{
    if (!node || !buf) return -1;
    return vfs_generic_getattr(node, buf);
}
```
Wire `.lookup = fat_iop_lookup` and `.getattr = fat_iop_getattr` into `fat_iops`.

- [ ] **Step 7: Run `make test-host` and verify `test_vfs_iops_fops.elf` passes (GREEN)**

Run:
```bash
make test-host
```
Expected: `test_vfs_iops_fops.elf` and all other host test binaries pass.

- [ ] **Step 8: Commit Task 2**

```bash
git add kernel/fs/devfs.c kernel/fs/procfs.c kernel/fs/tmpfs.c \
        kernel/fs/ext2.c kernel/fs/fat.c \
        hosttests/cases/test_vfs_iops_fops.c hosttests/Makefile
git commit -m "feat(vfs): implement i_ops lookup/getattr and node metadata across all filesystems (ARCH-5 Phase 2A)"
```

---

### Task 3: Full Clean Rebuild & Three-Tier Verification Gate (Phase 2A)

**Files:**
- No new source modifications (verification gate for Phase 2A).

- [ ] **Step 1: Mandatory `make clean` + Dual-Architecture Kernel Build + `make test-static`**

Because `struct vfs_node` and `struct vfs_mount` changed in `kernel/include/fs/vfs.h`, run a full clean build on both `x86_64-clang` and `aarch64-clang`:

```bash
make PROFILE=x86_64-clang clean
make PROFILE=aarch64-clang clean
make PROFILE=x86_64-clang kernel
make PROFILE=aarch64-clang kernel
make test-static
```
Expected: Both architectures compile with zero warnings/errors, and all static checks pass.

- [ ] **Step 2: Run Host Unit Tests (`make test-host`)**

```bash
make test-host
```
Expected: `[hosttests] all host test binaries passed` (including `test_vfs_fs_registry` and `test_vfs_iops_fops`).

- [ ] **Step 3: Run QEMU `phase-0`, `test-kernel-selftest`, and `test-syscall`**

Remember the rule from `AGENTS.md`: never set `KERNEL_SELFTEST=1` when running `test-syscall`. Run the three suites sequentially:

```bash
make test-qemu SUITE=phase-0
make KERNEL_SELFTEST=1 test-kernel-selftest
make OS01_SYSTEST=1 test-syscall
```
Expected:
- `SUITE=phase-0`: PASS
- `test-kernel-selftest`: PASS (including `ext2_selftest_*`, `tmpfs_selftest_*`, `symlink_selftest_resolver`, `symlink_selftest_ext2_rollback`)
- `test-syscall`: all 228 syscall tests PASS
