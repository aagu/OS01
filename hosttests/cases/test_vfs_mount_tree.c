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
{
    if (!dev || !dev->ops || !dev->ops->read) return -EIO;
    return dev->ops->read(dev, lba, count, buf);
}

int block_device_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    if (!dev || !dev->ops || !dev->ops->write) return -EIO;
    return dev->ops->write(dev, lba, count, buf);
}

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

#define RAM_DISK_SECTOR_SIZE 512
#define RAM_DISK_SECTOR_COUNT 1024
static uint8_t s_ram_disk[RAM_DISK_SECTOR_SIZE * RAM_DISK_SECTOR_COUNT];

static int ram_disk_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    (void)dev;
    if (lba + count > RAM_DISK_SECTOR_COUNT) return -EIO;
    memcpy(buf, s_ram_disk + lba * RAM_DISK_SECTOR_SIZE, count * RAM_DISK_SECTOR_SIZE);
    return 0;
}

static int ram_disk_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    (void)dev;
    if (lba + count > RAM_DISK_SECTOR_COUNT) return -EIO;
    memcpy(s_ram_disk + lba * RAM_DISK_SECTOR_SIZE, buf, count * RAM_DISK_SECTOR_SIZE);
    return 0;
}

static const struct block_device_ops s_ram_disk_ops = {
    .read = ram_disk_read,
    .write = ram_disk_write,
};

static block_device_t s_ram_block_dev = {
    .name = "ram0",
    .sector_count = RAM_DISK_SECTOR_COUNT,
    .sector_size = RAM_DISK_SECTOR_SIZE,
    .present = 1,
    .kind = BLOCK_DISK,
    .ops = &s_ram_disk_ops,
};

static void format_mock_fat32_disk(void)
{
    memset(s_ram_disk, 0, sizeof(s_ram_disk));
    FAT32_BPB *bpb = (FAT32_BPB *)s_ram_disk;

    bpb->bytes_per_sector = 512;
    bpb->sectors_per_cluster = 1;
    bpb->reserved_sectors = 32;
    bpb->num_fats = 2;
    bpb->sectors_per_fat_32 = 16;
    bpb->root_cluster = 2;
    bpb->total_sectors_32 = 256;
    bpb->media = 0xF8;
    bpb->sectors_per_fat_16 = 0;

    s_ram_disk[510] = 0x55;
    s_ram_disk[511] = 0xAA;

    uint32_t fat_start = bpb->reserved_sectors;
    uint32_t *fat1 = (uint32_t *)(s_ram_disk + fat_start * 512);
    fat1[0] = 0x0FFFFFF8;
    fat1[1] = 0x0FFFFFFF;
    fat1[2] = FAT32_EOC_MIN;

    uint32_t *fat2 = (uint32_t *)(s_ram_disk + (fat_start + bpb->sectors_per_fat_32) * 512);
    fat2[0] = 0x0FFFFFF8;
    fat2[1] = 0x0FFFFFFF;
    fat2[2] = FAT32_EOC_MIN;
}

TEST_FUNC(test_submount_downward_and_dotdot_upward_crossing) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));
    assert_eq(0, vfs_mkdir("/mnt", "/"));

    /* Mount tmpfs at /mnt/usb without pre-creating /mnt/usb (stub mountpoint) */
    assert_eq(0, vfs_mount_fs("tmpfs", "/mnt/usb", NULL, NULL));

    vfs_node_t *root = vfs_lookup("/");
    vfs_node_t *usb_root = vfs_lookup("/mnt/usb");
    assert_not_null(root);
    assert_not_null(usb_root);
    assert_true(root->mount != usb_root->mount);
    assert_not_null(usb_root->mount->mountpoint);
    assert_true(usb_root->mount->mountpoint->node_flags & VFS_NODE_MOUNT_STUB);

    /* Create file inside submount and read it back via fresh path walk */
    vfs_node_t *f = NULL;
    assert_eq(0, vfs_create_file("/mnt/usb/hello.txt", "/", &f));
    assert_not_null(f);
    assert_eq(11, vfs_write(f, 0, 11, "SUBMOUNT_OK"));

    char resolved[128] = {0};
    assert_true(vfs_resolve_path(f, resolved, sizeof(resolved)) > 0);
    assert_str_eq("/mnt/usb/hello.txt", resolved);
    vfs_node_put(f);

    vfs_node_t *f2 = vfs_lookup("/mnt/usb/hello.txt");
    assert_not_null(f2);
    char buf[32] = {0};
    assert_eq(11, vfs_read(f2, 0, sizeof(buf), buf));
    assert_str_eq("SUBMOUNT_OK", buf);
    vfs_node_put(f2);

    /* Upward ".." crossing from submount root back to /mnt and / */
    vfs_node_t *up_mnt = vfs_lookup("/mnt/usb/..");
    assert_not_null(up_mnt);
    assert_true(up_mnt->mount == root->mount);
    assert_str_eq("mnt", up_mnt->name);
    vfs_node_put(up_mnt);

    vfs_node_t *up_root = vfs_lookup("/mnt/usb/../..");
    assert_not_null(up_root);
    assert_true(up_root == root);
    vfs_node_put(up_root);

    vfs_node_put(usb_root);
    vfs_node_put(root);
}

TEST_FUNC(test_fresh_parent_lookup_and_case_insensitive_vfat_parent) {
    vfs_init();
    format_mock_fat32_disk();
    assert_eq(0, vfs_mount_fs("vfat", "/", &s_ram_block_dev, NULL));
    assert_eq(0, vfs_mkdir("/MNT", "/"));

    /* Mount tmpfs on /mnt/usb (parent is case-insensitive vfat) */
    assert_eq(0, vfs_mount_fs("tmpfs", "/mnt/usb", NULL, NULL));

    vfs_node_t *f = NULL;
    assert_eq(0, vfs_create_file("/mnt/usb/case.txt", "/", &f));
    assert_not_null(f);
    assert_eq(4, vfs_write(f, 0, 4, "VFAT"));
    vfs_node_put(f);

    /* Re-lookup with mixed case on both parent and submount name */
    vfs_node_t *l1 = vfs_lookup("/mnt/USB/case.txt");
    assert_not_null(l1);
    vfs_node_put(l1);

    vfs_node_t *l2 = vfs_lookup("/MNT/usb/case.txt");
    assert_not_null(l2);
    vfs_node_put(l2);

    vfs_node_t *l3 = vfs_lookup("/MNT/USB/..");
    assert_not_null(l3);
    assert_eq(0, strcasecmp(l3->name, "MNT"));
    vfs_node_put(l3);
}

static int count_dirent_name(vfs_node_t *dir, const char *target)
{
    uint8_t raw[2048];
    uint64_t pos = 0;
    int matches = 0;

    for (;;) {
        int n = vfs_getdents(dir, (struct linux_dirent64 *)raw, sizeof(raw), &pos);
        assert_true(n >= 0);
        if (n == 0) break;
        int off = 0;
        while (off < n) {
            struct linux_dirent64 *de = (struct linux_dirent64 *)(raw + off);
            if (strcmp(de->d_name, target) == 0) {
                matches++;
            }
            off += de->d_reclen;
        }
    }
    return matches;
}

TEST_FUNC(test_getdents_submount_injection_and_deduplication) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));
    assert_eq(0, vfs_mkdir("/mnt", "/"));
    assert_eq(0, vfs_mkdir("/mnt/existing_dir", "/"));

    /* /mnt/usb is stub-backed; /mnt/existing_dir already exists in /mnt */
    assert_eq(0, vfs_mount_fs("tmpfs", "/mnt/usb", NULL, NULL));
    assert_eq(0, vfs_mount_fs("tmpfs", "/mnt/existing_dir", NULL, NULL));

    vfs_node_t *root = vfs_lookup("/");
    assert_not_null(root);
    assert_eq(1, count_dirent_name(root, "mnt"));
    assert_eq(0, count_dirent_name(root, "usb"));
    assert_eq(0, count_dirent_name(root, "existing_dir"));
    vfs_node_put(root);

    vfs_node_t *mnt = vfs_lookup("/mnt");
    assert_not_null(mnt);
    assert_eq(1, count_dirent_name(mnt, "usb"));
    assert_eq(1, count_dirent_name(mnt, "existing_dir"));
    vfs_node_put(mnt);
}

TEST_FUNC(test_mount_protection_ebusy_and_mount_validation) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));
    assert_eq(0, vfs_mkdir("/a", "/"));
    assert_eq(0, vfs_mkdir("/a/d", "/"));
    assert_eq(0, vfs_mkdir("/b", "/"));

    vfs_node_t *reg = NULL;
    assert_eq(0, vfs_create_file("/regular.txt", "/", &reg));
    vfs_node_put(reg);

    /* Mount validation errors */
    assert_eq(-EBUSY, vfs_mount_fs("tmpfs", "/", NULL, NULL));
    assert_eq(-ENOENT, vfs_mount_fs("tmpfs", "/missing_parent/sub", NULL, NULL));
    assert_eq(-ENOTDIR, vfs_mount_fs("tmpfs", "/regular.txt/sub", NULL, NULL));
    assert_eq(-ENOTDIR, vfs_mount_fs("tmpfs", "/regular.txt", NULL, NULL));

    /* Mount at /a/d/usb and verify duplicate mount returns -EBUSY */
    assert_eq(0, vfs_mount_fs("tmpfs", "/a/d/usb", NULL, NULL));
    assert_eq(-EBUSY, vfs_mount_fs("tmpfs", "/a/d/usb", NULL, NULL));

    /* Renaming or removing mountpoint or any ancestor must return -EBUSY */
    assert_eq(-EBUSY, vfs_rename("/a/d", "/b/d", "/"));
    assert_eq(-EBUSY, vfs_rename("/a", "/b/a", "/"));
    assert_eq(-EBUSY, vfs_rename("/a/d/usb", "/a/d/usb2", "/"));
    assert_eq(-EBUSY, vfs_rmdir("/a/d", "/"));
    assert_eq(-EBUSY, vfs_rmdir("/a/d/usb", "/"));

    /* Ancestor chain must remain intact after rejected renames */
    vfs_node_t *anc_a = vfs_lookup("/a/d/usb/../..");
    assert_not_null(anc_a);
    assert_str_eq("a", anc_a->name);
    vfs_node_put(anc_a);
}

static int root_with_file_mount(struct vfs_mount *mp, block_device_t *dev, void *data)
{
    const vfs_fs_type_t *t = vfs_find_filesystem("tmpfs");
    if (!t || !t->mount) return -ENODEV;
    int rc = t->mount(mp, dev, data);
    if (rc < 0) return rc;
    vfs_node_t *f = mp->root->i_ops->create(mp->root, "conflict_file");
    if (f) {
        vfs_node_put(f);
    }
    return 0;
}

static vfs_fs_type_t s_root_conflict_fs = {
    .name = "root_conflict_tmpfs",
    .fs_flags = 0,
    .mount = root_with_file_mount,
};

TEST_FUNC(test_pre_root_pending_submounts_depth_order_and_rollback) {
    vfs_init();

    /* Queue pending submounts out of depth order before "/" is mounted */
    devfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/mnt/deep/sub", NULL, NULL));
    assert_eq(0, vfs_mount_fs("tmpfs", "/mnt", NULL, NULL));
    assert_eq(0, vfs_mount_fs("tmpfs", "/mnt/deep", NULL, NULL));

    /* Early pre-root lookup on /dev works before "/" is mounted */
    vfs_node_t *early_null = vfs_lookup("/dev/null");
    assert_not_null(early_null);
    vfs_node_put(early_null);

    /* Mount "/" -> binds all pending submounts in depth order */
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));

    vfs_node_t *deep_parent = vfs_lookup("/mnt/deep/sub/..");
    assert_not_null(deep_parent);
    vfs_node_put(deep_parent);

    vfs_node_t *mnt_root = vfs_lookup("/mnt/deep/sub/../..");
    assert_not_null(mnt_root);
    vfs_node_put(mnt_root);

    vfs_node_t *global_root = vfs_lookup("/mnt/deep/sub/../../..");
    assert_not_null(global_root);
    vfs_node_t *expected_root = vfs_lookup("/");
    assert_true(global_root == expected_root);

    vfs_node_put(expected_root);
    vfs_node_put(global_root);

    /* Pending submount rollback when root contains a non-dir at pending mountpoint */
    vfs_init();
    assert_eq(0, vfs_register_filesystem(&s_root_conflict_fs));
    assert_eq(0, vfs_mount_fs("tmpfs", "/conflict_file", NULL, NULL));
    assert_eq(-ENOTDIR, vfs_mount_fs("root_conflict_tmpfs", "/", NULL, NULL));
    assert_null(vfs_lookup("/"));
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_submount_downward_and_dotdot_upward_crossing),
    TEST_ENTRY(test_fresh_parent_lookup_and_case_insensitive_vfat_parent),
    TEST_ENTRY(test_getdents_submount_injection_and_deduplication),
    TEST_ENTRY(test_mount_protection_ebusy_and_mount_validation),
    TEST_ENTRY(test_pre_root_pending_submounts_depth_order_and_rollback),
TEST_LIST_END

int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
