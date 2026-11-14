/*
 * test_ext2_bmap.c — ext2 logical→physical block mapping (host test).
 *
 * The REAL kernel/fs/ext2.c is host-compiled here (the same
 * ext2_production.o the other filesystem host tests link) and driven
 * against a RAM disk that holds a hand-built ext2 image.
 *
 * What is proven here:
 *   1. a file's DIRECT blocks (i_block[0..11]) map to the right
 *      physical blocks;
 *   2. a file's SINGLE-INDIRECT blocks (i_block[12]) map to the right
 *      physical blocks;
 *   3. a file's DOUBLE-INDIRECT blocks (i_block[13]) map to the right
 *      physical blocks.  This is the case the driver used to get
 *      wrong: ext2_bmap() returned 0 ("double/triple indirect not
 *      implemented") for every logical block past the single-indirect
 *      range, so the tail of any file larger than 4 MiB read back as
 *      a zero-filled hole.  In the OS that is not a cosmetic bug —
 *      /bin/test_lvgl (5,109,920 bytes) keeps its stdio stream state
 *      in .ldata at file offset 0x4c1720 (logical block 1217), so the
 *      loader read the page as zeros and printf/fprintf/fwrite all
 *      failed silently;
 *   4. a read that STRADDLES the single→double indirect boundary
 *      returns both sides' bytes.
 *
 * The image is built by hand (not through the driver) so the expected
 * mapping is the on-disk ext2 format, stated by this test, and the
 * driver is measured against it.
 *
 * Geometry (4 KiB blocks, 1 block group):
 *   block 0            superblock (at byte 1024)
 *   block 1            group descriptor table
 *   block 2            block bitmap
 *   block 3            inode bitmap
 *   block 4            inode table (32 inodes x 128 bytes)
 *   block 5            root directory
 *   blocks 6..1045     the file's 1040 data blocks
 *   block 1046         single-indirect table
 *   block 1047         double-indirect table
 *   block 1048         the first double-indirect L2 table
 *
 * The file's logical→physical layout:
 *   logical   0..11     direct          → physical   6..17
 *   logical  12..1035   single indirect → physical  18..1041
 *   logical 1036..1039  double indirect → physical 1042..1045
 *
 * Every data block of logical block L holds the 32-bit little-endian
 * value L repeated, so the expected content of any read is derivable
 * from the offset alone — the test never needs to know the driver's
 * mapping to know what the right answer is.
 */

#include "test_framework.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <fs/ext2.h>
#include <fs/vfs.h>
#include <block/blockdev.h>

/* ── Runtime surface required by the host-compiled ext2.c ─────── */

task_t *vfs_test_current = NULL;
int g_log_level = 0;
void _log_write(int level, const char *fmt, ...) { (void)level; (void)fmt; }
void _log_err_impl(const char *fmt, ...) { (void)fmt; }
void _log_warn_impl(const char *fmt, ...) { (void)fmt; }

/* The REAL block-device dispatch: ext2 reads and writes the RAM disk
 * through this wrapper (the registry test stubs it -EIO because it
 * performs no IO; this test performs real IO). */
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

/* ── RAM disk ────────────────────────────────────────────────── */

#define SECTOR_SIZE      512
#define SECTOR_COUNT     32768              /* 16 MiB */
#define BLOCK_SIZE       4096
#define BLOCK_COUNT      (SECTOR_COUNT * SECTOR_SIZE / BLOCK_SIZE)

static uint8_t s_disk[SECTOR_SIZE * SECTOR_COUNT];

static int disk_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    (void)dev;
    if (lba + count > SECTOR_COUNT) return -EIO;
    memcpy(buf, s_disk + lba * SECTOR_SIZE, count * SECTOR_SIZE);
    return 0;
}

static int disk_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    (void)dev;
    if (lba + count > SECTOR_COUNT) return -EIO;
    memcpy(s_disk + lba * SECTOR_SIZE, buf, count * SECTOR_SIZE);
    return 0;
}

static const struct block_device_ops s_disk_ops = {
    .read = disk_read,
    .write = disk_write,
};

static block_device_t s_block_dev = {
    .name = "ext2bmap0",
    .sector_count = SECTOR_COUNT,
    .sector_size = SECTOR_SIZE,
    .present = 1,
    .kind = BLOCK_DISK,
    .ops = &s_disk_ops,
};

/* ── Image geometry ──────────────────────────────────────────── */

#define FILE_INO          11
#define WRITE_INO        12   /* a fresh inode the write path fills */
#define ROOT_INO          2
#define INODE_TABLE_BLK   4
#define ROOT_DIR_BLK      5
#define FIRST_DATA_BLK    6

#define DIRECT_BLOCKS     12
#define PTRS_PER_BLOCK    (BLOCK_SIZE / sizeof(uint32_t))   /* 1024 */
#define SINGLE_SPAN       PTRS_PER_BLOCK                     /* logical 12..1035 */
#define DOUBLE_START      (DIRECT_BLOCKS + SINGLE_SPAN)      /* logical 1036 */
#define DOUBLE_BLOCKS     4
#define FILE_BLOCKS       (DIRECT_BLOCKS + SINGLE_SPAN + DOUBLE_BLOCKS)  /* 1040 */
#define LAST_DATA_BLK     (FIRST_DATA_BLK + FILE_BLOCKS - 1)           /* 1045 */
#define SINGLE_TBL_BLK    (LAST_DATA_BLK + 1)                          /* 1046 */
#define DOUBLE_TBL_BLK    (LAST_DATA_BLK + 2)                          /* 1047 */
#define DOUBLE_L2_BLK     (LAST_DATA_BLK + 3)                          /* 1048 */
#define FILE_SIZE         ((uint64_t)FILE_BLOCKS * BLOCK_SIZE)

/* Write the 32-bit little-endian value `v` into every 4-byte slot of a
 * block — the file's data pattern: block for logical L holds L. */
static void fill_block(uint32_t blk, uint32_t v)
{
    uint32_t *p = (uint32_t *)(s_disk + (size_t)blk * BLOCK_SIZE);
    for (int i = 0; i < BLOCK_SIZE / sizeof(uint32_t); i++)
        p[i] = v;
}

static void build_image(void)
{
    memset(s_disk, 0, sizeof(s_disk));

    /* Superblock (byte 1024). */
    ext2_superblock_t *sb = (ext2_superblock_t *)(s_disk + 1024);
    sb->s_inodes_count      = 32;
    sb->s_blocks_count      = BLOCK_COUNT;
    sb->s_r_blocks_count    = 0;
    sb->s_free_blocks_count = BLOCK_COUNT - (DOUBLE_L2_BLK + 1);
    sb->s_free_inodes_count = 32 - FILE_INO;
    sb->s_first_data_block  = 0;
    sb->s_log_block_size    = 2;          /* 4 KiB blocks */
    sb->s_log_frag_size     = 2;
    sb->s_blocks_per_group  = BLOCK_COUNT;
    sb->s_frags_per_group   = BLOCK_COUNT;
    sb->s_inodes_per_group  = 32;
    sb->s_magic             = EXT2_MAGIC;
    sb->s_state             = 1;
    sb->s_errors            = 1;
    sb->s_rev_level         = 1;
    sb->s_first_ino         = 11;
    sb->s_inode_size        = 128;

    /* Group descriptor (block 1). */
    ext2_bgdesc_t *bg = (ext2_bgdesc_t *)(s_disk + BLOCK_SIZE);
    bg->bg_block_bitmap       = 2;
    bg->bg_inode_bitmap       = 3;
    bg->bg_inode_table        = INODE_TABLE_BLK;
    bg->bg_free_blocks_count  = BLOCK_COUNT - (DOUBLE_L2_BLK + 1);
    bg->bg_free_inodes_count  = 32 - FILE_INO;
    bg->bg_used_dirs_count    = 1;

    /* Block bitmap (block 2): bit set = in use. */
    uint8_t *bbitmap = s_disk + 2 * BLOCK_SIZE;
    for (uint32_t b = 0; b <= DOUBLE_L2_BLK; b++)
        bbitmap[b >> 3] |= (uint8_t)(1u << (b & 7));

    /* Inode bitmap (block 3): inodes 1..12 in use. */
    uint8_t *ibitmap = s_disk + 3 * BLOCK_SIZE;
    for (uint32_t i = 0; i < WRITE_INO; i++)
        ibitmap[i >> 3] |= (uint8_t)(1u << (i & 7));

    /* Root directory inode (inode 2).  ext2 inode numbers are 1-based:
     * inode N lives at index N-1 inside the inode table. */
    ext2_inode_t *root_ino =
        (ext2_inode_t *)(s_disk + INODE_TABLE_BLK * BLOCK_SIZE + (ROOT_INO - 1) * 128);
    root_ino->i_mode        = EXT2_S_IFDIR | 0755;
    root_ino->i_size        = BLOCK_SIZE;
    root_ino->i_links_count = 2;
    root_ino->i_blocks      = BLOCK_SIZE / 512;
    root_ino->i_block[0]    = ROOT_DIR_BLK;

    /* Root directory content (block 5): ".", "..", "big.bin". */
    uint8_t *root_dir = s_disk + ROOT_DIR_BLK * BLOCK_SIZE;
    ext2_dirent_t *dot = (ext2_dirent_t *)root_dir;
    dot->inode = ROOT_INO;
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = EXT2_FT_DIR;
    memcpy(dot->name, ".", 1);

    ext2_dirent_t *dotdot = (ext2_dirent_t *)(root_dir + 12);
    dotdot->inode = ROOT_INO;
    dotdot->rec_len = 20;
    dotdot->name_len = 2;
    dotdot->file_type = EXT2_FT_DIR;
    memcpy(dotdot->name, "..", 2);

    ext2_dirent_t *ent = (ext2_dirent_t *)(root_dir + 32);
    ent->inode = FILE_INO;
    ent->rec_len = BLOCK_SIZE - 32;
    ent->name_len = 7;
    ent->file_type = EXT2_FT_REG_FILE;
    memcpy(ent->name, "big.bin", 7);

    /* The file's inode (inode 11). */
    ext2_inode_t *ino =
        (ext2_inode_t *)(s_disk + INODE_TABLE_BLK * BLOCK_SIZE + (FILE_INO - 1) * 128);
    ino->i_mode        = EXT2_S_IFREG | 0644;
    ino->i_size        = FILE_SIZE;
    ino->i_links_count = 1;
    ino->i_blocks      = (FILE_BLOCKS + 3) * (BLOCK_SIZE / 512);
    for (int i = 0; i < DIRECT_BLOCKS; i++)
        ino->i_block[i] = FIRST_DATA_BLK + i;
    ino->i_block[12] = SINGLE_TBL_BLK;
    ino->i_block[13] = DOUBLE_TBL_BLK;
    ino->i_block[14] = 0;

    /* The inode the WRITE path fills: empty, no blocks yet.  The write
     * path has to build the whole index chain itself — including the
     * double-indirect level, because the file grows past the
     * single-indirect range. */
    ext2_inode_t *wino =
        (ext2_inode_t *)(s_disk + INODE_TABLE_BLK * BLOCK_SIZE + (WRITE_INO - 1) * 128);
    wino->i_mode        = EXT2_S_IFREG | 0644;
    wino->i_size        = 0;
    wino->i_links_count = 1;
    wino->i_blocks      = 0;

    /* Data blocks: logical block L lives in a block holding L. */
    for (uint32_t l = 0; l < FILE_BLOCKS; l++)
        fill_block(FIRST_DATA_BLK + l, l);

    /* Single-indirect table (block 1046): entry i -> logical 12+i. */
    uint32_t *single = (uint32_t *)(s_disk + SINGLE_TBL_BLK * BLOCK_SIZE);
    for (uint32_t i = 0; i < PTRS_PER_BLOCK; i++)
        single[i] = FIRST_DATA_BLK + DIRECT_BLOCKS + i;

    /* Double-indirect table (block 1047): entry 0 -> the L2 table. */
    uint32_t *dbl = (uint32_t *)(s_disk + DOUBLE_TBL_BLK * BLOCK_SIZE);
    for (uint32_t i = 0; i < PTRS_PER_BLOCK; i++)
        dbl[i] = 0;
    dbl[0] = DOUBLE_L2_BLK;

    /* The L2 table (block 1048): entry j -> the j-th double-indirect
     * data block. */
    uint32_t *l2 = (uint32_t *)(s_disk + DOUBLE_L2_BLK * BLOCK_SIZE);
    for (uint32_t i = 0; i < PTRS_PER_BLOCK; i++)
        l2[i] = 0;
    for (uint32_t j = 0; j < DOUBLE_BLOCKS; j++)
        l2[j] = FIRST_DATA_BLK + DIRECT_BLOCKS + SINGLE_SPAN + j;
}

/* ── The driver under test ───────────────────────────────────── */

static ext2_fs_t *s_fs;
static vfs_mount_t s_mount;
static vfs_node_t  s_node;

static void mount_image(void)
{
    build_image();

    s_fs = NULL;
    int rc = ext2_init(&s_block_dev, &s_fs);
    assert_eq(0, rc);
    assert_not_null(s_fs);
    if (!s_fs) return;

    memset(&s_mount, 0, sizeof(s_mount));
    s_mount.dev     = &s_block_dev;
    s_mount.fs_data = s_fs;

    memset(&s_node, 0, sizeof(s_node));
    s_node.mount   = &s_mount;
    s_node.fs_data = (void *)(uintptr_t)FILE_INO;
    s_node.f_ops   = &ext2_fops;
}

/* Read `size` bytes at `offset` through the production read op. */
static int read_at(uint64_t offset, uint64_t size, void *buf)
{
    return ext2_fops.read(&s_node, offset, size, buf);
}

/* The expected content of the file at `offset`: every 4-byte word of a
 * block holds the block's LOGICAL block number, little-endian. */
static void expected_pattern(uint64_t offset, uint64_t size, uint8_t *out)
{
    for (uint64_t i = 0; i < size; i++) {
        uint64_t abs = offset + i;
        uint32_t logical = (uint32_t)(abs / BLOCK_SIZE);
        out[i] = (uint8_t)((logical >> (8 * (abs % 4))) & 0xFF);
    }
}

/* Assert that the bytes read at `offset` are the expected pattern. */
static void assert_reads_pattern(uint64_t offset, uint64_t size)
{
    uint8_t got[64];
    uint8_t want[64];
    assert_eq((int)size, (int)read_at(offset, size, got));
    expected_pattern(offset, size, want);
    assert_mem_eq(want, got, (int)size);
}

/* ── Cases ───────────────────────────────────────────────────── */

TEST_FUNC(test_ext2_direct_block_mapping) {
    mount_image();
    /* First and last direct block. */
    assert_reads_pattern(0, 16);
    assert_reads_pattern((DIRECT_BLOCKS - 1) * BLOCK_SIZE, 16);
}

TEST_FUNC(test_ext2_single_indirect_block_mapping) {
    mount_image();
    /* First and last single-indirect block. */
    assert_reads_pattern(DIRECT_BLOCKS * BLOCK_SIZE, 16);
    assert_reads_pattern((DOUBLE_START - 1) * BLOCK_SIZE, 16);
}

TEST_FUNC(test_ext2_double_indirect_block_mapping) {
    mount_image();
    /* First and last double-indirect block.  Before the driver
     * implemented double-indirect mapping these reads came back as
     * zeros (the driver treated the range as a hole). */
    assert_reads_pattern(DOUBLE_START * BLOCK_SIZE, 16);
    assert_reads_pattern((DOUBLE_START + DOUBLE_BLOCKS - 1) * BLOCK_SIZE, 16);
}

TEST_FUNC(test_ext2_indirect_boundary_read) {
    mount_image();
    /* A read straddling the single→double indirect boundary must
     * return the tail of the last single-indirect block followed by
     * the head of the first double-indirect block. */
    uint64_t off = (uint64_t)DOUBLE_START * BLOCK_SIZE - 8;
    uint8_t got[16];
    uint8_t want[16];
    assert_eq(16, read_at(off, 16, got));
    expected_pattern(off, 16, want);
    assert_mem_eq(want, got, 16);
}

TEST_FUNC(test_ext2_tail_of_double_indirect_file) {
    mount_image();
    /* The last bytes of the file live in the last double-indirect
     * block; a short read at EOF must still return them. */
    uint64_t off = FILE_SIZE - 8;
    uint8_t got[8];
    assert_eq(8, read_at(off, 8, got));
    uint32_t logical = (uint32_t)(off / BLOCK_SIZE);
    for (int i = 0; i < 4; i++)
        assert_eq((int)((logical >> (8 * i)) & 0xFF), (int)got[i]);
    for (int i = 4; i < 8; i++)
        assert_eq((int)((logical >> (8 * (i - 4))) & 0xFF), (int)got[i]);
}

TEST_FUNC(test_ext2_write_builds_double_indirect_chain) {
    mount_image();

    /* A fresh inode: the write path has to build the whole index chain
     * itself.  The file is large enough to reach the double-indirect
     * range, so the driver must create the double-indirect table AND
     * the single-indirect table inside it. */
    static uint8_t *payload;
    uint64_t size = (uint64_t)FILE_BLOCKS * BLOCK_SIZE;
    payload = malloc((size_t)size);
    assert_not_null(payload);
    if (!payload) return;
    for (uint32_t l = 0; l < FILE_BLOCKS; l++) {
        uint32_t *p = (uint32_t *)(payload + (size_t)l * BLOCK_SIZE);
        for (int i = 0; i < BLOCK_SIZE / sizeof(uint32_t); i++)
            p[i] = l;
    }

    vfs_node_t wnode;
    memset(&wnode, 0, sizeof(wnode));
    wnode.mount   = &s_mount;
    wnode.fs_data = (void *)(uintptr_t)WRITE_INO;
    wnode.f_ops   = &ext2_fops;

    int n = ext2_fops.write(&wnode, 0, size, payload);
    assert_eq((int)size, n);
    free(payload);

    /* The inode must now carry the double-indirect pointer (i_block[13])
     * and NOT the triple-indirect one. */
    ext2_inode_t wino;
    memcpy(&wino, s_disk + INODE_TABLE_BLK * BLOCK_SIZE + (WRITE_INO - 1) * 128,
           sizeof(wino));
    assert_true(wino.i_block[13] != 0);
    assert_true(wino.i_block[14] == 0);
    assert_eq((uint32_t)size, wino.i_size);

    /* Read it back through the read path. */
    s_node.fs_data = (void *)(uintptr_t)WRITE_INO;
    for (uint32_t l = 0; l < FILE_BLOCKS; l += 97) {
        uint64_t off = (uint64_t)l * BLOCK_SIZE;
        uint8_t got[16];
        uint8_t want[16];
        assert_eq(16, read_at(off, 16, got));
        expected_pattern(off, 16, want);
        assert_mem_eq(want, got, 16);
    }
    /* The first double-indirect block and the last one. */
    assert_reads_pattern(DOUBLE_START * BLOCK_SIZE, 16);
    assert_reads_pattern((FILE_BLOCKS - 1) * BLOCK_SIZE, 16);
    s_node.fs_data = (void *)(uintptr_t)FILE_INO;
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_ext2_direct_block_mapping),
    TEST_ENTRY(test_ext2_single_indirect_block_mapping),
    TEST_ENTRY(test_ext2_double_indirect_block_mapping),
    TEST_ENTRY(test_ext2_indirect_boundary_read),
    TEST_ENTRY(test_ext2_tail_of_double_indirect_file),
    TEST_ENTRY(test_ext2_write_builds_double_indirect_chain),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}
