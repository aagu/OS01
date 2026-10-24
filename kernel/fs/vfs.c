#include <fs/vfs.h>
#include <fs/devfs.h>
#include <fs/ext2.h>
#include <fs/fat.h>
#include <fs/tmpfs.h>
#include <fs/procfs.h>
#include <stdbool.h>
#include <core/debug.h>
#include <core/printk.h>
#include <memory/slab.h>
#include <sync/rwlock.h>
#include <sync/mutex.h>
#include <sched/task.h>      // current->files->cwd for resolve_at
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#ifdef OS01_HOST_TEST
void vfs_test_on_active_register(vfs_node_t *node) __attribute__((weak));
#endif

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

#define VFS_MAX_FS_TYPES 16
static vfs_fs_type_t *fs_type_list = NULL;
static rwlock_t fs_type_lock;
struct vfs_fs_usage_slot {
    const vfs_fs_type_t *type;
    uint32_t             active_count;
};
static struct vfs_fs_usage_slot fs_type_usage[VFS_MAX_FS_TYPES];

static inline bool vfs_is_valid_kptr(const void *p)
{
#ifdef OS01_HOST_TEST
    return p != NULL;
#else
    return (uintptr_t)p >= 0xffff800000000000ULL;
#endif
}

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
    root_mount = NULL;
    next_stub_id = 0;
    mount_count = 0;
    rwlock_init(&mount_lock);
    mutex_init(&vfs_namespace_lock);
    vfs_bootstrap = 1;

    fs_type_list = NULL;
    memset(fs_type_usage, 0, sizeof(fs_type_usage));
    rwlock_init(&fs_type_lock);

    vfs_initialized = 1;
    debug_vfs("VFS: initialized\n");

    devfs_register_fs();
    ext2_register_fs();
    fat_register_fs();
    tmpfs_register_fs();
    procfs_register_fs();
}

// ── Mount-tree lookup and binding helpers ─────────────────

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

    const char *base_name = vfs_split_parent(mp->path, "/", parent_path);
    if (!base_name || base_name[0] == '\0') {
        kfree(parent_path);
        return -EINVAL;
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
        const char *base_name = vfs_split_parent(mp->path, "/", parent_path);
        if (!base_name || base_name[0] == '\0' || strlen(base_name) >= VFS_NAME_MAX) {
            kfree(parent_path);
            return -EINVAL;
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

// ── Mount ─────────────────────────────────────────────────
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

// ── Filesystem type registry ──────────────────────────────

static void vfs_drop_fs_type_usage(const vfs_fs_type_t *type)
{
    rwlock_write_lock(&fs_type_lock);
    for (int i = 0; i < VFS_MAX_FS_TYPES; i++) {
        if (fs_type_usage[i].type == type) {
            if (fs_type_usage[i].active_count > 0) {
                fs_type_usage[i].active_count--;
            }
            break;
        }
    }
    rwlock_write_unlock(&fs_type_lock);
}

int vfs_register_filesystem(vfs_fs_type_t *fs_type)
{
    if (!fs_type || !fs_type->name || fs_type->name[0] == '\0' || !fs_type->mount) {
        return -EINVAL;
    }

    rwlock_write_lock(&fs_type_lock);

    for (vfs_fs_type_t *cur = fs_type_list; cur; cur = cur->next) {
        if (strcmp(cur->name, fs_type->name) == 0) {
            rwlock_write_unlock(&fs_type_lock);
            return -EEXIST;
        }
    }

    int free_slot = -1;
    for (int i = 0; i < VFS_MAX_FS_TYPES; i++) {
        if (fs_type_usage[i].type == NULL) {
            free_slot = i;
            break;
        }
    }
    if (free_slot < 0) {
        rwlock_write_unlock(&fs_type_lock);
        return -ENOMEM;
    }

    fs_type_usage[free_slot].type = fs_type;
    fs_type_usage[free_slot].active_count = 0;

    fs_type->next = fs_type_list;
    fs_type_list = fs_type;

    rwlock_write_unlock(&fs_type_lock);
    return 0;
}

const vfs_fs_type_t *vfs_find_filesystem(const char *name)
{
    if (!name || name[0] == '\0') {
        return NULL;
    }

    rwlock_read_lock(&fs_type_lock);
    const vfs_fs_type_t *match = NULL;
    for (const vfs_fs_type_t *cur = fs_type_list; cur; cur = cur->next) {
        if (strcmp(cur->name, name) == 0) {
            match = cur;
            break;
        }
    }
    rwlock_read_unlock(&fs_type_lock);
    return match;
}

int vfs_unregister_filesystem(const char *name)
{
    if (!name || name[0] == '\0') {
        return -EINVAL;
    }

    rwlock_write_lock(&fs_type_lock);

    vfs_fs_type_t **prev = &fs_type_list;
    vfs_fs_type_t *target = NULL;
    while (*prev) {
        if (strcmp((*prev)->name, name) == 0) {
            target = *prev;
            break;
        }
        prev = &(*prev)->next;
    }

    if (!target) {
        rwlock_write_unlock(&fs_type_lock);
        return -ENOENT;
    }

    int slot = -1;
    for (int i = 0; i < VFS_MAX_FS_TYPES; i++) {
        if (fs_type_usage[i].type == target) {
            slot = i;
            break;
        }
    }

    if (slot >= 0 && fs_type_usage[slot].active_count > 0) {
        rwlock_write_unlock(&fs_type_lock);
        return -EBUSY;
    }

    *prev = target->next;
    target->next = NULL;

    if (slot >= 0) {
        fs_type_usage[slot].type = NULL;
        fs_type_usage[slot].active_count = 0;
    }

    rwlock_write_unlock(&fs_type_lock);
    return 0;
}

int vfs_mount_fs(const char *type_name, const char *path,
                 block_device_t *dev, void *data)
{
    if (!type_name || type_name[0] == '\0' || !path || path[0] != '/') {
        return -EINVAL;
    }

    if (!vfs_initialized) {
        vfs_init();
    }

    rwlock_write_lock(&fs_type_lock);
    vfs_fs_type_t *fs_type = NULL;
    for (vfs_fs_type_t *cur = fs_type_list; cur; cur = cur->next) {
        if (strcmp(cur->name, type_name) == 0) {
            fs_type = cur;
            break;
        }
    }

    if (!fs_type) {
        rwlock_write_unlock(&fs_type_lock);
        return -ENODEV;
    }

    if ((fs_type->fs_flags & VFS_FS_REQUIRES_DEV) && !dev) {
        rwlock_write_unlock(&fs_type_lock);
        return -EINVAL;
    }

    for (int i = 0; i < VFS_MAX_FS_TYPES; i++) {
        if (fs_type_usage[i].type == fs_type) {
            fs_type_usage[i].active_count++;
            break;
        }
    }
    rwlock_write_unlock(&fs_type_lock);

    char *path_copy = kmalloc(VFS_NAME_MAX);
    if (!path_copy) {
        vfs_drop_fs_type_usage(fs_type);
        return -ENOMEM;
    }
    normalize_vfs_path(path, path_copy);

    vfs_ns_lock();
    if (vfs_has_duplicate_mount_path(path_copy)) {
        vfs_ns_unlock();
        kfree(path_copy);
        vfs_drop_fs_type_usage(fs_type);
        return -EBUSY;
    }

    vfs_mount_t *mp = (vfs_mount_t *)calloc(1, sizeof(vfs_mount_t));
    if (!mp) {
        vfs_ns_unlock();
        kfree(path_copy);
        vfs_drop_fs_type_usage(fs_type);
        return -ENOMEM;
    }

    mp->root = (vfs_node_t *)calloc(1, sizeof(vfs_node_t));
    if (!mp->root) {
        vfs_ns_unlock();
        kfree(path_copy);
        kfree(mp);
        vfs_drop_fs_type_usage(fs_type);
        return -ENOMEM;
    }

    mp->root->name = strdup("/");
    if (!mp->root->name) {
        vfs_ns_unlock();
        kfree(mp->root);
        kfree(path_copy);
        kfree(mp);
        vfs_drop_fs_type_usage(fs_type);
        return -ENOMEM;
    }

    mp->dev = dev;
    mp->fs_type = fs_type;
    mp->mnt_flags = fs_type->fs_flags;
    mp->path = path_copy;
    spin_init(&mp->active_lock);
    mp->active_nodes = NULL;
    mp->root->mount = mp;
    mp->root->type = VFS_DIR;
    mp->root->ino = VFS_SYNTH_ROOT_INO;
    mp->root->mode = S_IFDIR | 0755;
    mp->root->nlink = 2;
    mp->root->refcount = 1;

    int rc = fs_type->mount(mp, dev, data);
    if (rc < 0 || !mp->root) {
        vfs_ns_unlock();
        if (mp->root) {
            if (mp->root->name) {
                kfree(mp->root->name);
            }
            kfree(mp->root);
        }
        kfree(path_copy);
        kfree(mp);
        vfs_drop_fs_type_usage(fs_type);
        return rc < 0 ? rc : -EIO;
    }

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

    debug_vfs("VFS: mounted '%s' (type %s)\n", path, type_name);
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

// ── Path normalizer ──────────────────────────────────────
// Collapse repeated '/' and remove "/./" segments in-place.  Without
// this, paths like "/./dev" (which busybox's ls produces via
// concat_path_file(".", "dev") when scanning the cwd) fail to match
// the /dev mount in find_mount() because find_mount uses strict
// prefix matching — "/./dev" ≠ "/dev", so the lookup falls through
// to the root ext2 inode, which doesn't contain /dev.  ".." is
// intentionally NOT handled here (it requires the cwd, which
// resolve_at has already collapsed into the absolute path before us).
//
// Caller must ensure `out` has room for at least VFS_NAME_MAX bytes.
// Returns 0 on success; the normalized path is always ≤ strlen(in).
static int normalize_vfs_path(const char *in, char *out)
{
    const char *r = in;
    char *w = out;

    // Collapse leading "/" runs to a single "/".
    if (*r == '/') {
        *w++ = '/';
        r++;
        while (*r == '/') r++;
    }

    while (*r) {
        // Read next component up to the next '/' (or end).
        const char *start = r;
        while (*r && *r != '/') r++;
        size_t len = (size_t)(r - start);

        if (len == 1 && start[0] == '.') {
            // Skip "." segment.
        } else if (len == 0) {
            // Empty (consecutive '/') — already collapsed above.
        } else {
            // Add a '/' separator if this isn't the first emitted
            // component.
            if (w > out && w[-1] != '/') *w++ = '/';
            memcpy(w, start, len);
            w += len;
        }

        // Skip the '/' and any additional ones (handles "//" and
        // "/foo//bar" — also the "/" that follows a stripped "./").
        while (*r == '/') r++;
    }

    // "/." → "/" trailing trim: if the last emitted char is '.'
    // preceded by '/', drop the '.'.
    if (w > out + 1 && w[-1] == '.' && w[-2] == '/') w--;

    *w = '\0';
    return 0;
}

// ── Find pending submount during early boot (before "/" is mounted) ──
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

// ── Generic lookup fallback via readdir ───────────────────
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

    vfs_active_node_register(child);
    *out = child;
    return 0;
}

// ── Lookup (core, raw) ────────────────────────────────────
// Resolves an absolute path to a VFS node, with mid-path symlink early-return.
//
// Return value (2-state):
//   0    = walked to a non-symlink final node; *out_node holds the node
//          with refcount++.  consumed_out / remaining_out are optional and
//          contain the walked path / "" respectively.
//   1    = hit a symlink (mid-path or last component); *out_node holds the
//          symlink with refcount++.  *consumed_out is the absolute prefix
//          walked up to (but not including) the symlink; *remaining_out is
//          the unconsumed suffix after the symlink (contract: "" or starts
//          with '/').
//   < 0  = -errno (ENOENT / ENOTDIR / ENAMETOOLONG / EIO / EINVAL / ENOMEM);
//          *out_node = NULL.
//
// NOFOLLOW semantics are NOT applied here — the caller (vfs_lookup_at /
// vfs_lookup_resolved) decides whether to splice + restart based on flags
// and the suffix content.
static __attribute__((noinline)) int __vfs_lookup_raw(const char *path,
                            vfs_node_t **out_node,
                            char *consumed_out, size_t consumed_size,
                            char *remaining_out, size_t remaining_size)
{
    *out_node = NULL;
    if (consumed_out && consumed_size > 0) consumed_out[0] = '\0';
    if (remaining_out && remaining_size > 0) remaining_out[0] = '\0';

    if (!vfs_initialized || !path) return -EINVAL;

    struct vfs_raw_lookup_buffers {
        char path_buf[VFS_NAME_MAX];
        char path_copy[VFS_NAME_MAX];
    };
    struct vfs_raw_lookup_buffers *bufs = kmalloc(sizeof(struct vfs_raw_lookup_buffers));
    if (!bufs) return -ENOMEM;

    normalize_vfs_path(path, bufs->path_buf);
    const char *norm_path = bufs->path_buf;

    size_t plen = strlen(norm_path);
    if (plen >= VFS_NAME_MAX) {
        kfree(bufs);
        return -ENAMETOOLONG;
    }

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
                    // Last component → empty suffix.
                    remaining_out[0] = '\0';
                } else {
                    // Mid-path: contract requires suffix starts with '/'.
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

    // Walk completed with no symlink — *out_node is the final node.
    *out_node = cur;
    ret = 0;

out:
    kfree(bufs);
    return ret;
}

// ── Splice symlink target + remaining suffix ─────────────
// Builds the next iteration's absolute path from the consumed prefix,
// the symlink target (read via ops->readlink), and the unconsumed suffix.
//
//   target[0] == '/'  →  new = target + suffix       (absolute override)
//   else              →  new = dirname(consumed) + "/" + target + suffix
//
// Returns 0 on success or -ENAMETOOLONG if the expanded path exceeds
// out_size.  On error, the caller is responsible for freeing `target`
// and dropping the symlink node (handled in vfs_lookup_resolved).
static int splice_symlink_path(const char *consumed, const char *target,
                               const char *suffix,
                               char *out, size_t out_size)
{
    if (!consumed || !target || !out || out_size == 0) return -EINVAL;

    size_t pos = 0;

    if (target[0] == '/') {
        // Absolute override — consumed is irrelevant.
        size_t tlen = strlen(target);
        if (tlen + 1 > out_size) return -ENAMETOOLONG;
        memcpy(out, target, tlen);
        pos = tlen;
    } else {
        // Relative: prepend dirname(consumed).
        //   consumed "/foo/bar" → prefix "/foo"
        //   consumed "/foo"     → prefix "/"    (root)
        //   consumed "/"        → prefix "/"    (no change)
        const char *last_slash = NULL;
        for (const char *s = consumed; *s; s++)
            if (*s == '/') last_slash = s;

        const char *prefix;
        size_t prefix_len;
        if (!last_slash) {
            // No "/" in consumed (shouldn't happen for absolute paths).
            prefix = "";
            prefix_len = 0;
        } else if (last_slash == consumed) {
            // consumed is "/" or "/foo" — prefix is just "/".
            prefix = "/";
            prefix_len = 1;
        } else {
            // consumed is "/foo/bar" or deeper.
            prefix = consumed;
            prefix_len = (size_t)(last_slash - consumed);
        }

        if (prefix_len + 1 + 1 > out_size) return -ENAMETOOLONG;
        if (prefix_len > 0) memcpy(out, prefix, prefix_len);
        out[prefix_len] = '/';
        pos = prefix_len + 1;

        size_t tlen = strlen(target);
        if (pos + tlen + 1 > out_size) return -ENAMETOOLONG;
        memcpy(out + pos, target, tlen);
        pos += tlen;
    }

    // Append suffix (already starts with '/' or is empty per contract).
    size_t slen = strlen(suffix);
    if (pos + slen + 1 > out_size) return -ENAMETOOLONG;
    if (slen > 0) memcpy(out + pos, suffix, slen);
    pos += slen;
    out[pos] = '\0';

    return 0;
}

// ── Resolve dirfd + path into an absolute path ───────────
//
//   dirfd == AT_FDCWD && path absolute → out = path
//   dirfd == AT_FDCWD && path relative → out = current->files->cwd + "/" + path
//   dirfd != AT_FDCWD                 → -EBADF (real fd support deferred)
//
// Absolute paths always ignore dirfd (v5 fix).
static int resolve_at(int dirfd, const char *path,
                      char *out, size_t out_size)
{
    if (!path || !out || out_size == 0) return -EINVAL;

    size_t plen = strlen(path);
    if (plen == 0) return -EINVAL;

    // Absolute path — ignore dirfd entirely.
    if (path[0] == '/') {
        if (plen + 1 > out_size) return -ENAMETOOLONG;
        memcpy(out, path, plen + 1);
        return 0;
    }

    // Relative path — require AT_FDCWD for now.
    if (dirfd != AT_FDCWD) return -EBADF;

    const char *cwd = "/";
    if (current && current->files && current->files->cwd) {
        cwd = current->files->cwd;
    }

    size_t cwd_len = strlen(cwd);
    int add_sep = (cwd_len > 0 && cwd[cwd_len - 1] != '/') ? 1 : 0;
    size_t total = cwd_len + add_sep + plen;
    if (total + 1 > out_size) return -ENAMETOOLONG;

    size_t off = 0;
    if (cwd_len > 0) {
        memcpy(out, cwd, cwd_len);
        off = cwd_len;
    }
    if (add_sep) out[off++] = '/';
    memcpy(out + off, path, plen + 1);

    return 0;
}

// ── Symlink-aware lookup (fold walk + splice + restart) ──
//
// Iterates: walk → if symlink, splice + restart, up to MAXSYMLINKS hops.
// NOFOLLOW only blocks when suffix is empty (last-component symlink).
//
// Returns 0 on success (*out_node refcount++) or -errno on failure.
// Caller must vfs_node_put(*out_node) on success.
static int vfs_lookup_resolved(const char *absolute,
                               lookup_flags_t flags,
                               vfs_node_t **out_node)
{
    *out_node = NULL;

    size_t alen = strlen(absolute);
    if (alen >= VFS_NAME_MAX) return -ENAMETOOLONG;

    struct vfs_lookup_ctx {
        char remaining[VFS_NAME_MAX];
        char consumed[VFS_NAME_MAX];
        char suffix[VFS_NAME_MAX];
        char new_remaining[VFS_NAME_MAX];
        char target[VFS_NAME_MAX];
    };
    struct vfs_lookup_ctx *ctx = kmalloc(sizeof(struct vfs_lookup_ctx));
    if (!ctx) return -ENOMEM;

    memcpy(ctx->remaining, absolute, alen + 1);

    vfs_node_t *node = NULL;
    int depth = 0;
    int ret = 0;

    for (;;) {
        int st = __vfs_lookup_raw(ctx->remaining, &node,
                                  ctx->consumed, sizeof(ctx->consumed),
                                  ctx->suffix, sizeof(ctx->suffix));
        if (st < 0) { ret = st; goto out; }
        if (st == 0) {
            *out_node = node;
            ret = 0;
            goto out;
        }
        // st == 1: symlink hit.
        if (depth >= MAXSYMLINKS) {
            vfs_node_put(node);
            ret = -ELOOP;
            goto out;
        }

        // NOFOLLOW semantics: only block the last-component symlink.
        if ((flags & LOOKUP_NOFOLLOW) && ctx->suffix[0] == '\0') {
            *out_node = node;
            ret = 0;
            goto out;
        }

        if (!node->i_ops || !vfs_is_valid_kptr(node->i_ops) ||
            !node->i_ops->readlink || !vfs_is_valid_kptr(node->i_ops->readlink)) {
            vfs_node_put(node);
            ret = -EOPNOTSUPP;
            goto out;
        }

        int tlen = node->i_ops->readlink(node, ctx->target, VFS_NAME_MAX - 1);
        if (tlen < 0) {
            vfs_node_put(node);
            ret = tlen;
            goto out;
        }
        if (tlen >= VFS_NAME_MAX - 1) {
            // readlink returned >= bufsize; we couldn't NUL-terminate.
            vfs_node_put(node);
            ret = -ENAMETOOLONG;
            goto out;
        }
        ctx->target[tlen] = '\0';

        int src = splice_symlink_path(ctx->consumed, ctx->target, ctx->suffix,
                                      ctx->new_remaining, sizeof(ctx->new_remaining));
        if (src < 0) {
            // Per hard rule: vfs_node_put(node) on failure.
            vfs_node_put(node);
            ret = src;
            goto out;
        }

        vfs_node_put(node);
        memcpy(ctx->remaining, ctx->new_remaining, VFS_NAME_MAX);
        depth++;
    }

out:
    kfree(ctx);
    return ret;
}

// ── Public lookup: absolute path only ─────────────────────
//
// Legacy contract: returns vfs_node_t* (NULL on error, no errno).
// T6 migration: call vfs_lookup_resolved so symlinks at any depth
// are followed (LOOKUP_FOLLOW).  errno from the resolver is dropped.
vfs_node_t *vfs_lookup(const char *path)
{
    if (!path) return NULL;
    vfs_node_t *node = NULL;
    if (vfs_lookup_resolved(path, LOOKUP_FOLLOW, &node) < 0) {
        return NULL;
    }
    return node;
}

// ── Public lookup: relative path support ──────────────────
//
// If path is absolute (starts with '/'), cwd is ignored.
// Otherwise, path is resolved relative to cwd (cwd must be non-NULL).
// Legacy contract: NULL on error.  T6 migration: call
// vfs_lookup_resolved directly to apply the same symlink-following
// behavior as vfs_lookup (LOOKUP_FOLLOW).
vfs_node_t *vfs_lookup_from(const char *path, const char *cwd)
{
    if (!path) return NULL;

    // Absolute path — use directly
    if (path[0] == '/')
        return vfs_lookup(path);

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

    return vfs_lookup(abs_path);
}

// ── Public lookup: dirfd + flags + errno ─────────────────
//
// New T2 API: returns 0/-errno with refcount++ on success.
// Normalizes via resolve_at, then loops in vfs_lookup_resolved.
int vfs_lookup_at(int dirfd, const char *path, lookup_flags_t flags,
                  vfs_node_t **out_node)
{
    if (!out_node) return -EINVAL;
    *out_node = NULL;
    if (!path) return -EINVAL;

    char remaining[VFS_NAME_MAX];
    int rc = resolve_at(dirfd, path, remaining, sizeof(remaining));
    if (rc < 0) return rc;

    return vfs_lookup_resolved(remaining, flags, out_node);
}

// ── Read ──────────────────────────────────────────────────
int vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    if (!node || !node->f_ops || !vfs_is_valid_kptr(node->f_ops))
        return -1;
    if (!node->f_ops->read || !vfs_is_valid_kptr(node->f_ops->read))
        return -1;
    return node->f_ops->read(node, offset, size, buffer);
}

// ── Write ─────────────────────────────────────────────────
int vfs_write(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    if (!node || !node->f_ops || !vfs_is_valid_kptr(node->f_ops))
        return -1;
    if (!node->f_ops->write || !vfs_is_valid_kptr(node->f_ops->write))
        return -1;
    return node->f_ops->write(node, offset, size, buffer);
}

// ── Read directory ────────────────────────────────────────
int vfs_readdir(vfs_node_t *dir, uint64_t index, vfs_dirent_t *entry)
{
    if (!dir || !dir->f_ops || !vfs_is_valid_kptr(dir->f_ops))
        return -1;
    if (!dir->f_ops->readdir || !vfs_is_valid_kptr(dir->f_ops->readdir))
        return -1;
    return dir->f_ops->readdir(dir, index, entry);
}

// ── Active node registry (Spec §4.4, §5.2.2, §5.2.3) ─────
void vfs_active_node_register(vfs_node_t *node)
{
    if (!node || !node->mount) return;
    vfs_mount_t *mp = node->mount;
    if (node == mp->root) return;

    spin_lock(&mp->active_lock);
    // Guard against duplicate registration of the same pointer
    for (vfs_node_t *cur = mp->active_nodes; cur; cur = cur->mnt_active_next) {
        if (cur == node) {
            spin_unlock(&mp->active_lock);
            return;
        }
    }

#ifdef OS01_HOST_TEST
    if (vfs_test_on_active_register) {
        vfs_test_on_active_register(node);
    }
#endif

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

// ── Stat ───────────────────────────────────────────────────
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

// Fills a struct stat from a VFS node.  Used by SYS_stat and SYS_fstat.
int vfs_stat(vfs_node_t *node, struct stat *buf)
{
    if (!node || !buf) return -1;
    if (node->i_ops && vfs_is_valid_kptr(node->i_ops) &&
        node->i_ops->getattr && vfs_is_valid_kptr(node->i_ops->getattr)) {
        return node->i_ops->getattr(node, buf);
    }
    return vfs_generic_getattr(node, buf);
}

// ── Sorted getdents entry (internal, for mount-point injection) ──
// We use vfs_dirent_t (from vfs.h) directly. max 64 entries keeps the
// stack footprint under 20 KB on a 32 KB kernel stack.
#define VFS_GETDENTS_SORT_MAX 64

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

    // ── Phase 3: Sort by name (case-insensitive) ──────────────
    for (int i = 0; i < total - 1; i++) {
        for (int j = 0; j < total - 1 - i; j++) {
            int cmp;
            if (dir->mount && (dir->mount->mnt_flags & VFS_FS_CASE_INSENSITIVE))
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
        case VFS_SYMLINK: d->d_type = DT_LNK; break;
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
// Exported (T6): sys_symlink (T7) needs relative linkpath handling.
const char *vfs_split_parent(const char *path, const char *cwd,
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

// ── Create a regular file ─────────────────────────────────────
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

// ── Create a symbolic link ────────────────────────────────────
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

// ── Unlink a file ─────────────────────────────────────────────
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

// ── Create a directory ───────────────────────────────────────
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

    // The directory was created on disk; we don't need the node ref
    vfs_node_put(newdir);
    vfs_node_put(parent);
    vfs_ns_unlock();
    return 0;
}

// ── Remove an empty directory ─────────────────────────────────
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
    if (parent->node_flags & VFS_NODE_ORPHANED) {
        vfs_node_put(parent);
        vfs_ns_unlock();
        return -ENOENT;
    }
    if (vfs_is_mount_or_ancestor(parent, name)) {
        vfs_node_put(parent);
        vfs_ns_unlock();
        return -EBUSY;
    }
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

// ── Rename a file/directory ───────────────────────────────────
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

// ── Truncate a file ──────────────────────────────────────────
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

    // The mount root (names[depth-1]) is "/" — skip it.
    // Build: mount->path + "/" + names[depth-2] + "/" + ... + names[0]

    size_t written = 0;
    const char *mpath = top_mount ? top_mount->path : node->mount->path;

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
