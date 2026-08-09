#include <fs/vfs.h>
#include <kernel/debug.h>
#include <kernel/slab.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

// ── Mount table ───────────────────────────────────────────
static vfs_mount_t *mount_list = NULL;
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
    mount_list = NULL;
    mount_count = 0;
    vfs_initialized = 1;
    debug_vfs("VFS: initialized\n");
}

// ── Mount ─────────────────────────────────────────────────
int vfs_mount(const char *path, block_device_t *dev,
              vfs_ops_t *ops, void *fs_data)
{
    if (!vfs_initialized) vfs_init();

    vfs_mount_t *mp = (vfs_mount_t *)calloc(1, sizeof(vfs_mount_t));
    if (!mp) {
        debug_vfs("VFS: mount: out of memory\n");
        return -1;
    }

    mp->dev = dev;
    mp->path = path;
    mp->ops = ops;
    mp->fs_data = fs_data;
    mp->root = NULL;

    // Allocate a root node — the filesystem fills it via ops
    mp->root = (vfs_node_t *)calloc(1, sizeof(vfs_node_t));
    if (!mp->root) {
        debug_vfs("VFS: mount: root node alloc failed\n");
        kfree(mp);
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
    mp->root->name = strdup("/");
    if (!mp->root->name) {
        kfree(mp->root);
        kfree(mp);
        return -1;
    }

    // Prepend to mount list
    mp->next = mount_list;
    mount_list = mp;
    mount_count++;
    debug_vfs("VFS: mounted '%s'\n", path);
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

    for (vfs_mount_t *mp = mount_list; mp; mp = mp->next) {
        size_t len = strlen(mp->path);
        if (strncmp(path, mp->path, len) == 0) {
            // Root mount ("/") matches any path.
            // Non-root mounts must end at a component boundary.
            if (len == 1 && mp->path[0] == '/') {
                if (len > best_len) { best_len = len; best = mp; }
            } else if (path[len] == '\0' || path[len] == '/') {
                if (len > best_len) { best_len = len; best = mp; }
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
        __sync_add_and_fetch(&mp->root->refcount, 1);
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
    __sync_add_and_fetch(&current->refcount, 1);

    while ((comp = next_component(&ptr)) != NULL) {
        if (strlen(comp) == 0) {
            continue;
        }

        // "." — stay in current directory
        if (strcmp(comp, ".") == 0) {
            continue;
        }

        // ".." — go to parent, or stay if at root
        if (strcmp(comp, "..") == 0) {
            if (current->parent) {
                vfs_node_t *parent = current->parent;
                __sync_add_and_fetch(&parent->refcount, 1);
                __sync_sub_and_fetch(&current->refcount, 1);
                current = parent;
            }
            continue;
        }

        vfs_dirent_t entry;
        char _entry_name[VFS_NAME_MAX];
        entry.name = _entry_name;
        int found = 0;
        uint64_t idx = 0;
        int max_iter = 256;  // safety bound for corrupted ops

        while (max_iter-- > 0) {
            int ret = vfs_readdir(current, idx, &entry);
            if (ret == 0 && entry.name[0] == '\0') break;
            if (ret == 0) {
                int match;
                if (current->ops &&
                    (uint64_t)current->ops >= 0xffff800000000000ULL &&
                    (current->ops->flags & VFS_OPS_CASE_INSENSITIVE))
                    match = (vfs_name_cmp(entry.name, comp) == 0);
                else
                    match = (strcmp(entry.name, comp) == 0);

                if (match) {
                    found = 1;
                    break;
                }
            }
            idx++;
        }

        if (!found) {
            __sync_sub_and_fetch(&current->refcount, 1);
            return NULL;
        }

        vfs_node_t *child = (vfs_node_t *)calloc(1, sizeof(vfs_node_t));
        if (!child) {
            __sync_sub_and_fetch(&current->refcount, 1);
            return NULL;
        }

        child->mount = mp;
        child->parent = current;
        __sync_add_and_fetch(&current->refcount, 1);  // child holds a ref through parent pointer
        child->type = entry.type;
        child->size = entry.size;
        child->ops = current->ops;
        child->fs_data = (void *)(uintptr_t)entry.ino;
        child->refcount = 1;
        child->name = strndup(entry.name, VFS_NAME_MAX - 1);

        __sync_sub_and_fetch(&current->refcount, 1);
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

    // Strip leading "./" prefix — this keeps mount point matching clean
    // (./dev → dev, so the absolute path becomes /dev not /./dev)
    while (path[0] == '.' && path[1] == '/')
        path += 2;

    // Build absolute path: cwd + "/" + path
    // Avoid double slashes when cwd already ends with '/'
    char abs_path[VFS_NAME_MAX];
    int cwd_len = (int)strlen(cwd);
    int path_len = (int)strlen(path);
    int add_sep = (cwd_len > 0 && cwd[cwd_len - 1] != '/') ? 1 : 0;
    int total = cwd_len + add_sep + path_len;
    if (total >= VFS_NAME_MAX)
        return NULL;

    int off = 0;
    if (cwd_len > 0) {
        memcpy(abs_path, cwd, cwd_len);
        off = cwd_len;
    }
    if (add_sep)
        abs_path[off++] = '/';
    memcpy(abs_path + off, path, path_len + 1);  // include NUL

    return __vfs_lookup(abs_path);
}

// ── Read ──────────────────────────────────────────────────
int vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    if (!node || !node->ops || (uint64_t)node->ops < 0xffff800000000000ULL)
        return -1;
    if (!node->ops->read)
        return -1;
    if ((uint64_t)node->ops->read < 0xffff800000000000ULL)
        return -1;
    return node->ops->read(node, offset, size, buffer);
}

// ── Write ─────────────────────────────────────────────────
int vfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    if (!node || !node->ops || (uint64_t)node->ops < 0xffff800000000000ULL)
        return -1;
    if (!node->ops->write)
        return -1;
    if ((uint64_t)node->ops->write < 0xffff800000000000ULL)
        return -1;
    return node->ops->write(node, offset, size, buffer);
}

// ── Read directory ────────────────────────────────────────
int vfs_readdir(vfs_node_t *dir, uint64_t index, vfs_dirent_t *entry)
{
    if (!dir || !dir->ops || (uint64_t)dir->ops < 0xffff800000000000ULL)
        return -1;
    if (!dir->ops->readdir)
        return -1;
    if ((uint64_t)dir->ops->readdir < 0xffff800000000000ULL)
        return -1;
    return dir->ops->readdir(dir, index, entry);
}

// ── Reference counting ────────────────────────────────────
vfs_node_t *vfs_node_get(vfs_node_t *node)
{
    if (node) __sync_add_and_fetch(&node->refcount, 1);
    return node;
}

void vfs_node_put(vfs_node_t *node)
{
    if (!node) return;
    if (__sync_sub_and_fetch(&node->refcount, 1) == 0) {
        vfs_node_t *parent = node->parent;
        // Poison to catch use-after-free: any stale reference
        // will hit a null-pointer check (unlike kernel-address
        // guards which the optimizer may elide).
        node->ops    = NULL;
        node->parent = NULL;
        node->mount  = NULL;
        node->fs_data = NULL;
        if (node->name) kfree(node->name);
        node->name = NULL;
        free(node);
        vfs_node_put(parent);  // release parent ref after child is gone
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

// ── Sorted getdents entry (internal, for mount-point injection) ──
// We use vfs_dirent_t (from vfs.h) directly. max 64 entries keeps the
// stack footprint under 20 KB on a 32 KB kernel stack.
#define VFS_GETDENTS_SORT_MAX 64

// ── Check if mount_path is a direct child of dir_path ───────
// Returns 1 if mount_path is exactly one component deeper than dir_path.
static int vfs_is_child_mount(const char *dir_path, const char *mount_path)
{
    if (!dir_path || !mount_path)
        return 0;

    size_t dlen = strlen(dir_path);

    // Root "/" — resolve to empty; any immediate child is single-component.
    if (dlen == 1 && dir_path[0] == '/') {
        if (mount_path[0] != '/' || mount_path[1] == '\0')
            return 0;
        // e.g., "/dev" → "dev" (no '/' after position 1)
        return strchr(mount_path + 1, '/') == NULL;
    }

    // Non-root: mount_path = dir_path + "/" + one_component
    if (strncmp(dir_path, mount_path, dlen) != 0)
        return 0;
    if (mount_path[dlen] != '/')
        return 0;
    return strchr(mount_path + dlen + 1, '/') == NULL;
}

// ── getdents64 ─────────────────────────────────────────────
// Collects entries from the underlying filesystem, injects VFS mount
// points that are direct children of this directory, sorts all entries
// case-insensitively, then streams them to the user buffer via *pos.
int vfs_getdents(vfs_node_t *dir, struct linux_dirent64 *buf, unsigned int count,
                 uint64_t *pos)
{
    if (!dir || !buf || !pos || dir->type != VFS_DIR)
        return -1;

    // ── Phase 1: Collect entries from the underlying filesystem ──
    vfs_dirent_t *entries = kmalloc(sizeof(vfs_dirent_t) * VFS_GETDENTS_SORT_MAX);
    char *entry_names = kmalloc(VFS_NAME_MAX * VFS_GETDENTS_SORT_MAX);
    if (!entries || !entry_names) {
        if (entries) kfree(entries);
        if (entry_names) kfree(entry_names);
        return -ENOMEM;
    }
    for (int i = 0; i < VFS_GETDENTS_SORT_MAX; i++)
        entries[i].name = entry_names + i * VFS_NAME_MAX;

    int total = 0;
    char _de_name[VFS_NAME_MAX];
    vfs_dirent_t de = { .name = _de_name };
    uint64_t idx = 0;

    while (total < VFS_GETDENTS_SORT_MAX) {
        int ret = vfs_readdir(dir, idx++, &de);
        if (ret != 0) continue;
        if (de.name[0] == '\0') break;

        size_t nlen = strlen(de.name);
        if (nlen >= VFS_NAME_MAX) nlen = VFS_NAME_MAX - 1;
        memcpy(entries[total].name, de.name, nlen);
        entries[total].name[nlen] = '\0';
        entries[total].size = de.size;
        entries[total].type = de.type;
        entries[total].ino  = de.ino;
        total++;
    }

    // ── Phase 2: Inject sub-mount entries ──────────────────────
    // Only when listing a mount root (e.g., "/" which is FAT32's root).
    if (dir->mount && dir == dir->mount->root) {
        for (vfs_mount_t *mp = mount_list; mp && total < VFS_GETDENTS_SORT_MAX; mp = mp->next) {
            if (mp == dir->mount) continue;  // skip self
            if (!vfs_is_child_mount(dir->mount->path, mp->path))
                continue;

            // Extract basename (skip leading "/")
            const char *base = mp->path;
            if (base[0] == '/') base++;
            size_t blen = strlen(base);
            if (blen >= VFS_NAME_MAX) blen = VFS_NAME_MAX - 1;
            memcpy(entries[total].name, base, blen);
            entries[total].name[blen] = '\0';
            entries[total].size = 0;
            entries[total].type = VFS_DIR;
            entries[total].ino  = (uint32_t)(0x80000000 | total);
            total++;
        }
    }

    // ── Phase 3: Sort by name (case-insensitive) ──────────────
    for (int i = 0; i < total - 1; i++) {
        for (int j = 0; j < total - 1 - i; j++) {
            int cmp;
            if (dir->ops && (uint64_t)dir->ops >= 0xffff800000000000ULL &&
                (dir->ops->flags & VFS_OPS_CASE_INSENSITIVE))
                cmp = vfs_name_cmp(entries[j].name, entries[j + 1].name);
            else
                cmp = strcmp(entries[j].name, entries[j + 1].name);
            if (cmp > 0) {
                vfs_dirent_t tmp = entries[j];
                entries[j]     = entries[j + 1];
                entries[j + 1] = tmp;
            }
        }
    }

    // ── Phase 4: Output from sorted list starting at *pos ────
    unsigned int bytes_written = 0;

    while (*pos < (uint64_t)total) {
        vfs_dirent_t *e = &entries[*pos];
        size_t name_len = strlen(e->name);
        uint16_t reclen = (uint16_t)(sizeof(struct linux_dirent64) + name_len + 1);
        reclen = (reclen + 7) & ~7;

        if (bytes_written + reclen > count)
            break;

        struct linux_dirent64 *d = (struct linux_dirent64 *)((char *)buf + bytes_written);
        d->d_ino   = e->ino;
        d->d_off   = (int64_t)(*pos + 1);
        d->d_reclen = reclen;

        switch (e->type) {
        case VFS_FILE:   d->d_type = DT_REG; break;
        case VFS_DIR:    d->d_type = DT_DIR; break;
        case VFS_CHRDEV: d->d_type = DT_CHR; break;
        case VFS_BLKDEV: d->d_type = DT_BLK; break;
        default:         d->d_type = DT_UNKNOWN; break;
        }

        memcpy(d->d_name, e->name, name_len + 1);
        bytes_written += reclen;
        (*pos)++;
    }

    kfree(entry_names);
    kfree(entries);
    return (int)bytes_written;
}

void vfs_debug_list(const char *path)
{
    vfs_node_t *dir = vfs_lookup(path);
    if (!dir) {
        debug_vfs("VFS: cannot list '%s' (not found)\n", path);
        return;
    }
    if (dir->type != VFS_DIR) {
        debug_vfs("VFS: '%s' is not a directory\n", path);
        vfs_node_put(dir);
        return;
    }

    debug_vfs("VFS: listing '%s':\n", path);
    char _entry_name[VFS_NAME_MAX];
    vfs_dirent_t entry = { .name = _entry_name };
    uint64_t idx = 0;
    int max_iter = 256;  // safety bound to prevent infinite loops

    while (max_iter-- > 0) {
        int ret = vfs_readdir(dir, idx, &entry);
        if (ret != 0) { idx++; continue; }
        if (entry.name[0] == '\0') break;
        if (entry.type == VFS_DIR)
            debug_vfs("  [DIR ]  %s\n", entry.name);
        else if (entry.type == VFS_CHRDEV)
            debug_vfs("  [CHR ]  %s\n", entry.name);
        else if (entry.type == VFS_BLKDEV)
            debug_vfs("  [BLK ]  %s\n", entry.name);
        else
            debug_vfs("  [FILE]  %s (%lu bytes)\n", entry.name, entry.size);
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
    if (!parent->ops || (uint64_t)parent->ops < 0xffff800000000000ULL || !parent->ops->unlink) {
        vfs_node_put(parent);
        return -EROFS;
    }
    if ((uint64_t)parent->ops->unlink < 0xffff800000000000ULL) {
        vfs_node_put(parent);
        return -1;
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
    if (!parent->ops || (uint64_t)parent->ops < 0xffff800000000000ULL || !parent->ops->mkdir) {
        vfs_node_put(parent);
        return -EROFS;
    }
    if ((uint64_t)parent->ops->mkdir < 0xffff800000000000ULL) {
        vfs_node_put(parent);
        return -1;
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
    if (!parent->ops || (uint64_t)parent->ops < 0xffff800000000000ULL || !parent->ops->rmdir) {
        vfs_node_put(parent);
        return -EROFS;
    }
    if ((uint64_t)parent->ops->rmdir < 0xffff800000000000ULL) {
        vfs_node_put(parent);
        return -1;
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

    if (!olddir->ops || (uint64_t)olddir->ops < 0xffff800000000000ULL || !olddir->ops->rename) {
        vfs_node_put(olddir);
        vfs_node_put(newdir);
        return -EROFS;
    }
    if ((uint64_t)olddir->ops->rename < 0xffff800000000000ULL) {
        vfs_node_put(olddir);
        vfs_node_put(newdir);
        return -1;
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
    if (!node->ops || (uint64_t)node->ops < 0xffff800000000000ULL || !node->ops->truncate) return -EROFS;
    if ((uint64_t)node->ops->truncate < 0xffff800000000000ULL)
        return -1;

    return node->ops->truncate(node, new_size);
}

// ── Resolve full path from a VFS node ─────────────────────────
//
// Walks parent chain up to mount root, collecting names in a
// stack array, then emits mount->path + names top-down + leaf.
// Mount root's own name ("/") is skipped to avoid doubling.
//
// Returns path length (excl NUL).  Returns >= pathsz if truncated.
// Returns -1 on error (node, mount, or name is NULL).
//
// CONTRACT: path is always NUL-terminated on return, even when
// truncated.  Caller should call with pathsz = real_bufsize - 4
// to reserve room for "...\0" appended after truncation.
int vfs_resolve_path(vfs_node_t *node, char *path, size_t pathsz)
{
    if (!node || !node->mount || !node->name)
        return -1;

    // Collect names bottom-up (max 32 depth — far more than any
    // real path in this system; returning -1 is safer than silent
    // corruption if the limit is ever exceeded)
    const char *names[32];
    int depth = 0;
    vfs_node_t *cur = node;

    while (cur) {
        if (depth >= 32)
            return -1;  // path too deep
        names[depth++] = cur->name;
        if (!cur->parent)
            break;  // reached mount root
        cur = cur->parent;
    }
    // names[0] = leaf, names[depth-1] = mount root ("/")

    // The mount root (names[depth-1]) is "/" — skip it.
    // Build: mount->path + "/" + names[depth-2] + "/" + ... + names[0]

    size_t written = 0;
    const char *mpath = node->mount->path;

    // Helper: safely write a character — NUL-terminates on overflow
    #define PUT(c) do {                                \
        if (written < pathsz) path[written] = (c);     \
        written++;                                      \
    } while (0)
    #define PUTS(s, len) do {                           \
        for (size_t _k = 0; _k < (len); _k++)          \
            PUT((s)[_k]);                               \
    } while (0)
    #define TERM() do {                                 \
        /* At exact-fit (written==pathsz), the last byte  \
           is overwritten with NUL — 1 char lost.         \
           Inherent: pathsz bytes can't store pathsz      \
           chars + NUL. */                                \
        if (pathsz > 0)                                \
            path[(written < pathsz) ? written           \
                                    : pathsz - 1] = '\0'; \
    } while (0)

    if (mpath) {
        size_t mlen = strlen(mpath);
        // Strip trailing '/' from mount path (root mount has "/")
        // so we don't produce "//bin/init.elf".
        if (mlen > 0 && mpath[mlen - 1] == '/')
            mlen--;
        PUTS(mpath, mlen);
    }

    // Emit names from mount root's child down to leaf
    // (skip index depth-1 which is mount root "/")
    for (int i = depth - 2; i >= 0; i--) {
        PUT('/');
        size_t nlen = strlen(names[i]);
        PUTS(names[i], nlen);
    }

    TERM();
    #undef PUT
    #undef PUTS
    #undef TERM

    return (int)written;
}
