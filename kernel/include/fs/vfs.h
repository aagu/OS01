#ifndef _FS_VFS_H
#define _FS_VFS_H

#include <stdint.h>
#include <block/blockdev.h>

#define VFS_FILE 1
#define VFS_DIR  2
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
    int (*readdir)(struct vfs_node *node, uint64_t index,
                   struct vfs_dirent *entry);
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

// Resolve a path to a VFS node (call vfs_node_put when done)
struct vfs_node *vfs_lookup(const char *path);

// Read from a file
int vfs_read(struct vfs_node *node, uint64_t offset,
             uint64_t size, void *buffer);

// Read directory entries
int vfs_readdir(struct vfs_node *dir, uint64_t index,
                vfs_dirent_t *entry);

// Reference counting
struct vfs_node *vfs_node_get(struct vfs_node *node);
void vfs_node_put(struct vfs_node *node);

// Debug: list directory contents via serial
void vfs_debug_list(const char *path);

#endif // _FS_VFS_H
