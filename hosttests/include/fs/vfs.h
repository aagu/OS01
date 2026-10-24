#ifndef _FS_VFS_H
#define _FS_VFS_H

#include <stdint.h>
#include <block/blockdev.h>
#include <uapi/stat.h>

#define VFS_FILE   1
#define VFS_DIR    2
#define VFS_CHRDEV 3   // character device
#define VFS_BLKDEV 4   // block device
#define VFS_MOUNTPOINT_MAX 8
#define VFS_NAME_MAX 256

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

// Resolve a path to a VFS node (call vfs_node_put when done).
// Absolute path only — no cwd resolution.
struct vfs_node *vfs_lookup(const char *path);

// Resolve a path, supporting relative paths via cwd.
// If path starts with '/', cwd is ignored.  If cwd is NULL,
// only absolute paths are supported (same as vfs_lookup).
struct vfs_node *vfs_lookup_from(const char *path, const char *cwd);

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

// Truncate a file node to a new size.  Returns 0 or -errno.
int vfs_truncate(vfs_node_t *node, uint64_t new_size);

#endif // _FS_VFS_H
