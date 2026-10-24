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

static void (*s_on_block_read)(block_device_t *dev, uint64_t lba) = NULL;

int block_device_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    if (s_on_block_read) s_on_block_read(dev, lba);
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

static void format_mock_ext2_disk(void)
{
    memset(s_ram_disk, 0, sizeof(s_ram_disk));

    // Superblock at byte 1024 (sector 2)
    ext2_superblock_t *sb = (ext2_superblock_t *)(s_ram_disk + 1024);
    sb->s_inodes_count = 32;
    sb->s_blocks_count = 64;
    sb->s_r_blocks_count = 0;
    sb->s_free_blocks_count = 55;
    sb->s_free_inodes_count = 30;
    sb->s_first_data_block = 0;
    sb->s_log_block_size = 2; // 4096 bytes per block
    sb->s_log_frag_size = 2;
    sb->s_blocks_per_group = 64;
    sb->s_frags_per_group = 64;
    sb->s_inodes_per_group = 32;
    sb->s_magic = EXT2_MAGIC;
    sb->s_state = 1;
    sb->s_errors = 1;
    sb->s_rev_level = 1;
    sb->s_first_ino = 11;
    sb->s_inode_size = 128;

    // Block Group Descriptor at block 1 (byte 4096)
    ext2_bgdesc_t *bg = (ext2_bgdesc_t *)(s_ram_disk + 4096);
    bg->bg_block_bitmap = 2;
    bg->bg_inode_bitmap = 3;
    bg->bg_inode_table = 4;
    bg->bg_free_blocks_count = 55;
    bg->bg_free_inodes_count = 30;
    bg->bg_used_dirs_count = 1;

    // Block bitmap at block 2 (byte 2 * 4096 = 8192)
    uint8_t *bbitmap = s_ram_disk + 2 * 4096;
    bbitmap[0] = 0xFF; // bits 0..7
    bbitmap[1] = 0x01; // bit 8

    // Inode bitmap at block 3 (byte 3 * 4096 = 12288)
    uint8_t *ibitmap = s_ram_disk + 3 * 4096;
    ibitmap[0] = 0x03; // bits 0..1 (inodes 1 and 2)

    // Inode table starts at block 4 (byte 4 * 4096 = 16384)
    ext2_inode_t *root_ino = (ext2_inode_t *)(s_ram_disk + 4 * 4096 + 128);
    root_ino->i_mode = EXT2_S_IFDIR | 0755;
    root_ino->i_size = 4096;
    root_ino->i_links_count = 2;
    root_ino->i_blocks = 8; // 8 sectors = 4096 bytes
    root_ino->i_block[0] = 8; // data block 8

    // Root directory content at block 8 (byte 8 * 4096 = 32768)
    uint8_t *root_dir = s_ram_disk + 8 * 4096;
    ext2_dirent_t *dot = (ext2_dirent_t *)root_dir;
    dot->inode = 2;
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = EXT2_FT_DIR;
    memcpy(dot->name, ".", 1);

    ext2_dirent_t *dotdot = (ext2_dirent_t *)(root_dir + 12);
    dotdot->inode = 2;
    dotdot->rec_len = 4096 - 12;
    dotdot->name_len = 2;
    dotdot->file_type = EXT2_FT_DIR;
    memcpy(dotdot->name, "..", 2);
}

void vfs_test_on_active_register(vfs_node_t *node)
{
    if (!node || !node->mount || !node->mount->fs_type) return;
    if (strcmp(node->mount->fs_type->name, "ext2") == 0) {
        ext2_fs_t *fs = (ext2_fs_t *)node->mount->fs_data;
        if (fs) {
            assert_eq(0, fs->lock.lock); // lock must be HELD (0)
        }
    } else if (strcmp(node->mount->fs_type->name, "vfat") == 0) {
        fat32_fs_t *fs = (fat32_fs_t *)node->mount->fs_data;
        if (fs) {
            assert_eq(0, fs->fat_lifecycle_lock.lock); // lock must be HELD (0)
        }
    }
}

static fat32_fs_t *s_test_fat_fs = NULL;
static void on_fat_block_read(block_device_t *dev, uint64_t lba)
{
    (void)dev; (void)lba;
    if (s_test_fat_fs) {
        assert_eq(0, s_test_fat_fs->fat_lifecycle_lock.lock);
    }
}

TEST_FUNC(test_tmpfs_orphan_lifecycle_and_evict) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));

    vfs_node_t *created = NULL;
    assert_eq(0, vfs_create_file("/tmpfile", "/", &created));
    assert_not_null(created);
    vfs_node_put(created);

    vfs_node_t *h1 = vfs_lookup("/tmpfile");
    vfs_node_t *h2 = vfs_lookup("/tmpfile");
    assert_not_null(h1);
    assert_not_null(h2);

    char *buf4k = malloc(4096);
    memset(buf4k, 'Z', 4096);
    assert_eq(4096, vfs_write(h1, 0, 4096, buf4k));

    assert_eq(0, vfs_unlink("/tmpfile", "/"));

    /* Verify /tmpfile lookup now returns -ENOENT */
    vfs_node_t *n_gone = NULL;
    assert_eq(-ENOENT, vfs_lookup_at(AT_FDCWD, "/tmpfile", LOOKUP_FOLLOW, &n_gone));
    assert_null(n_gone);

    /* h1 and h2 are marked orphaned */
    assert_true(h1->node_flags & VFS_NODE_ORPHANED);
    assert_true(h2->node_flags & VFS_NODE_ORPHANED);

    struct stat st1, st2;
    assert_eq(0, vfs_stat(h1, &st1));
    assert_eq(0, vfs_stat(h2, &st2));
    assert_eq(0, st1.st_nlink);
    assert_eq(0, st2.st_nlink);
    assert_eq(4096, st1.st_size);
    assert_eq(4096, st2.st_size);

    /* Read via h2 */
    char *read_buf = malloc(4096);
    assert_eq(4096, vfs_read(h2, 0, 4096, read_buf));
    assert_eq(0, memcmp(buf4k, read_buf, 4096));

    /* Write extends orphan file */
    char extra[100];
    memset(extra, 'X', 100);
    assert_eq(100, vfs_write(h1, 4096, 100, extra));
    assert_eq(0, vfs_stat(h2, &st2));
    assert_eq(4196, st2.st_size);

    free(read_buf);
    free(buf4k);

    vfs_node_put(h1);
    vfs_node_put(h2);
}

TEST_FUNC(test_ext2_orphan_deferred_reclaim_and_sole_reclaimer) {
    vfs_init();
    format_mock_ext2_disk();
    assert_eq(0, vfs_mount_fs("ext2", "/", &s_ram_block_dev, NULL));

    vfs_node_t *r = vfs_lookup("/");
    assert_not_null(r);
    vfs_mount_t *mp = r->mount;
    assert_not_null(mp);
    ext2_fs_t *fs = (ext2_fs_t *)mp->fs_data;
    assert_not_null(fs);
    vfs_node_put(r);
    assert_eq(0, fs->test_reclaim_count);

    /* Create /a.txt */
    vfs_node_t *h1 = NULL;
    assert_eq(0, vfs_create_file("/a.txt", "/", &h1));
    assert_not_null(h1);

    const char *data_old = "EXT2_OLD_DATA";
    assert_eq((int)strlen(data_old), vfs_write(h1, 0, strlen(data_old), (void *)data_old));

    vfs_node_t *h2 = vfs_lookup("/a.txt");
    assert_not_null(h2);
    assert_eq(h1->ino, h2->ino);

    /* Unlink /a.txt while h1 and h2 are open */
    assert_eq(0, vfs_unlink("/a.txt", "/"));
    assert_eq(0, fs->test_reclaim_count); // deferred!
    assert_true(h1->node_flags & VFS_NODE_ORPHANED);
    assert_true(h2->node_flags & VFS_NODE_ORPHANED);
    assert_eq(0, h1->nlink);
    assert_eq(0, h2->nlink);

    /* Create /b.txt - inode must NOT be reused */
    vfs_node_t *h_b = NULL;
    assert_eq(0, vfs_create_file("/b.txt", "/", &h_b));
    assert_not_null(h_b);
    assert_true(h_b->ino != h1->ino);

    const char *data_b = "EXT2_NEW_B";
    assert_eq((int)strlen(data_b), vfs_write(h_b, 0, strlen(data_b), (void *)data_b));

    const char *append = "_APPEND";
    assert_eq((int)strlen(append), vfs_write(h1, strlen(data_old), strlen(append), (void *)append));

    char r_buf[64] = {0};
    assert_eq((int)strlen(data_b), vfs_read(h_b, 0, sizeof(r_buf), r_buf));
    assert_str_eq("EXT2_NEW_B", r_buf);

    memset(r_buf, 0, sizeof(r_buf));
    assert_eq((int)(strlen(data_old) + strlen(append)), vfs_read(h2, 0, sizeof(r_buf), r_buf));
    assert_str_eq("EXT2_OLD_DATA_APPEND", r_buf);

    vfs_node_put(h_b);

    /* Drop h1: 1 handle left, reclaim_count remains 0 */
    vfs_node_put(h1);
    assert_eq(0, fs->test_reclaim_count);

    /* Drop h2: last handle, reclaim_count becomes 1 */
    vfs_node_put(h2);
    assert_eq(1, fs->test_reclaim_count);

    /* Zero-refcount pending-evict vs unlink race test */
    vfs_node_t *hc = NULL;
    assert_eq(0, vfs_create_file("/c.txt", "/", &hc));
    assert_not_null(hc);

    /* Simulate pending evict: refcount dropped to 0 before unlink */
    hc->refcount = 0;
    assert_eq(0, vfs_unlink("/c.txt", "/"));
    assert_eq(1, fs->test_reclaim_count); // unlink saw hc in active_nodes, did NOT reclaim!

    /* Now run evict_node on hc */
    hc->i_ops->evict_node(hc);
    assert_eq(2, fs->test_reclaim_count); // sole reclaimer executed!
    kfree(hc->name);
    kfree(hc);
}

TEST_FUNC(test_fat32_shared_file_object_and_orphan_reclaim) {
    vfs_init();
    format_mock_fat32_disk();
    assert_eq(0, vfs_mount_fs("vfat", "/", &s_ram_block_dev, NULL));

    vfs_node_t *r = vfs_lookup("/");
    assert_not_null(r);
    vfs_mount_t *mp = r->mount;
    assert_not_null(mp);
    fat32_fs_t *fs = (fat32_fs_t *)mp->fs_data;
    assert_not_null(fs);
    vfs_node_put(r);
    s_test_fat_fs = fs;
    assert_eq(0, fs->test_reclaim_count);

    /* Create two empty files /e1.txt and /e2.txt */
    vfs_node_t *e1 = NULL;
    assert_eq(0, vfs_create_file("/e1.txt", "/", &e1));
    assert_not_null(e1);
    assert_true(e1->ino >= FAT_OBJECT_ID_BASE);

    vfs_node_t *e2 = NULL;
    assert_eq(0, vfs_create_file("/e2.txt", "/", &e2));
    assert_not_null(e2);
    assert_true(e2->ino >= FAT_OBJECT_ID_BASE);
    assert_true(e1->ino != e2->ino);

    /* Open second handle e1_dup */
    vfs_node_t *e1_dup = vfs_lookup("/e1.txt");
    assert_not_null(e1_dup);
    assert_eq(e1->ino, e1_dup->ino);
    assert_eq(e1->fs_data, e1_dup->fs_data);

    /* Write "HELLO_FAT" via e1 */
    const char *msg = "HELLO_FAT";
    assert_eq(9, vfs_write(e1, 0, 9, (void *)msg));

    struct stat st_dup;
    assert_eq(0, vfs_stat(e1_dup, &st_dup));
    assert_eq(9, st_dup.st_size);

    /* Read via e1_dup with s_on_block_read asserting fat_lifecycle_lock held */
    s_on_block_read = on_fat_block_read;
    char r_fat[32] = {0};
    assert_eq(9, vfs_read(e1_dup, 0, sizeof(r_fat), r_fat));
    assert_str_eq("HELLO_FAT", r_fat);
    s_on_block_read = NULL;

    /* Truncate e1 to 0 */
    assert_eq(0, vfs_truncate(e1, 0));
    fat_file_object_t *e1_obj = (fat_file_object_t *)e1->fs_data;
    assert_not_null(e1_obj);
    assert_eq(0, e1_obj->first_cluster);
    assert_eq(0, e1_obj->size);
    assert_eq(0, vfs_read(e1_dup, 0, sizeof(r_fat), r_fat));

    /* Unlink /e1.txt while e1 and e1_dup are open (0-byte empty orphan) */
    assert_eq(0, vfs_unlink("/e1.txt", "/"));
    assert_true(e1->node_flags & VFS_NODE_ORPHANED);
    assert_true(e1_dup->node_flags & VFS_NODE_ORPHANED);

    /* Create /reuse.txt which can reuse the deleted dirent slot */
    vfs_node_t *r_node = NULL;
    assert_eq(0, vfs_create_file("/reuse.txt", "/", &r_node));
    assert_not_null(r_node);
    assert_true(r_node->ino != e1->ino);

    /* Write "ORPHAN_WRITE" to unlinked empty e1 */
    const char *orph_data = "ORPHAN_WRITE";
    assert_eq(12, vfs_write(e1, 0, 12, (void *)orph_data));

    memset(r_fat, 0, sizeof(r_fat));
    assert_eq(12, vfs_read(e1_dup, 0, sizeof(r_fat), r_fat));
    assert_str_eq("ORPHAN_WRITE", r_fat);

    struct stat st_reuse;
    assert_eq(0, vfs_stat(r_node, &st_reuse));
    assert_eq(0, st_reuse.st_size);

    vfs_node_put(r_node);

    /* Drop e1 (reclaim_count still 0) */
    vfs_node_put(e1);
    assert_eq(0, fs->test_reclaim_count);

    /* Drop e1_dup (reclaims cluster chain) */
    vfs_node_put(e1_dup);
    assert_eq(1, fs->test_reclaim_count);

    /* Test rename /e2.txt -> /renamed.txt while e2 open */
    assert_eq(0, vfs_rename("/e2.txt", "/renamed.txt", "/"));
    const char *after_rename = "AFTER_RENAME";
    assert_eq(12, vfs_write(e2, 0, 12, (void *)after_rename));

    vfs_node_t *fresh_renamed = vfs_lookup("/renamed.txt");
    assert_not_null(fresh_renamed);
    struct stat st_ren;
    assert_eq(0, vfs_stat(fresh_renamed, &st_ren));
    assert_eq(12, st_ren.st_size);

    memset(r_fat, 0, sizeof(r_fat));
    assert_eq(12, vfs_read(fresh_renamed, 0, sizeof(r_fat), r_fat));
    assert_str_eq("AFTER_RENAME", r_fat);

    vfs_node_put(fresh_renamed);
    vfs_node_put(e2);

    s_test_fat_fs = NULL;
}

TEST_FUNC(test_ext2_disk_inode_metadata_stat) {
    vfs_init();
    format_mock_ext2_disk();
    assert_eq(0, vfs_mount_fs("ext2", "/", &s_ram_block_dev, NULL));

    /* Create file /meta.txt */
    vfs_node_t *node = NULL;
    assert_eq(0, vfs_create_file("/meta.txt", "/", &node));
    assert_not_null(node);
    uint32_t ino = (uint32_t)node->ino;
    vfs_node_put(node);

    /* Modify on-disk inode metadata directly */
    ext2_inode_t *d_ino = (ext2_inode_t *)(s_ram_disk + 4 * 4096 + (ino - 1) * 128);
    d_ino->i_uid   = 1000;
    d_ino->i_gid   = 2000;
    d_ino->i_mode  = EXT2_S_IFREG | 0640;
    d_ino->i_atime = 111;
    d_ino->i_mtime = 222;
    d_ino->i_ctime = 333;

    /* Fresh lookup to populate from disk inode */
    vfs_node_t *target = vfs_lookup("/meta.txt");
    assert_not_null(target);

    struct stat st;
    assert_eq(0, vfs_stat(target, &st));
    assert_eq(1000, st.st_uid);
    assert_eq(2000, st.st_gid);
    assert_eq((uint32_t)(S_IFREG | 0640), (uint32_t)st.st_mode);
    assert_eq(111, st.st_atime);
    assert_eq(222, st.st_mtime);
    assert_eq(333, st.st_ctime);
    assert_eq(4096, st.st_blksize);
    assert_eq(d_ino->i_blocks, st.st_blocks);

    vfs_node_put(target);
}

TEST_FUNC(test_fat32_rmdir_open_dir_deferred_reclaim) {
    vfs_init();
    format_mock_fat32_disk();
    assert_eq(0, vfs_mount_fs("vfat", "/", &s_ram_block_dev, NULL));

    vfs_node_t *r = vfs_lookup("/");
    assert_not_null(r);
    vfs_mount_t *mp = r->mount;
    assert_not_null(mp);
    fat32_fs_t *fs = (fat32_fs_t *)mp->fs_data;
    assert_not_null(fs);
    vfs_node_put(r);
    assert_eq(0, fs->test_reclaim_count);

    /* Create empty directory /sub */
    assert_eq(0, vfs_mkdir("/sub", "/"));

    /* Lookup and hold reference to /sub */
    vfs_node_t *sub = vfs_lookup("/sub");
    assert_not_null(sub);
    assert_eq(VFS_DIR, sub->type);
    assert_eq(0, fs->test_reclaim_count);

    /* Call vfs_rmdir on /sub while sub node is open */
    assert_eq(0, vfs_rmdir("/sub", "/"));
    /* Deferred: cluster chain NOT yet reclaimed because sub is active */
    assert_eq(0, fs->test_reclaim_count);
    assert_true(sub->node_flags & VFS_NODE_ORPHANED);
    assert_eq(0, sub->nlink);

    /* Dropping sub triggers evict_node which reclaims the cluster chain */
    vfs_node_put(sub);
    assert_eq(1, fs->test_reclaim_count);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_lookup_300_entries_tmpfs),
    TEST_ENTRY(test_devfs_and_tmpfs_metadata_and_getattr),
    TEST_ENTRY(test_write_and_truncate_stat_consistency),
    TEST_ENTRY(test_erofs_eopnotsupp_and_exdev),
    TEST_ENTRY(test_tmpfs_orphan_lifecycle_and_evict),
    TEST_ENTRY(test_ext2_orphan_deferred_reclaim_and_sole_reclaimer),
    TEST_ENTRY(test_fat32_shared_file_object_and_orphan_reclaim),
    TEST_ENTRY(test_ext2_disk_inode_metadata_stat),
    TEST_ENTRY(test_fat32_rmdir_open_dir_deferred_reclaim),
TEST_LIST_END

int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
