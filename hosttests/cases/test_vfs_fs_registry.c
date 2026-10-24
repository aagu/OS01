#include "test_framework.h"
#include <fs/vfs.h>
#include <block/blockdev.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>

task_t *vfs_test_current = NULL;
int g_log_level = 0;
void _log_write(int level, const char *fmt, ...) { (void)level; (void)fmt; }
void _log_err_impl(const char *fmt, ...) { (void)fmt; }
void _log_warn_impl(const char *fmt, ...) { (void)fmt; }

static const vfs_inode_ops_t s_dummy_iops = {0};
static const vfs_file_ops_t  s_dummy_fops = {0};

int block_device_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{ (void)dev; (void)lba; (void)count; (void)buf; return -EIO; }
int block_device_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{ (void)dev; (void)lba; (void)count; (void)buf; return -EIO; }
uint64_t alloc_4k_page(void) { return (uint64_t)(uintptr_t)calloc(1, 4096); }
void free_4k_page(uint64_t addr) { free((void *)(uintptr_t)addr); }

static int host_procfs_mount(struct vfs_mount *mp, block_device_t *dev, void *data)
{
    (void)dev; (void)data;
    mp->i_ops = &s_dummy_iops;
    mp->f_ops = &s_dummy_fops;
    mp->root->i_ops = &s_dummy_iops;
    mp->root->f_ops = &s_dummy_fops;
    return 0;
}
static vfs_fs_type_t s_host_procfs_type = {
    .name     = "procfs",
    .fs_flags = VFS_FS_NO_DCACHE,
    .mount    = host_procfs_mount,
};
int procfs_register_fs(void) { return vfs_register_filesystem(&s_host_procfs_type); }

static int s_mock_mount_calls = 0;
static int s_mock_mount_ret = 0;
static int s_unreg_inside_mount_rc = 0;
static bool s_try_unreg_inside_mount = false;

static int mock_fs_mount(struct vfs_mount *mp, block_device_t *dev, void *data)
{
    (void)dev;
    s_mock_mount_calls++;
    if (s_try_unreg_inside_mount && mp && mp->fs_type) {
        s_unreg_inside_mount_rc = vfs_unregister_filesystem(mp->fs_type->name);
    }
    if (s_mock_mount_ret < 0) {
        return s_mock_mount_ret;
    }
    mp->i_ops = &s_dummy_iops;
    mp->f_ops = &s_dummy_fops;
    mp->fs_data = data;
    if (mp->root) {
        mp->root->i_ops = &s_dummy_iops;
        mp->root->f_ops = &s_dummy_fops;
    }
    return 0;
}

TEST_FUNC(test_fs_register_and_find) {
    vfs_init();

    vfs_fs_type_t custom = {
        .name = "customfs",
        .fs_flags = VFS_FS_NO_DCACHE,
        .mount = mock_fs_mount,
    };

    assert_eq(0, vfs_register_filesystem(&custom));
    const vfs_fs_type_t *found = vfs_find_filesystem("customfs");
    assert_true(found == &custom);
    assert_eq(VFS_FS_NO_DCACHE, found->fs_flags);

    assert_null(vfs_find_filesystem("nonexistent"));
    assert_null(vfs_find_filesystem(""));
    assert_null(vfs_find_filesystem(NULL));

    assert_eq(0, vfs_unregister_filesystem("customfs"));
    assert_null(vfs_find_filesystem("customfs"));
}

TEST_FUNC(test_fs_register_invalid_and_duplicate) {
    vfs_init();

    vfs_fs_type_t no_name = { .name = NULL, .mount = mock_fs_mount };
    vfs_fs_type_t empty_name = { .name = "", .mount = mock_fs_mount };
    vfs_fs_type_t no_mount = { .name = "nomount", .mount = NULL };
    vfs_fs_type_t valid1 = { .name = "dupfs", .mount = mock_fs_mount };
    vfs_fs_type_t valid2 = { .name = "dupfs", .mount = mock_fs_mount };

    assert_eq(-EINVAL, vfs_register_filesystem(NULL));
    assert_eq(-EINVAL, vfs_register_filesystem(&no_name));
    assert_eq(-EINVAL, vfs_register_filesystem(&empty_name));
    assert_eq(-EINVAL, vfs_register_filesystem(&no_mount));

    assert_eq(0, vfs_register_filesystem(&valid1));
    assert_eq(-EEXIST, vfs_register_filesystem(&valid2));
    assert_eq(-EINVAL, vfs_unregister_filesystem(NULL));
    assert_eq(-EINVAL, vfs_unregister_filesystem(""));
    assert_eq(-ENOENT, vfs_unregister_filesystem("missing_fs"));
}

TEST_FUNC(test_fs_mount_validation_and_rollback) {
    vfs_init();
    s_mock_mount_calls = 0;
    s_mock_mount_ret = 0;
    s_try_unreg_inside_mount = false;

    vfs_fs_type_t dev_fs = {
        .name = "mockblk",
        .fs_flags = VFS_FS_REQUIRES_DEV,
        .mount = mock_fs_mount,
    };
    assert_eq(0, vfs_register_filesystem(&dev_fs));

    /* Invalid args & relative path rejected before mount() callback */
    assert_eq(-EINVAL, vfs_mount_fs(NULL, "/mnt", NULL, NULL));
    assert_eq(-EINVAL, vfs_mount_fs("", "/mnt", NULL, NULL));
    assert_eq(-EINVAL, vfs_mount_fs("mockblk", NULL, NULL, NULL));
    assert_eq(-EINVAL, vfs_mount_fs("mockblk", "", NULL, NULL));
    assert_eq(-EINVAL, vfs_mount_fs("mockblk", "relative/path", NULL, NULL));
    assert_eq(-ENODEV, vfs_mount_fs("unknown_fs", "/mnt", NULL, NULL));

    /* REQUIRES_DEV with NULL block_device_t -> -EINVAL */
    assert_eq(-EINVAL, vfs_mount_fs("mockblk", "/mnt", NULL, NULL));
    assert_eq(0, s_mock_mount_calls);

    /* Failing mount() propagates -errno, does not publish mount, releases reservation */
    block_device_t fake_bdev;
    memset(&fake_bdev, 0, sizeof(fake_bdev));
    s_mock_mount_ret = -EIO;
    assert_eq(-EIO, vfs_mount_fs("mockblk", "/fail", &fake_bdev, NULL));
    assert_eq(1, s_mock_mount_calls);
    assert_null(vfs_lookup("/fail"));
    assert_eq(0, vfs_unregister_filesystem("mockblk"));
}

TEST_FUNC(test_fs_unregister_busy_and_in_progress) {
    vfs_init();
    s_mock_mount_calls = 0;
    s_mock_mount_ret = 0;
    s_try_unreg_inside_mount = true;
    s_unreg_inside_mount_rc = 0;

    vfs_fs_type_t mem_fs = {
        .name = "mockmem",
        .fs_flags = VFS_FS_CASE_INSENSITIVE,
        .mount = mock_fs_mount,
    };
    assert_eq(0, vfs_register_filesystem(&mem_fs));

    int tag = 42;
    assert_eq(0, vfs_mount_fs("mockmem", "/", NULL, &tag));
    /* Unregister attempted inside mount() must have seen -EBUSY */
    assert_eq(-EBUSY, s_unreg_inside_mount_rc);
    /* Unregister while mounted must also return -EBUSY */
    assert_eq(-EBUSY, vfs_unregister_filesystem("mockmem"));

    vfs_node_t *root = vfs_lookup("/");
    assert_not_null(root);
    assert_not_null(root->mount);
    assert_true(root->mount->fs_type == &mem_fs);
    assert_eq(VFS_FS_CASE_INSENSITIVE, root->mount->mnt_flags);
    assert_true(root->mount->fs_data == &tag);
    vfs_node_put(root);
}

TEST_FUNC(test_builtin_filesystems_registered_on_init) {
    /* Call vfs_init() twice to pin idempotent re-registration without cycle */
    vfs_init();
    vfs_init();

    const vfs_fs_type_t *devfs_t  = vfs_find_filesystem("devfs");
    const vfs_fs_type_t *ext2_t   = vfs_find_filesystem("ext2");
    const vfs_fs_type_t *vfat_t   = vfs_find_filesystem("vfat");
    const vfs_fs_type_t *tmpfs_t  = vfs_find_filesystem("tmpfs");
    const vfs_fs_type_t *procfs_t = vfs_find_filesystem("procfs");

    assert_not_null(devfs_t);
    assert_eq(VFS_FS_NO_DCACHE, devfs_t->fs_flags);
    assert_not_null(devfs_t->mount);

    assert_not_null(ext2_t);
    assert_eq(VFS_FS_REQUIRES_DEV, ext2_t->fs_flags);
    assert_not_null(ext2_t->mount);

    assert_not_null(vfat_t);
    assert_eq(VFS_FS_REQUIRES_DEV | VFS_FS_CASE_INSENSITIVE, vfat_t->fs_flags);
    assert_not_null(vfat_t->mount);

    assert_not_null(tmpfs_t);
    assert_eq(0u, tmpfs_t->fs_flags);
    assert_not_null(tmpfs_t->mount);

    assert_not_null(procfs_t);
    assert_eq(VFS_FS_NO_DCACHE, procfs_t->fs_flags);
    assert_not_null(procfs_t->mount);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_fs_register_and_find),
    TEST_ENTRY(test_fs_register_invalid_and_duplicate),
    TEST_ENTRY(test_fs_mount_validation_and_rollback),
    TEST_ENTRY(test_fs_unregister_busy_and_in_progress),
    TEST_ENTRY(test_builtin_filesystems_registered_on_init),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
