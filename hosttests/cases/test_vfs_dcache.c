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

static int s_proc_lookup_calls = 0;
static int host_proc_lookup(struct vfs_node *dir, const char *name, struct vfs_node **out)
{
    s_proc_lookup_calls++;
    if (strcmp(name, "meminfo") != 0) return -ENOENT;
    vfs_node_t *n = (vfs_node_t *)calloc(1, sizeof(vfs_node_t));
    n->name = strdup("meminfo");
    n->type = VFS_FILE;
    n->ino = 100;
    n->mode = S_IFREG | 0444;
    n->nlink = 1;
    n->mount = dir->mount;
    n->parent = vfs_node_get(dir);
    n->i_ops = dir->i_ops;
    n->f_ops = dir->f_ops;
    n->refcount = 1;
    vfs_active_node_register(n);
    *out = n;
    return 0;
}

static const vfs_inode_ops_t s_ro_proc_iops = {
    .lookup = host_proc_lookup,
};
static const vfs_file_ops_t s_ro_proc_fops = {0};
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

TEST_FUNC(test_dcache_hit_miss_and_case_insensitive_vfat) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));

    vfs_node_t *f = NULL;
    assert_eq(0, vfs_create_file("/alpha.txt", "/", &f));
    vfs_node_put(f);

    uint64_t hits = 0, misses = 0, evictions = 0;
    dcache_get_stats(&hits, &misses, &evictions);
    assert_eq(0, hits);

    /* First lookup misses dcache and inserts /alpha.txt */
    vfs_node_t *n1 = vfs_lookup("/alpha.txt");
    assert_not_null(n1);
    dcache_get_stats(&hits, &misses, &evictions);
    assert_eq(0, hits);
    assert_true(misses >= 1);

    /* Second lookup hits dcache and returns the exact same cached vfs_node_t */
    uint64_t prev_misses = misses;
    vfs_node_t *n2 = vfs_lookup("/alpha.txt");
    assert_not_null(n2);
    assert_true(n1 == n2);
    dcache_get_stats(&hits, &misses, &evictions);
    assert_eq(1, hits);
    assert_eq(prev_misses, misses);
    vfs_node_put(n2);
    vfs_node_put(n1);

    /* Case-insensitive vfat mount must hit dcache across case variants */
    vfs_init();
    format_mock_fat32_disk();
    assert_eq(0, vfs_mount_fs("vfat", "/", &s_ram_block_dev, NULL));

    vfs_node_t *vf = NULL;
    assert_eq(0, vfs_create_file("/README.TXT", "/", &vf));
    vfs_node_put(vf);

    vfs_node_t *v1 = vfs_lookup("/README.TXT");
    assert_not_null(v1);
    dcache_get_stats(&hits, &misses, &evictions);
    uint64_t hits_after_v1 = hits;

    vfs_node_t *v2 = vfs_lookup("/readme.txt");
    assert_not_null(v2);
    assert_true(v1 == v2);
    dcache_get_stats(&hits, &misses, &evictions);
    assert_eq(hits_after_v1 + 1, hits);

    vfs_node_t *v3 = vfs_lookup("/ReAdMe.TxT");
    assert_not_null(v3);
    assert_true(v1 == v3);
    dcache_get_stats(&hits, &misses, &evictions);
    assert_eq(hits_after_v1 + 2, hits);

    vfs_node_put(v3);
    vfs_node_put(v2);
    vfs_node_put(v1);
}

TEST_FUNC(test_dcache_lru_256_capacity_eviction_and_submount_parent_rebuild) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));
    assert_eq(0, vfs_mkdir("/mnt", "/"));
    assert_eq(0, vfs_mkdir("/mnt/usb", "/"));
    assert_eq(0, vfs_mount_fs("tmpfs", "/mnt/usb", NULL, NULL));

    vfs_node_t *sub_f = NULL;
    assert_eq(0, vfs_create_file("/mnt/usb/payload.txt", "/", &sub_f));
    assert_eq(7, vfs_write(sub_f, 0, 7, "USB_256"));
    vfs_node_put(sub_f);

    /* Create and lookup 300 files in "/" to overflow the 256-entry dcache */
    vfs_node_t *first_cached = NULL;
    for (int i = 0; i < 300; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/lru_%03d", i);
        vfs_node_t *created = NULL;
        assert_eq(0, vfs_create_file(path, "/", &created));
        vfs_node_put(created);

        vfs_node_t *looked = vfs_lookup(path);
        assert_not_null(looked);
        if (i == 0) {
            /* Keep an extra ref on /lru_000 to verify refcount balance after LRU eviction */
            first_cached = vfs_node_get(looked);
            assert_eq(3u, first_cached->refcount); /* dcache(1) + looked(1) + first_cached(1) */
        }
        vfs_node_put(looked);
    }

    uint64_t hits = 0, misses = 0, evictions = 0;
    dcache_get_stats(&hits, &misses, &evictions);
    assert_true(evictions >= 44);

    /* /lru_000 was evicted from dcache, so only first_cached's ref (1) remains */
    assert_eq(1u, first_cached->refcount);
    vfs_node_put(first_cached);

    /* /mnt was also evicted from dcache; looking up /mnt/usb/payload.txt must rebuild /mnt and cross submount */
    vfs_node_t *re_usb = vfs_lookup("/mnt/usb/payload.txt");
    assert_not_null(re_usb);
    char buf[16] = {0};
    assert_eq(7, vfs_read(re_usb, 0, sizeof(buf), buf));
    assert_str_eq("USB_256", buf);
    vfs_node_put(re_usb);
}

TEST_FUNC(test_dcache_concurrent_lookup_unlink_generation_barrier) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));

    vfs_node_t *created = NULL;
    assert_eq(0, vfs_create_file("/race.txt", "/", &created));
    assert_eq(9, vfs_write(created, 0, 9, "RACE_DATA"));
    vfs_node_put(created);

    vfs_node_t *root = vfs_lookup("/");
    assert_not_null(root);

    /* Step 1: CPU A starts lookup, misses dcache, and obtains lookup_gen */
    uint64_t lookup_gen = 0;
    vfs_node_t *cached = dcache_lookup_begin(root, "race.txt", &lookup_gen);
    assert_null(cached);
    assert_true(lookup_gen != 0);

    /* Step 2: CPU A completes backend i_ops->lookup and registers child */
    vfs_node_t *child_a = NULL;
    assert_eq(0, root->i_ops->lookup(root, "race.txt", &child_a));
    assert_not_null(child_a);
    vfs_active_node_register(child_a);

    /* Step 3: CPU B unlinks /race.txt before CPU A calls dcache_insert_validated */
    assert_eq(0, vfs_unlink("/race.txt", "/"));
    assert_true(child_a->node_flags & VFS_NODE_ORPHANED);

    /* Step 4: CPU A attempts dcache_insert_validated with stale lookup_gen -> MUST be rejected */
    dcache_insert_validated(root, "race.txt", child_a, lookup_gen);
    assert_eq(1u, child_a->refcount); /* dcache did NOT take a ref */

    /* Verify /race.txt is NOT in dcache and vfs_lookup returns NULL */
    assert_null(vfs_lookup("/race.txt"));

    /* Verify CPU A can still safely read its open orphan handle before final put */
    char buf[16] = {0};
    assert_eq(9, vfs_read(child_a, 0, sizeof(buf), buf));
    assert_str_eq("RACE_DATA", buf);

    vfs_node_put(child_a);
    vfs_node_put(root);
}

TEST_FUNC(test_dcache_metadata_stat_consistency_across_write_and_truncate) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));

    vfs_node_t *f = NULL;
    assert_eq(0, vfs_create_file("/meta.bin", "/", &f));
    vfs_node_put(f);

    /* Populate dcache by looking up /meta.bin */
    vfs_node_t *cached_h = vfs_lookup("/meta.bin");
    vfs_node_t *open_h = vfs_lookup("/meta.bin");
    assert_not_null(cached_h);
    assert_true(cached_h == open_h);

    struct stat st;
    assert_eq(0, vfs_stat(cached_h, &st));
    assert_eq(0, st.st_size);

    /* Write 200 bytes via open_h */
    char data[200];
    memset(data, 'M', sizeof(data));
    assert_eq(200, vfs_write(open_h, 0, sizeof(data), data));

    struct stat st_cached, st_open, st_rebuilt;
    assert_eq(0, vfs_stat(cached_h, &st_cached));
    assert_eq(0, vfs_stat(open_h, &st_open));

    /* Evict from dcache and lookup a fresh node to verify rebuilt node matches too */
    dcache_flush_mount(cached_h->mount);
    vfs_node_t *rebuilt_h = vfs_lookup("/meta.bin");
    assert_not_null(rebuilt_h);
    assert_eq(0, vfs_stat(rebuilt_h, &st_rebuilt));

    assert_eq(200, st_cached.st_size);
    assert_eq(200, st_open.st_size);
    assert_eq(200, st_rebuilt.st_size);

    /* Truncate to 48 bytes and verify all three again */
    assert_eq(0, vfs_truncate(open_h, 48));
    assert_eq(0, vfs_stat(cached_h, &st_cached));
    assert_eq(0, vfs_stat(open_h, &st_open));
    dcache_flush_mount(cached_h->mount);
    vfs_node_put(rebuilt_h);
    rebuilt_h = vfs_lookup("/meta.bin");
    assert_not_null(rebuilt_h);
    assert_eq(0, vfs_stat(rebuilt_h, &st_rebuilt));

    assert_eq(48, st_cached.st_size);
    assert_eq(48, st_open.st_size);
    assert_eq(48, st_rebuilt.st_size);

    vfs_node_put(rebuilt_h);
    vfs_node_put(open_h);
    vfs_node_put(cached_h);
}

TEST_FUNC(test_dcache_rename_directory_flushes_descendant_parent_chain) {
    vfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));
    assert_eq(0, vfs_mkdir("/a", "/"));
    assert_eq(0, vfs_mkdir("/a/d", "/"));
    assert_eq(0, vfs_mkdir("/a/d/sub", "/"));
    assert_eq(0, vfs_mkdir("/b", "/"));

    /* Warm dcache with /a/d/sub */
    vfs_node_t *old_sub = vfs_lookup("/a/d/sub");
    assert_not_null(old_sub);
    char path_before[128] = {0};
    assert_true(vfs_resolve_path(old_sub, path_before, sizeof(path_before)) > 0);
    assert_str_eq("/a/d/sub", path_before);
    vfs_node_put(old_sub);

    /* Move /a/d -> /b/d */
    assert_eq(0, vfs_rename("/a/d", "/b/d", "/"));
    assert_null(vfs_lookup("/a/d/sub"));

    /* Lookup /b/d/sub: must NOT have stale parent pointing to /a/d */
    vfs_node_t *new_sub = vfs_lookup("/b/d/sub");
    assert_not_null(new_sub);
    char path_after[128] = {0};
    assert_true(vfs_resolve_path(new_sub, path_after, sizeof(path_after)) > 0);
    assert_str_eq("/b/d/sub", path_after);
    vfs_node_put(new_sub);

    vfs_node_t *up_two = vfs_lookup("/b/d/sub/../..");
    assert_not_null(up_two);
    assert_str_eq("b", up_two->name);
    vfs_node_put(up_two);
}

TEST_FUNC(test_dcache_no_dcache_bypass_devfs_procfs_and_node_flag) {
    vfs_init();
    devfs_init();
    assert_eq(0, vfs_mount_fs("tmpfs", "/", NULL, NULL));
    assert_eq(0, vfs_mount_fs("procfs", "/proc", NULL, NULL));

    uint64_t h0 = 0, m0 = 0, e0 = 0;
    dcache_get_stats(&h0, &m0, &e0);

    s_proc_lookup_calls = 0;
    vfs_node_t *p1 = vfs_lookup("/proc/meminfo");
    assert_not_null(p1);
    vfs_node_t *p2 = vfs_lookup("/proc/meminfo");
    assert_not_null(p2);
    /* procfs has VFS_FS_NO_DCACHE, so host_proc_lookup must be called every time */
    assert_eq(2, s_proc_lookup_calls);
    assert_true(p1 != p2);
    vfs_node_put(p2);
    vfs_node_put(p1);

    /* devfs also has VFS_FS_NO_DCACHE */
    vfs_node_t *d1 = vfs_lookup("/dev/null");
    vfs_node_t *d2 = vfs_lookup("/dev/null");
    assert_not_null(d1);
    assert_not_null(d2);
    assert_true(d1 != d2);
    vfs_node_put(d2);
    vfs_node_put(d1);

    uint64_t h1 = 0, m1 = 0, e1 = 0;
    dcache_get_stats(&h1, &m1, &e1);
    assert_eq(h0, h1);
    assert_eq(m0, m1);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_dcache_hit_miss_and_case_insensitive_vfat),
    TEST_ENTRY(test_dcache_lru_256_capacity_eviction_and_submount_parent_rebuild),
    TEST_ENTRY(test_dcache_concurrent_lookup_unlink_generation_barrier),
    TEST_ENTRY(test_dcache_metadata_stat_consistency_across_write_and_truncate),
    TEST_ENTRY(test_dcache_rename_directory_flushes_descendant_parent_chain),
    TEST_ENTRY(test_dcache_no_dcache_bypass_devfs_procfs_and_node_flag),
TEST_LIST_END

int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
