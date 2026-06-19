#include <fs/vfs.h>
#include <kernel/printk.h>
#include <string.h>
#include <stdlib.h>

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
    mp->root = (vfs_node_t *)calloc(sizeof(vfs_node_t));
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

        vfs_node_t *child = (vfs_node_t *)calloc(sizeof(vfs_node_t));
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

// ── Debug: list directory via serial ──────────────────────
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
