#ifndef _FS_VFS_H
#define _FS_VFS_H

#include <stdint.h>
#include <stddef.h>   // size_t
#include <stdbool.h>
#include <block/blockdev.h>
#include <sys/stat.h>
#include <arch/spinlock.h>

typedef enum {
    VFS_FILE    = 1,
    VFS_DIR     = 2,
    VFS_CHRDEV  = 3,
    VFS_BLKDEV  = 4,
    VFS_SYMLINK = 5,   // appended; never renumber existing values (spec §3.1, v5)
} vfs_node_type_t;

// vfs_node_t.type and vfs_dirent_t.type are stored as uint8_t to keep the
// struct small and match the on-disk ext2 dirent file_type width. Lock the
// enum to uint8_t range so adding a new node type that doesn't fit is a
// build break (caught here, not at runtime as a -EIO from a readdir).
_Static_assert(VFS_SYMLINK <= 0xFF, "vfs_node_type_t does not fit in uint8_t");

typedef enum {
    LOOKUP_FOLLOW    = 0,
    LOOKUP_NOFOLLOW  = 1,
} lookup_flags_t;

#define MAXSYMLINKS 8
#define VFS_NAME_MAX 256

// ── Forward declarations ──────────────────────────────────
struct vfs_node;
struct vfs_mount;
struct vfs_dirent;
struct vfs_fs_type;
struct vma;
struct file;

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

// Resolve a path to a VFS node (call vfs_node_put when done).
// Absolute path only — no cwd resolution.
struct vfs_node *vfs_lookup(const char *path);

// Resolve a path, supporting relative paths via cwd.
// If path starts with '/', cwd is ignored.  If cwd is NULL,
// only absolute paths are supported (same as vfs_lookup).
struct vfs_node *vfs_lookup_from(const char *path, const char *cwd);

// Symlink-aware lookup with mid-path + last-component follow loop.
// Returns 0 on success (*out_node refcount++) or -errno on failure.
// Honors dirfd: AT_FDCWD for current cwd, or any other (currently -EBADF).
// flags: LOOKUP_FOLLOW (default) follows symlinks at any depth;
//        LOOKUP_NOFOLLOW only blocks the LAST component (mid-path symlinks
//        are still followed, per POSIX semantics).
int vfs_lookup_at(int dirfd, const char *path, lookup_flags_t flags,
                  vfs_node_t **out_node);

// Read from a file
int vfs_read(struct vfs_node *node, uint64_t offset,
             uint64_t size, void *buffer);

// Write to a file
int vfs_write(struct vfs_node *node, uint64_t offset,
              uint64_t size, void *buffer);

// Read directory entries
int vfs_readdir(struct vfs_node *dir, uint64_t index,
                vfs_dirent_t *entry);

// Reference counting
struct vfs_node *vfs_node_get(struct vfs_node *node);
void vfs_node_put(struct vfs_node *node);

// Debug: list directory contents via serial
void vfs_debug_list(const char *path);

// Fill a stat structure from a VFS node
int vfs_stat(vfs_node_t *node, struct stat *buf);

// Resolve a VFS node's full path by walking parent chain to mount root.
// Returns path length (excluding NUL), or -1 on error (NULL node/mount/name).
// Returns >= pathsz if truncated.
int vfs_resolve_path(vfs_node_t *node, char *path, size_t pathsz);

// Read directory entries into getdents64 format
// Returns bytes written to buf, or -1 on error
int vfs_getdents(vfs_node_t *dir, struct linux_dirent64 *buf, unsigned int count,
                 uint64_t *pos);

// Unlink (delete) a file by path.  Returns 0 or -errno.
int vfs_unlink(const char *path, const char *cwd);

// Create a new directory by path.  Returns 0 or -errno.
int vfs_mkdir(const char *path, const char *cwd);

// Remove an empty directory by path.  Returns 0 or -errno.
int vfs_rmdir(const char *path, const char *cwd);

// Rename oldpath to newpath.  Returns 0 or -errno.
int vfs_rename(const char *oldpath, const char *newpath, const char *cwd);

// Split a path into parent directory path and base name.
// Given "/foo/bar/baz", sets parent_path to "/foo/bar" and returns "baz".
// Given "/file", sets parent_path to "/" and returns "file".
// Given "file" (no slash), uses cwd as parent and returns "file".
// Returns pointer into parent_path, or NULL on error.  parent_path must
// be at least VFS_NAME_MAX bytes.  Exported (T6): sys_symlink (T7) uses
// it for relative linkpath handling.
const char *vfs_split_parent(const char *path, const char *cwd,
                             char parent_path[VFS_NAME_MAX]);

// Truncate a file node to a new size.  Returns 0 or -errno.
int vfs_truncate(vfs_node_t *node, uint64_t new_size);

#endif // _FS_VFS_H
