#include "test_framework.h"
#include <fs/vfs.h>
#include <fs/devfs.h>
#include <fs/tmpfs.h>
#include <fs/ext2.h>
#include <fs/fat.h>
#include <block/blockdev.h>
#include <errno.h>
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
{ (void)dev; (void)lba; (void)count; (void)buf; return -EIO; }
int block_device_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{ (void)dev; (void)lba; (void)count; (void)buf; return -EIO; }

uint64_t alloc_4k_page(void) { return (uint64_t)(uintptr_t)calloc(1, 4096); }
void free_4k_page(uint64_t addr) { free((void *)(uintptr_t)addr); }

void get_random_bytes(void *b, size_t n) { if (b && n) memset(b, 0x5A, n); }
void random_add_entropy(const void *d, size_t n) { (void)d; (void)n; }
int  user_write_range_begin(uint64_t a, size_t l) { (void)a; (void)l; return 0; }
void user_write_range_end(void) {}
int  user_read_range_begin(uint64_t a, size_t l) { (void)a; (void)l; return 0; }
void user_read_range_end(void) {}
void *keyboard_get_tty(void) { return NULL; }
char read_serial(void) { return 0; }
void write_serial(char c) { (void)c; }
tty_t *get_dev_tty(void) { return NULL; }

static const vfs_inode_ops_t s_ro_proc_iops = {0};
static const vfs_file_ops_t  s_ro_proc_fops = {0};
static int host_procfs_mount(struct vfs_mount *mp, block_device_t *dev, void *data)
{
    (void)dev; (void)data;
    mp->i_ops = &s_ro_proc_iops;
    mp->f_ops = &s_ro_proc_fops;
    mp->root->i_ops = &s_ro_proc_iops;
    mp->root->f_ops = &s_ro_proc_fops;
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

TEST_FUNC(test_lookup_300_entries_tmpfs) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));

    for (int i = 0; i < 300; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/file_%03d", i);
        vfs_node_t *created = NULL;
        assert_eq(0, vfs_create_file(path, "/", &created));
        assert_not_null(created);
        vfs_node_put(created);
    }

    /* Lookup the 300th entry (index 299, well past the old 256 readdir cap) */
    vfs_node_t *n299 = NULL;
    assert_eq(0, vfs_lookup_at(AT_FDCWD, "/file_299", LOOKUP_FOLLOW, &n299));
    assert_not_null(n299);
    assert_str_eq("file_299", n299->name);
    assert_true(vfs_node_stable_ino(n299) != 0);
    vfs_node_put(n299);
}

TEST_FUNC(test_devfs_and_tmpfs_metadata_and_getattr) {
    vfs_init();
    devfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));

    /* Check /dev/null metadata populated by devfs_iop_lookup */
    vfs_node_t *nul = vfs_lookup("/dev/null");
    assert_not_null(nul);
    assert_not_null(nul->i_ops);
    assert_not_null(nul->i_ops->lookup);

    struct stat st;
    assert_eq(0, vfs_stat(nul, &st));
    assert_eq((uint32_t)(S_IFCHR | 0666), (uint32_t)st.st_mode);
    assert_true(st.st_rdev > 0);
    assert_eq(st.st_ino, st.st_rdev);
    assert_eq(1, st.st_nlink);
    vfs_node_put(nul);

    /* Verify vfs_generic_getattr preserves nlink == 0 for orphaned nodes */
    vfs_node_t orphan = {0};
    orphan.type = VFS_FILE;
    orphan.ino = 999;
    orphan.size = 4096;
    orphan.mode = S_IFREG | 0644;
    orphan.nlink = 0;
    orphan.node_flags = VFS_NODE_ORPHANED;
    assert_eq(0, vfs_generic_getattr(&orphan, &st));
    assert_eq(0, st.st_nlink);
    assert_eq(999, st.st_ino);
    assert_eq((uint32_t)(S_IFREG | 0644), (uint32_t)st.st_mode);
}

TEST_FUNC(test_write_and_truncate_stat_consistency) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));

    vfs_node_t *created = NULL;
    assert_eq(0, vfs_create_file("/consistency.bin", "/", &created));
    assert_not_null(created);
    vfs_node_put(created);

    vfs_node_t *h1 = vfs_lookup("/consistency.bin");
    vfs_node_t *h2 = vfs_lookup("/consistency.bin");
    assert_not_null(h1);
    assert_not_null(h2);

    char payload[128];
    memset(payload, 'A', sizeof(payload));
    assert_eq(128, vfs_write(h1, 0, sizeof(payload), payload));

    struct stat st1, st2, st3;
    assert_eq(0, vfs_stat(h1, &st1));
    assert_eq(0, vfs_stat(h2, &st2));
    vfs_node_t *h3 = vfs_lookup("/consistency.bin");
    assert_not_null(h3);
    assert_eq(0, vfs_stat(h3, &st3));

    assert_eq(128, st1.st_size);
    assert_eq(128, st2.st_size);
    assert_eq(128, st3.st_size);
    vfs_node_put(h3);

    /* Truncate via h1 and verify h2 + fresh lookup see new size 32 */
    assert_eq(0, vfs_truncate(h1, 32));
    assert_eq(0, vfs_stat(h1, &st1));
    assert_eq(0, vfs_stat(h2, &st2));
    vfs_node_t *h4 = vfs_lookup("/consistency.bin");
    assert_not_null(h4);
    assert_eq(0, vfs_stat(h4, &st3));

    assert_eq(32, st1.st_size);
    assert_eq(32, st2.st_size);
    assert_eq(32, st3.st_size);

    vfs_node_put(h4);
    vfs_node_put(h2);
    vfs_node_put(h1);
}

TEST_FUNC(test_erofs_eopnotsupp_and_exdev) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));
    assert_eq(0, vfs_mount_fs("procfs", "/proc", NULL, NULL));

    vfs_node_t *out = NULL;
    assert_eq(-EROFS, vfs_create_file("/proc/file", "/", &out));
    assert_null(out);
    assert_eq(-EROFS, vfs_mkdir("/proc/dir", "/"));
    assert_eq(-EROFS, vfs_unlink("/proc/meminfo", "/"));
    assert_eq(-EROFS, vfs_rmdir("/proc/dir", "/"));
    assert_eq(-EOPNOTSUPP, vfs_symlink("/target", "/proc/link", "/"));

    vfs_node_t *f = NULL;
    assert_eq(0, vfs_create_file("/a.txt", "/", &f));
    vfs_node_put(f);
    assert_eq(-EXDEV, vfs_rename("/a.txt", "/proc/a.txt", "/"));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_lookup_300_entries_tmpfs),
    TEST_ENTRY(test_devfs_and_tmpfs_metadata_and_getattr),
    TEST_ENTRY(test_write_and_truncate_stat_consistency),
    TEST_ENTRY(test_erofs_eopnotsupp_and_exdev),
TEST_LIST_END

int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
