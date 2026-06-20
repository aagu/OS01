#include <fs/vfs.h>
#include <kernel/printk.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

// ── Mount table ───────────────────────────────────────────
static vfs_mount_t mount_table[VFS_MOUNTPOINT_MAX];
static int mount_count = 0;
static int vfs_initialized = 0;

// ── Helpers ────────────────────────────────────────────────
// Case-insensitive string compare — FAT32 stores 8.3 names in
// uppercase, so we loosen the lookup to accept any case.
static int vfs_name_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        char ca = *a++;
        char cb = *b++;
        if (ca >= 'a' && ca <= 'z') ca -= 32;
        if (cb >= 'a' && cb <= 'z') cb -= 32;
        if (ca != cb) return (int)(unsigned char)ca - (int)(unsigned char)cb;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

// ── Initialization ────────────────────────────────────────
void vfs_init(void)
{
    memset(mount_table, 0, sizeof(mount_table));
    mount_count = 0;
    vfs_initialized = 1;
    serial_printk("VFS: initialized\n");
}

// ── Mount ─────────────────────────────────────────────────
int vfs_mount(const char *path, block_device_t *dev,
              vfs_ops_t *ops, void *fs_data)
{
    if (!vfs_initialized) vfs_init();
    if (mount_count >= VFS_MOUNTPOINT_MAX) {
        serial_printk("VFS: mount table full\n");
        return -1;
    }

    vfs_mount_t *mp = &mount_table[mount_count];
    mp->dev = dev;
    mp->path = path;
    mp->ops = ops;
    mp->fs_data = fs_data;
    mp->root = NULL;

    // Allocate a root node — the filesystem fills it via ops
    mp->root = (vfs_node_t *)calloc(1, sizeof(vfs_node_t));
    if (!mp->root) {
        serial_printk("VFS: mount: out of memory\n");
        return -1;
    }

    mp->root->mount = mp;
    mp->root->type = VFS_DIR;
    mp->root->ops = ops;
    // Root node uses fs_data = NULL to tell the filesystem to use its
    // internal root cluster (e.g., BPB_RootClus for FAT32).
    // Subdirectory nodes will hold the cluster number here.
    mp->root->fs_data = NULL;
    mp->root->refcount = 1;
    strcpy(mp->root->name, "/");

    mount_count++;
    serial_printk("VFS: mounted '%s'\n", path);
    return 0;
}

// ── Path tokenizer ────────────────────────────────────────
// Returns successive path components, modifying `path` in place.
// After the last component, returns NULL.
static char *next_component(char **path_ptr)
{
    char *p = *path_ptr;
    if (!p || *p == '\0') return NULL;

    // skip leading '/'
    while (*p == '/') p++;

    if (*p == '\0') { *path_ptr = p; return NULL; }

    char *start = p;
    while (*p && *p != '/') p++;

    if (*p == '/') {
        *p = '\0';
        *path_ptr = p + 1;
    } else {
        *path_ptr = p;  // end of string
    }

    return start;
}

// ── Find mount point by prefix match ──────────────────────
static vfs_mount_t *find_mount(const char *path)
{
    // Find the deepest matching mount point
    vfs_mount_t *best = NULL;
    size_t best_len = 0;

    for (int i = 0; i < mount_count; i++) {
        size_t len = strlen(mount_table[i].path);
        if (strncmp(path, mount_table[i].path, len) == 0) {
            // Root mount ("/") matches any path.
            // Non-root mounts must end at a component boundary.
            if (len == 1 && mount_table[i].path[0] == '/') {
                if (len > best_len) { best_len = len; best = &mount_table[i]; }
            } else if (path[len] == '\0' || path[len] == '/') {
                if (len > best_len) { best_len = len; best = &mount_table[i]; }
            }
        }
    }
    return best;
}

// ── Lookup (core) ─────────────────────────────────────────
// Resolves an absolute path to a VFS node.
static vfs_node_t *__vfs_lookup(const char *path)
{
    if (!vfs_initialized || !path) return NULL;

    // Handle root
    if (strcmp(path, "/") == 0) {
        vfs_mount_t *mp = find_mount("/");
        if (!mp || !mp->root) return NULL;
        mp->root->refcount++;
        return mp->root;
    }

    // Find the mount point
    vfs_mount_t *mp = find_mount(path);
    if (!mp || !mp->root || !mp->root->ops) return NULL;

    // Tokenize path — skip mount point prefix for sub-mounts
    char path_copy[VFS_NAME_MAX];
    size_t plen = strlen(path);
    if (plen >= VFS_NAME_MAX) return NULL;
    memcpy(path_copy, path, plen + 1);

    char *ptr;
    size_t mp_len = strlen(mp->path);
    if (mp_len == 1 && mp->path[0] == '/') {
        ptr = path_copy;  // root mount, no prefix to skip
    } else {
        ptr = path_copy + mp_len;
        while (*ptr == '/') ptr++;  // skip leading slash
    }

    char *comp;

    vfs_node_t *current = mp->root;
    current->refcount++;

    while ((comp = next_component(&ptr)) != NULL) {
        if (strlen(comp) == 0) {
            continue;
        }

        vfs_dirent_t entry;
        int found = 0;
        uint64_t idx = 0;

        while (1) {
            int ret = current->ops->readdir(current, idx, &entry);
            if (ret == 0 && entry.name[0] == '\0') break;
            if (ret == 0) {
                if (vfs_name_cmp(entry.name, comp) == 0) {
                    found = 1;
                    break;
                }
            }
            idx++;
        }

        if (!found) {
            current->refcount--;
            return NULL;
        }

        vfs_node_t *child = (vfs_node_t *)calloc(1, sizeof(vfs_node_t));
        if (!child) {
            current->refcount--;
            return NULL;
        }

        child->mount = mp;
        child->parent = current;
        child->type = entry.type;
        child->size = entry.size;
        child->ops = current->ops;
        child->fs_data = (void *)(uintptr_t)entry.ino;
        child->refcount = 1;
        size_t nlen = strlen(entry.name);
        if (nlen >= VFS_NAME_MAX) nlen = VFS_NAME_MAX - 1;
        memcpy(child->name, entry.name, nlen);
        child->name[nlen] = '\0';

        current->refcount--;
        current = child;
    }

    return current;
}

// ── Public lookup: absolute path only ─────────────────────
vfs_node_t *vfs_lookup(const char *path)
{
    return __vfs_lookup(path);
}

// ── Public lookup: relative path support ──────────────────
// If path is absolute (starts with '/'), cwd is ignored.
// Otherwise, path is resolved relative to cwd.
vfs_node_t *vfs_lookup_from(const char *path, const char *cwd)
{
    if (!path) return NULL;

    // Absolute path — use directly
    if (path[0] == '/')
        return __vfs_lookup(path);

    // Relative path with no cwd — can't resolve
    if (!cwd)
        return NULL;

    // Build absolute path: cwd + "/" + path
    char abs_path[VFS_NAME_MAX];
    int cwd_len = (int)strlen(cwd);
    int path_len = (int)strlen(path);
    int total = cwd_len + 1 + path_len;  // cwd + "/" + path
    if (total >= VFS_NAME_MAX)
        return NULL;

    memcpy(abs_path, cwd, cwd_len);
    abs_path[cwd_len] = '/';
    memcpy(abs_path + cwd_len + 1, path, path_len + 1);  // include NUL

    return __vfs_lookup(abs_path);
}

// ── Read ──────────────────────────────────────────────────
int vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    if (!node || !node->ops || !node->ops->read)
        return -1;
    return node->ops->read(node, offset, size, buffer);
}

// ── Write ─────────────────────────────────────────────────
int vfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    if (!node || !node->ops || !node->ops->write)
        return -1;
    return node->ops->write(node, offset, size, buffer);
}

// ── Read directory ────────────────────────────────────────
int vfs_readdir(vfs_node_t *dir, uint64_t index, vfs_dirent_t *entry)
{
    if (!dir || !dir->ops || !dir->ops->readdir)
        return -1;
    return dir->ops->readdir(dir, index, entry);
}

// ── Reference counting ────────────────────────────────────
vfs_node_t *vfs_node_get(vfs_node_t *node)
{
    if (node) node->refcount++;
    return node;
}

void vfs_node_put(vfs_node_t *node)
{
    if (!node) return;
    if (--node->refcount == 0) {
        // For now, free the node
        free(node);
    }
}

// ── Stat ───────────────────────────────────────────────────
// Fills a struct stat from a VFS node.  Used by SYS_stat and SYS_fstat.
int vfs_stat(vfs_node_t *node, struct stat *buf)
{
    if (!node || !buf) return -1;

    memset(buf, 0, sizeof(struct stat));

    // st_ino: use fs_data (cluster number) as inode number
    buf->st_ino = (uint64_t)(uintptr_t)node->fs_data;

    // st_size: file size in bytes
    buf->st_size = (int64_t)node->size;

    // st_mode: file type + default permissions
    switch (node->type) {
    case VFS_FILE:  buf->st_mode = S_IFREG | 0755; break;
    case VFS_DIR:   buf->st_mode = S_IFDIR | 0755; break;
    case VFS_CHRDEV: buf->st_mode = S_IFCHR | 0600; break;
    case VFS_BLKDEV: buf->st_mode = S_IFBLK | 0600; break;
    default:        buf->st_mode = 0; break;
    }

    // Default ownership
    buf->st_uid = 0;
    buf->st_gid = 0;
    buf->st_nlink = 1;

    // Block size and count
    buf->st_blksize = 512;
    buf->st_blocks = (node->size + 511) / 512;

    return 0;
}

// ── getdents64 ─────────────────────────────────────────────
// Packs directory entries into linux_dirent64 format.
// *pos tracks the current readdir index across calls.
// Returns bytes written to buf, 0 at end of directory, -1 on error.
int vfs_getdents(vfs_node_t *dir, struct linux_dirent64 *buf, unsigned int count,
                 uint64_t *pos)
{
    if (!dir || !buf || !pos || dir->type != VFS_DIR)
        return -1;

    unsigned int bytes_written = 0;
    vfs_dirent_t entry;

    while (1) {
        int ret = vfs_readdir(dir, *pos, &entry);
        if (ret != 0 || entry.name[0] == '\0') {
            break;
        }

        size_t name_len = strlen(entry.name);
        // Calculate record length: struct + name + null terminator, aligned to 8
        uint16_t reclen = (uint16_t)(sizeof(struct linux_dirent64) + name_len + 1);
        reclen = (reclen + 7) & ~7;  // 8-byte align

        if (bytes_written + reclen > count) {
            break;
        }

        struct linux_dirent64 *d = (struct linux_dirent64 *)((char *)buf + bytes_written);
        d->d_ino = (uint64_t)entry.ino;
        d->d_off = (int64_t)(*pos + 1);
        d->d_reclen = reclen;

        switch (entry.type) {
        case VFS_FILE:  d->d_type = DT_REG; break;
        case VFS_DIR:   d->d_type = DT_DIR; break;
        case VFS_CHRDEV: d->d_type = DT_CHR; break;
        case VFS_BLKDEV: d->d_type = DT_BLK; break;
        default:        d->d_type = DT_UNKNOWN; break;
        }

        memcpy(d->d_name, entry.name, name_len + 1);

        bytes_written += reclen;
        (*pos)++;
    }

    return (int)bytes_written;
}

void vfs_debug_list(const char *path)
{
    vfs_node_t *dir = vfs_lookup(path);
    if (!dir) {
        serial_printk("VFS: cannot list '%s' (not found)\n", path);
        return;
    }
    if (dir->type != VFS_DIR) {
        serial_printk("VFS: '%s' is not a directory\n", path);
        vfs_node_put(dir);
        return;
    }

    serial_printk("VFS: listing '%s':\n", path);
    vfs_dirent_t entry;
    uint64_t idx = 0;
    int max_iter = 256;  // safety bound to prevent infinite loops

    while (max_iter-- > 0) {
        int ret = vfs_readdir(dir, idx, &entry);
        if (ret != 0) { idx++; continue; }
        if (entry.name[0] == '\0') break;
        if (entry.type == VFS_DIR)
            serial_printk("  [DIR ]  %s\n", entry.name);
        else if (entry.type == VFS_CHRDEV)
            serial_printk("  [CHR ]  %s\n", entry.name);
        else if (entry.type == VFS_BLKDEV)
            serial_printk("  [BLK ]  %s\n", entry.name);
        else
            serial_printk("  [FILE]  %s (%lu bytes)\n", entry.name, entry.size);
        idx++;
    }
    vfs_node_put(dir);
}

// ── Split a path into parent directory path and base name ──
// Given "/foo/bar/baz", sets parent to "/foo/bar" and returns "baz".
// Given "/file", sets parent to "/" and returns "file".
// Given "file" (no slash), uses cwd as parent and returns "file".
// Returns pointer into a static buffer (parent_path), or NULL on error.
static const char *vfs_split_parent(const char *path, const char *cwd,
                                    char parent_path[VFS_NAME_MAX])
{
    if (!path || !parent_path) return NULL;

    size_t plen = strlen(path);
    if (plen >= VFS_NAME_MAX) return NULL;

    // Find the last '/'
    const char *last_slash = NULL;
    for (const char *s = path; *s; s++)
        if (*s == '/') last_slash = s;

    if (last_slash && last_slash != path) {
        // e.g., "/dir/file" — parent is "/dir", name is "file"
        size_t parent_len = (size_t)(last_slash - path);
        memcpy(parent_path, path, parent_len);
        parent_path[parent_len] = '\0';
        return last_slash + 1;
    } else if (last_slash == path && plen > 1) {
        // e.g., "/file" — parent is "/", name is "file"
        parent_path[0] = '/';
        parent_path[1] = '\0';
        return path + 1;
    } else {
        // No slash — relative path, parent is cwd
        if (!cwd) return NULL;
        size_t cwd_len = strlen(cwd);
        if (cwd_len >= VFS_NAME_MAX) return NULL;
        memcpy(parent_path, cwd, cwd_len + 1);
        return path;
    }
}

// ── Unlink a file ─────────────────────────────────────────────
int vfs_unlink(const char *path, const char *cwd)
{
    if (!path) return -EINVAL;

    char parent_path[VFS_NAME_MAX];
    const char *name = vfs_split_parent(path, cwd, parent_path);
    if (!name || *name == '\0') return -EINVAL;

    vfs_node_t *parent = vfs_lookup_from(parent_path, cwd);
    if (!parent) return -ENOENT;
    if (parent->type != VFS_DIR) { vfs_node_put(parent); return -ENOTDIR; }
    if (!parent->ops || !parent->ops->unlink) {
        vfs_node_put(parent);
        return -EROFS;
    }

    int ret = parent->ops->unlink(parent, name);
    vfs_node_put(parent);
    return ret;
}

// ── Create a directory ───────────────────────────────────────
int vfs_mkdir(const char *path, const char *cwd)
{
    if (!path) return -EINVAL;

    char parent_path[VFS_NAME_MAX];
    const char *name = vfs_split_parent(path, cwd, parent_path);
    if (!name || *name == '\0') return -EINVAL;

    vfs_node_t *parent = vfs_lookup_from(parent_path, cwd);
    if (!parent) return -ENOENT;
    if (parent->type != VFS_DIR) { vfs_node_put(parent); return -ENOTDIR; }
    if (!parent->ops || !parent->ops->mkdir) {
        vfs_node_put(parent);
        return -EROFS;
    }

    vfs_node_t *newdir = parent->ops->mkdir(parent, name);
    if (!newdir) { vfs_node_put(parent); return -EEXIST; }

    // The directory was created on disk; we don't need the node ref
    vfs_node_put(newdir);
    vfs_node_put(parent);
    return 0;
}

// ── Remove an empty directory ─────────────────────────────────
int vfs_rmdir(const char *path, const char *cwd)
{
    if (!path) return -EINVAL;

    char parent_path[VFS_NAME_MAX];
    const char *name = vfs_split_parent(path, cwd, parent_path);
    if (!name || *name == '\0') return -EINVAL;

    vfs_node_t *parent = vfs_lookup_from(parent_path, cwd);
    if (!parent) return -ENOENT;
    if (parent->type != VFS_DIR) { vfs_node_put(parent); return -ENOTDIR; }
    if (!parent->ops || !parent->ops->rmdir) {
        vfs_node_put(parent);
        return -EROFS;
    }

    int ret = parent->ops->rmdir(parent, name);
    vfs_node_put(parent);
    return ret;
}

// ── Rename a file/directory ───────────────────────────────────
int vfs_rename(const char *oldpath, const char *newpath, const char *cwd)
{
    if (!oldpath || !newpath) return -EINVAL;

    char old_parent[VFS_NAME_MAX], new_parent[VFS_NAME_MAX];
    const char *oldname = vfs_split_parent(oldpath, cwd, old_parent);
    const char *newname = vfs_split_parent(newpath, cwd, new_parent);
    if (!oldname || *oldname == '\0' || !newname || *newname == '\0')
        return -EINVAL;

    vfs_node_t *olddir = vfs_lookup_from(old_parent, cwd);
    if (!olddir) return -ENOENT;
    if (olddir->type != VFS_DIR) { vfs_node_put(olddir); return -ENOTDIR; }

    vfs_node_t *newdir = vfs_lookup_from(new_parent, cwd);
    if (!newdir) { vfs_node_put(olddir); return -ENOENT; }
    if (newdir->type != VFS_DIR) {
        vfs_node_put(olddir);
        vfs_node_put(newdir);
        return -ENOTDIR;
    }

    if (!olddir->ops || !olddir->ops->rename) {
        vfs_node_put(olddir);
        vfs_node_put(newdir);
        return -EROFS;
    }

    int ret = olddir->ops->rename(olddir, oldname, newdir, newname);
    vfs_node_put(olddir);
    vfs_node_put(newdir);
    return ret;
}

// ── Truncate a file ──────────────────────────────────────────
int vfs_truncate(vfs_node_t *node, uint64_t new_size)
{
    if (!node) return -EINVAL;
    if (node->type != VFS_FILE) return -EISDIR;
    if (!node->ops || !node->ops->truncate) return -EROFS;

    return node->ops->truncate(node, new_size);
}
