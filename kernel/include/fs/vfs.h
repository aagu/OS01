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

// ── File operations (filesystem driver provides these) ────
typedef struct vfs_ops {
    int (*read)(struct vfs_node *node, uint64_t offset,
                uint64_t size, void *buffer);
    int (*write)(struct vfs_node *node, uint64_t offset,
                 uint64_t size, void *buffer);
    int (*readdir)(struct vfs_node *node, uint64_t index,
                   struct vfs_dirent *entry);
    // Create a new regular file in a directory node.  Returns the
    // new vfs_node, or NULL on error (e.g., already exists).
    struct vfs_node *(*create)(struct vfs_node *dir, const char *name);

    // Unlink (delete) a file from a directory.  Returns 0 or -errno.
    int (*unlink)(struct vfs_node *dir, const char *name);

    // Create a new directory.  Returns new vfs_node or NULL on error.
    struct vfs_node *(*mkdir)(struct vfs_node *dir, const char *name);

    // Remove an empty directory.  Returns 0 or -errno.
    int (*rmdir)(struct vfs_node *dir, const char *name);

    // Rename a file/directory.  olddir/oldname → newdir/newname.
    // Returns 0 or -errno.
    int (*rename)(struct vfs_node *olddir, const char *oldname,
                  struct vfs_node *newdir, const char *newname);

    // Truncate a file to a new size.  Returns 0 or -errno.
    int (*truncate)(struct vfs_node *node, uint64_t new_size);
} vfs_ops_t;

// ── A mounted filesystem instance ─────────────────────────
typedef struct vfs_mount {
    block_device_t    *dev;
    const char        *path;          // mount point path
    struct vfs_node   *root;          // root node of this filesystem
    struct vfs_ops    *ops;
    void              *fs_data;       // filesystem-private data
} vfs_mount_t;

// ── Directory entry returned by readdir ───────────────────
typedef struct vfs_dirent {
    char     name[VFS_NAME_MAX];
    uint64_t size;
    uint8_t  type;          // VFS_FILE or VFS_DIR
    uint32_t ino;           // filesystem-specific id (cluster number for FAT32)
} vfs_dirent_t;

// ── VFS node (inode) ──────────────────────────────────────
typedef struct vfs_node {
    char              name[VFS_NAME_MAX];
    uint64_t          size;
    uint8_t           type;           // VFS_FILE or VFS_DIR
    struct vfs_mount *mount;
    void             *fs_data;        // filesystem-private (e.g., first cluster)
    struct vfs_node  *parent;
    struct vfs_ops   *ops;
    uint32_t          refcount;
} vfs_node_t;

// ── VFS API ───────────────────────────────────────────────

void vfs_init(void);

// Mount a filesystem from a block device
int vfs_mount(const char *path, block_device_t *dev,
              vfs_ops_t *ops, void *fs_data);

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
