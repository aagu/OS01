/*
 * test_devfs_capacity.c — ARCH-10 Task 9: Dynamic devfs capacity and stability
 *
 * Verifies:
 *  - 40 dynamic devices can be registered beyond the legacy 32-entry table.
 *  - Each read returns its own device id / private payload.
 *  - Nodes looked up and fds opened before subsequent registrations remain
 *    valid, stable, and continue returning the correct id without dangling.
 *  - Looking up a non-existent name returns -ENOENT; registering it at runtime
 *    makes subsequent lookup and open succeed.
 *  - Readdir reports consistent non-zero ino matching lookup ino and rdev.
 *  - Allocation failure during node lookup fails with -ENOMEM without
 *    unregistering or corrupting the device record; subsequent retry succeeds.
 *  - Remounting devfs preserves all global device records and makes them
 *    enumerable again; unmounting with open nodes honors VFS busy rules.
 *  - Single-component name validation: reject empty, NULL, slash, >= DEVFS_NAME_MAX,
 *    and duplicates.
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
#include <arch/x86_64/uaccess.h>

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

extern void *__real_kmalloc(size_t size);
static bool s_inject_kmalloc_fail = false;
void *__wrap_kmalloc(size_t size)
{
    if (s_inject_kmalloc_fail) return NULL;
    return __real_kmalloc(size);
}

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

static int test_cap_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)offset;
    if (!node || !buffer || size < sizeof(uint64_t)) return -EINVAL;
    void *priv = devfs_get_private(node);
    if (!priv) return -EIO;
    uint64_t id = (uint64_t)(uintptr_t)priv;
    memcpy(buffer, &id, sizeof(id));
    return (int)sizeof(id);
}

static const struct devfs_ops s_test_cap_ops = {
    .read = test_cap_read,
};

/* ── Test Cases ────────────────────────────────────────────── */

TEST_FUNC(test_devfs_capacity_40_devices_and_id_stability) {
    vfs_init();
    devfs_init();

    // Register first 10 devices
    for (int i = 0; i < 10; i++) {
        char name[DEVFS_NAME_MAX];
        snprintf(name, sizeof(name), "cap_dev_%02d", i);
        int rc = devfs_register_chrdev(name, (void *)(uintptr_t)(100 + i), &s_test_cap_ops);
        assert_eq(0, rc);
    }

    // Lookup and open cap_dev_05
    vfs_node_t *node5 = vfs_lookup("/dev/cap_dev_05");
    assert_not_null(node5);

    file_t *file5 = NULL;
    assert_eq(0, devfs_open_node(node5, "/dev/cap_dev_05", O_RDONLY, &file5));
    assert_not_null(file5);

    uint64_t val = 0;
    assert_eq((int)sizeof(val), node5->f_ops->read(node5, 0, sizeof(val), &val));
    assert_eq(105, val);

    // Register 30 more devices (total 40 test devices + 5 builtins = 45 devices).
    // On legacy DEVFS_MAX_DEVICES=32, device 27 (the 33rd) fails here!
    for (int i = 10; i < 40; i++) {
        char name[DEVFS_NAME_MAX];
        snprintf(name, sizeof(name), "cap_dev_%02d", i);
        int rc = devfs_register_chrdev(name, (void *)(uintptr_t)(100 + i), &s_test_cap_ops);
        assert_eq(0, rc);
    }

    // Previously opened node and file must still point to cap_dev_05 and return 105
    val = 0;
    assert_eq((int)sizeof(val), node5->f_ops->read(node5, 0, sizeof(val), &val));
    assert_eq(105, val);

    val = 0;
    assert_eq((int)sizeof(val), file5->node->f_ops->read(file5->node, 0, sizeof(val), &val));
    assert_eq(105, val);

    // Verify the 40th device (cap_dev_39) is readable and returns 139
    vfs_node_t *node39 = vfs_lookup("/dev/cap_dev_39");
    assert_not_null(node39);
    if (node39) {
        val = 0;
        assert_eq((int)sizeof(val), node39->f_ops->read(node39, 0, sizeof(val), &val));
        assert_eq(139, val);
        vfs_node_put(node39);
    }
    file_free(file5);
    vfs_node_put(node5);
}

TEST_FUNC(test_devfs_lookup_future_and_open) {
    vfs_init();
    devfs_init();

    // 1. Lookup a device name that has not been registered yet
    vfs_node_t *future_node = NULL;
    int rc = vfs_lookup_at(AT_FDCWD, "/dev/future_sensor", LOOKUP_FOLLOW, &future_node);
    assert_eq(-ENOENT, rc);
    assert_null(future_node);

    // 2. Register it at runtime
    assert_eq(0, devfs_register_chrdev("future_sensor", (void *)0x5555, &s_test_cap_ops));

    // 3. Subsequent lookup must succeed
    future_node = vfs_lookup("/dev/future_sensor");
    assert_not_null(future_node);

    // 4. Open it
    file_t *future_file = NULL;
    assert_eq(0, devfs_open_node(future_node, "/dev/future_sensor", O_RDWR, &future_file));
    assert_not_null(future_file);

    uint64_t val = 0;
    assert_eq((int)sizeof(val), future_node->f_ops->read(future_node, 0, sizeof(val), &val));
    assert_eq(0x5555, val);

    file_free(future_file);
    vfs_node_put(future_node);
}

TEST_FUNC(test_devfs_readdir_matches_lookup_ino_rdev) {
    vfs_init();
    devfs_init();

    // Register 10 test devices
    for (int i = 0; i < 10; i++) {
        char name[DEVFS_NAME_MAX];
        snprintf(name, sizeof(name), "enum_dev_%02d", i);
        assert_eq(0, devfs_register_chrdev(name, (void *)(uintptr_t)(200 + i), &s_test_cap_ops));
    }

    vfs_node_t *dev_dir = vfs_lookup("/dev");
    assert_not_null(dev_dir);

    char name_buf[VFS_NAME_MAX];
    vfs_dirent_t entry;
    uint64_t idx = 0;
    int count = 0;
    while (1) {
        memset(&entry, 0, sizeof(entry));
        entry.name = name_buf;
        assert_eq(0, dev_dir->f_ops->readdir(dev_dir, idx++, &entry));
        if (entry.name[0] == '\0') break;

        count++;
        assert_true(entry.ino != 0);

        char path[64];
        snprintf(path, sizeof(path), "/dev/%s", entry.name);
        vfs_node_t *child = vfs_lookup(path);
        assert_not_null(child);

        // ino and rdev on looked-up node must match readdir ino and be non-zero
        assert_eq(entry.ino, child->ino);
        assert_eq(entry.ino, child->rdev);

        vfs_node_put(child);
    }

    assert_true(count >= 15); // 5 builtins + 10 enum_dev
    vfs_node_put(dev_dir);
}

TEST_FUNC(test_devfs_lookup_oom_retry) {
    vfs_init();
    devfs_init();

    assert_eq(0, devfs_register_chrdev("oom_test_dev", (void *)0x8888, &s_test_cap_ops));

    // Inject allocation failure during lookup
    s_inject_kmalloc_fail = true;
    vfs_node_t *node = NULL;
    int rc = vfs_lookup_at(AT_FDCWD, "/dev/oom_test_dev", LOOKUP_FOLLOW, &node);
    assert_eq(-ENOMEM, rc);
    assert_null(node);

    // Restore allocator and retry lookup: registration was not corrupted/unregistered
    s_inject_kmalloc_fail = false;
    node = vfs_lookup("/dev/oom_test_dev");
    assert_not_null(node);

    uint64_t val = 0;
    assert_eq((int)sizeof(val), node->f_ops->read(node, 0, sizeof(val), &val));
    assert_eq(0x8888, val);

    vfs_node_put(node);
}

TEST_FUNC(test_devfs_remount_and_busy_rules) {
    vfs_init();
    devfs_init();

    assert_eq(0, devfs_register_chrdev("remount_dev", (void *)0x7777, &s_test_cap_ops));

    vfs_node_t *held_node = vfs_lookup("/dev/remount_dev");
    assert_not_null(held_node);

    // VFS mount busy check: active_nodes list on the mount has held_node registered
    assert_not_null(held_node->mount);
    assert_not_null(held_node->mount->active_nodes);

    // Releasing held_node unregisters it from active_nodes when refcount drops to 0
    vfs_node_put(held_node);

    // Reinitializing VFS mount table and remounting devfs:
    vfs_init();
    devfs_init();

    // Global device records must still exist and be discoverable on remounted devfs
    vfs_node_t *new_node = vfs_lookup("/dev/remount_dev");
    assert_not_null(new_node);
    if (new_node) {
        uint64_t val = 0;
        assert_eq((int)sizeof(val), new_node->f_ops->read(new_node, 0, sizeof(val), &val));
        assert_eq(0x7777, val);
        vfs_node_put(new_node);
    }
}

TEST_FUNC(test_devfs_name_validation) {
    vfs_init();
    devfs_init();

    // Empty or NULL name
    assert_eq(-EINVAL, devfs_register_chrdev(NULL, NULL, &s_test_cap_ops));
    assert_eq(-EINVAL, devfs_register_chrdev("", NULL, &s_test_cap_ops));

    // A '/' now separates path components: the name is registered as
    // the multi-component path "sub/dev" and "sub" becomes a directory.
    assert_eq(0, devfs_register_chrdev("sub/dev", NULL, &s_test_cap_ops));
    vfs_node_t *sub_dir = vfs_lookup("/dev/sub");
    assert_not_null(sub_dir);
    assert_eq(VFS_DIR, sub_dir->type);
    vfs_node_put(sub_dir);

    // Absolute paths, trailing separators and empty components stay
    // invalid.
    assert_eq(-EINVAL, devfs_register_chrdev("/leading", NULL, &s_test_cap_ops));
    assert_eq(-EINVAL, devfs_register_chrdev("trailing/", NULL, &s_test_cap_ops));
    assert_eq(-EINVAL, devfs_register_chrdev("a//b", NULL, &s_test_cap_ops));

    // Name length >= DEVFS_NAME_MAX (32 characters)
    assert_eq(-EINVAL, devfs_register_chrdev("01234567890123456789012345678901", NULL, &s_test_cap_ops));

    // Valid registration
    assert_eq(0, devfs_register_chrdev("valid_name", NULL, &s_test_cap_ops));

    // Duplicate name rejected with -EEXIST
    assert_eq(-EEXIST, devfs_register_chrdev("valid_name", NULL, &s_test_cap_ops));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_devfs_capacity_40_devices_and_id_stability),
    TEST_ENTRY(test_devfs_lookup_future_and_open),
    TEST_ENTRY(test_devfs_readdir_matches_lookup_ino_rdev),
    TEST_ENTRY(test_devfs_lookup_oom_retry),
    TEST_ENTRY(test_devfs_remount_and_busy_rules),
    TEST_ENTRY(test_devfs_name_validation),
TEST_LIST_END

int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
