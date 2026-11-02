/*
 * test_devfs_hierarchy.c — ARCH-11: devfs sub-directory support
 *
 * Verifies:
 *  - A '/'-separated registration path creates intermediate directories
 *    and the full path is reachable through vfs_lookup().
 *  - Readdir enumerates each level's direct children with the right type.
 *  - A device under a sub-directory reads back its own payload.
 *  - Single-component registration keeps working unchanged.
 *  - Path conflicts and malformed paths return the documented errno.
 *  - The depth limit and the total path length limit are enforced.
 */

#include "test_framework.h"
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <block/blockdev.h>
#include <errno.h>
#include <fcntl.h>
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
    (void)dev; (void)lba; (void)count; (void)buf;
    return -EIO;
}

int block_device_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    (void)dev; (void)lba; (void)count; (void)buf;
    return -EIO;
}

uint64_t alloc_4k_page(void) { return (uint64_t)(uintptr_t)calloc(1, 4096); }
void free_4k_page(uint64_t addr) { free((void *)(uintptr_t)addr); }

void get_random_bytes(void *b, size_t n) { if (b && n) memset(b, 0x42, n); }
void random_add_entropy(const void *d, size_t n) { (void)d; (void)n; }
int  user_write_range_begin(uint64_t a, size_t l) { (void)a; (void)l; return 0; }
void user_write_range_end(void) {}
int  user_read_range_begin(uint64_t a, size_t l) { (void)a; (void)l; return 0; }
void user_read_range_end(void) {}
void *keyboard_get_tty(void) { return NULL; }
char read_serial(void) { return 0; }
void write_serial(char c) { (void)c; }
tty_t *get_dev_tty(void) { return NULL; }

static const vfs_inode_ops_t s_dummy_proc_iops = {0};
static const vfs_file_ops_t  s_dummy_proc_fops = {0};
static int host_procfs_mount(struct vfs_mount *mp, block_device_t *dev, void *data)
{
    (void)dev; (void)data;
    mp->i_ops = &s_dummy_proc_iops;
    mp->f_ops = &s_dummy_proc_fops;
    mp->root->i_ops = &s_dummy_proc_iops;
    mp->root->f_ops = &s_dummy_proc_fops;
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

file_t *file_alloc(void)
{
    file_t *f = (file_t *)calloc(1, sizeof(file_t));
    if (f) {
        f->refcount = 1;
        f->type = FD_NONE;
    }
    return f;
}

void file_free(file_t *f)
{
    if (!f) return;
    if (f->type == FD_DEV) {
        devfs_release_file(f);
    }
    if (f->node) {
        vfs_node_put(f->node);
        f->node = NULL;
    }
    free(f);
}

// The device read returns its own private payload as a uint64_t.
static int hier_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)offset;
    if (!node || !buffer || size < sizeof(uint64_t)) return -EINVAL;
    void *priv = devfs_get_private(node);
    if (!priv) return -EIO;
    uint64_t id = (uint64_t)(uintptr_t)priv;
    memcpy(buffer, &id, sizeof(id));
    return (int)sizeof(id);
}

static const struct devfs_ops s_hier_ops = {
    .read = hier_read,
};

/* ── Helpers ─────────────────────────────────────────────────── */

// Does `dir` list an entry called `name` with the given type?
// type == 0 matches any type.
static bool readdir_has(vfs_node_t *dir, const char *name, uint8_t type)
{
    char name_buf[VFS_NAME_MAX];
    vfs_dirent_t entry;
    uint64_t idx = 0;
    while (1) {
        memset(&entry, 0, sizeof(entry));
        entry.name = name_buf;
        if (dir->f_ops->readdir(dir, idx++, &entry) != 0) return false;
        if (entry.name[0] == '\0') return false;
        if (strcmp(entry.name, name) == 0)
            return type == 0 || entry.type == type;
    }
}

/* ── Test Cases ────────────────────────────────────────────── */
// NOTE: devfs records live until shutdown (ARCH-10 §7.3), so they
// survive the vfs_init()/devfs_init() pair each case runs.  Every
// case therefore registers under its OWN top-level directory name to
// stay independent of the cases that ran before it.

TEST_FUNC(test_devfs_hierarchy_register_and_lookup) {
    vfs_init();
    devfs_init();

    assert_eq(0, devfs_register_path("pci/0000:00:1f.2/vendor",
                                     (void *)(uintptr_t)0x1111, &s_hier_ops));

    vfs_node_t *d1 = vfs_lookup("/dev/pci");
    assert_not_null(d1);
    assert_eq(VFS_DIR, d1->type);

    vfs_node_t *d2 = vfs_lookup("/dev/pci/0000:00:1f.2");
    assert_not_null(d2);
    assert_eq(VFS_DIR, d2->type);

    vfs_node_t *leaf = vfs_lookup("/dev/pci/0000:00:1f.2/vendor");
    assert_not_null(leaf);
    assert_eq(VFS_CHRDEV, leaf->type);
    assert_true(leaf->ino != 0);
    assert_eq(leaf->ino, leaf->rdev);

    vfs_node_put(leaf);
    vfs_node_put(d2);
    vfs_node_put(d1);
}

TEST_FUNC(test_devfs_hierarchy_readdir_per_level) {
    vfs_init();
    devfs_init();

    assert_eq(0, devfs_register_path("tree2/0000:00:1f.2/vendor",
                                     (void *)(uintptr_t)0x2222, &s_hier_ops));

    vfs_node_t *root = vfs_lookup("/dev");
    assert_not_null(root);
    assert_true(readdir_has(root, "tree2", VFS_DIR));

    vfs_node_t *d1 = vfs_lookup("/dev/tree2");
    assert_not_null(d1);
    assert_true(readdir_has(d1, "0000:00:1f.2", VFS_DIR));

    vfs_node_t *d2 = vfs_lookup("/dev/tree2/0000:00:1f.2");
    assert_not_null(d2);
    assert_true(readdir_has(d2, "vendor", VFS_CHRDEV));

    vfs_node_put(d2);
    vfs_node_put(d1);
    vfs_node_put(root);
}

TEST_FUNC(test_devfs_hierarchy_read_data) {
    vfs_init();
    devfs_init();

    assert_eq(0, devfs_register_path("tree3/0000:00:1f.2/vendor",
                                     (void *)(uintptr_t)0x3333, &s_hier_ops));

    vfs_node_t *leaf = vfs_lookup("/dev/tree3/0000:00:1f.2/vendor");
    assert_not_null(leaf);
    if (leaf) {
        uint64_t val = 0;
        assert_eq((int)sizeof(val), leaf->f_ops->read(leaf, 0, sizeof(val), &val));
        assert_eq(0x3333, val);
        vfs_node_put(leaf);
    }
}

TEST_FUNC(test_devfs_hierarchy_legacy_single_segment) {
    vfs_init();
    devfs_init();

    assert_eq(0, devfs_register_chrdev("plain_dev", (void *)(uintptr_t)0x4444,
                                       &s_hier_ops));

    vfs_node_t *node = vfs_lookup("/dev/plain_dev");
    assert_not_null(node);
    if (node) {
        assert_eq(VFS_CHRDEV, node->type);
        uint64_t val = 0;
        assert_eq((int)sizeof(val), node->f_ops->read(node, 0, sizeof(val), &val));
        assert_eq(0x4444, val);
        vfs_node_put(node);
    }
}

TEST_FUNC(test_devfs_hierarchy_conflicts) {
    vfs_init();
    devfs_init();

    assert_eq(0, devfs_register_path("tree4/0000:00:1f.2/vendor",
                                     (void *)(uintptr_t)0x5555, &s_hier_ops));

    // Exact duplicate path
    assert_eq(-EEXIST, devfs_register_path("tree4/0000:00:1f.2/vendor",
                                           (void *)(uintptr_t)0x6666, &s_hier_ops));

    // "null" is a built-in character device, not a directory
    assert_eq(-ENOTDIR, devfs_register_path("null/foo",
                                             (void *)(uintptr_t)0x7777, &s_hier_ops));

    // "tree4" is now a directory — the name is taken
    assert_eq(-EEXIST, devfs_register_path("tree4",
                                           (void *)(uintptr_t)0x8888, &s_hier_ops));
}

TEST_FUNC(test_devfs_hierarchy_invalid_paths) {
    vfs_init();
    devfs_init();

    assert_eq(-EINVAL, devfs_register_path(NULL, NULL, &s_hier_ops));
    assert_eq(-EINVAL, devfs_register_path("", NULL, &s_hier_ops));
    assert_eq(-EINVAL, devfs_register_path("/leading", NULL, &s_hier_ops));
    assert_eq(-EINVAL, devfs_register_path("trailing/", NULL, &s_hier_ops));
    assert_eq(-EINVAL, devfs_register_path("a//b", NULL, &s_hier_ops));
    assert_eq(-EINVAL, devfs_register_path("./x", NULL, &s_hier_ops));
    assert_eq(-EINVAL, devfs_register_path("../x", NULL, &s_hier_ops));
    assert_eq(-EINVAL, devfs_register_path("a/./b", NULL, &s_hier_ops));
    assert_eq(-EINVAL, devfs_register_path("01234567890123456789012345678901",
                                           NULL, &s_hier_ops));
}

TEST_FUNC(test_devfs_hierarchy_deep_and_long_paths) {
    vfs_init();
    devfs_init();

    // 16 components is the maximum depth
    char deep[DEVFS_MAX_DEPTH * 16];
    int n = 0;
    for (int i = 0; i < DEVFS_MAX_DEPTH; i++)
        n += snprintf(deep + n, sizeof(deep) - n, "%sd%d", i ? "/" : "", i);
    assert_eq(0, devfs_register_path(deep, (void *)(uintptr_t)0x9999, &s_hier_ops));

    // 17 components exceeds DEVFS_MAX_DEPTH
    char too_deep[DEVFS_MAX_DEPTH * 16];
    n = 0;
    for (int i = 0; i < DEVFS_MAX_DEPTH + 1; i++)
        n += snprintf(too_deep + n, sizeof(too_deep) - n, "%sd%d", i ? "/" : "", i);
    assert_eq(-EINVAL, devfs_register_path(too_deep, NULL, &s_hier_ops));

    // A path whose absolute form would not fit in VFS_NAME_MAX
    char toolong[VFS_NAME_MAX * 2];
    memset(toolong, 'x', sizeof(toolong));
    toolong[0] = 'l';
    toolong[sizeof(toolong) - 1] = '\0';
    assert_eq(-ENAMETOOLONG, devfs_register_path(toolong, NULL, &s_hier_ops));
}

TEST_FUNC(test_devfs_hierarchy_vfs_walk_through_mount) {
    vfs_init();
    devfs_init();

    // The /dev mount root must carry the devfs root record so that
    // the first lookup() below resolves against the tree.
    assert_eq(0, devfs_register_path("usb/xhci/1-1/vendor",
                                     (void *)(uintptr_t)0xAAAA, &s_hier_ops));

    // Walk all three levels through the VFS path resolver.
    vfs_node_t *leaf = vfs_lookup("/dev/usb/xhci/1-1/vendor");
    assert_not_null(leaf);
    if (leaf) {
        assert_eq(VFS_CHRDEV, leaf->type);
        uint64_t val = 0;
        assert_eq((int)sizeof(val), leaf->f_ops->read(leaf, 0, sizeof(val), &val));
        assert_eq(0xAAAA, val);
        vfs_node_put(leaf);
    }

    // The intermediate directory must be reachable as a directory too.
    vfs_node_t *mid = vfs_lookup("/dev/usb/xhci");
    assert_not_null(mid);
    assert_eq(VFS_DIR, mid->type);
    vfs_node_put(mid);

    // A path that continues past a device must fail.
    vfs_node_t *bad = vfs_lookup("/dev/usb/xhci/1-1/vendor/extra");
    assert_null(bad);
}

TEST_FUNC(test_devfs_hierarchy_remount) {
    vfs_init();
    devfs_init();

    assert_eq(0, devfs_register_path("tree5/0000:00:1f.2/vendor",
                                     (void *)(uintptr_t)0xCCCC, &s_hier_ops));

    // Reinitialise VFS + devfs: this remounts devfs at /dev.
    vfs_init();
    devfs_init();

    // The global device records survive the remount (ARCH-10 §7.3), so
    // the whole hierarchy must still be reachable and enumerable.
    vfs_node_t *leaf = vfs_lookup("/dev/tree5/0000:00:1f.2/vendor");
    assert_not_null(leaf);
    if (leaf) {
        uint64_t val = 0;
        assert_eq((int)sizeof(val), leaf->f_ops->read(leaf, 0, sizeof(val), &val));
        assert_eq(0xCCCC, val);
        vfs_node_put(leaf);
    }

    vfs_node_t *d1 = vfs_lookup("/dev/tree5");
    assert_not_null(d1);
    if (d1) {
        assert_true(readdir_has(d1, "0000:00:1f.2", VFS_DIR));
        vfs_node_put(d1);
    }
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_devfs_hierarchy_register_and_lookup),
    TEST_ENTRY(test_devfs_hierarchy_readdir_per_level),
    TEST_ENTRY(test_devfs_hierarchy_read_data),
    TEST_ENTRY(test_devfs_hierarchy_legacy_single_segment),
    TEST_ENTRY(test_devfs_hierarchy_conflicts),
    TEST_ENTRY(test_devfs_hierarchy_invalid_paths),
    TEST_ENTRY(test_devfs_hierarchy_deep_and_long_paths),
    TEST_ENTRY(test_devfs_hierarchy_vfs_walk_through_mount),
    TEST_ENTRY(test_devfs_hierarchy_remount),
TEST_LIST_END

int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
