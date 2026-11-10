/* hosttests/mock/vfs_ram_disk.h
 *
 * A RAM-backed block device plus the mock ext2 / FAT32 formatters used
 * by the host filesystem tests.
 *
 * The block device is a plain byte array: the formatter writes a
 * filesystem image into it and the kernel's ext2.c / fat.c read and
 * write it through the block-device ops.  That is enough to exercise
 * the real filesystem code paths (block allocation, directory entries,
 * inodes) without a real disk.
 *
 * The image geometry matches the one the kernel's ext2 / FAT32 drivers
 * expect:
 *   - ext2: 4096-byte blocks, 64 blocks, 32 inodes, 128-byte inodes
 *   - FAT32: 512-byte sectors, 1 sector/cluster, 16 sectors/FAT
 */
#ifndef OS01_VFS_RAM_DISK_H
#define OS01_VFS_RAM_DISK_H

#include <stdint.h>
#include <string.h>

#include <block/blockdev.h>
#include <fs/ext2.h>
#include <fs/fat.h>

#define RAM_DISK_SECTOR_SIZE 512
#define RAM_DISK_SECTOR_COUNT 1024

static uint8_t s_ram_disk[RAM_DISK_SECTOR_SIZE * RAM_DISK_SECTOR_COUNT];

static int ram_disk_read(block_device_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    (void)dev;
    if (lba + count > RAM_DISK_SECTOR_COUNT) return -EIO;
    memcpy(buf, s_ram_disk + lba * RAM_DISK_SECTOR_SIZE,
           count * RAM_DISK_SECTOR_SIZE);
    return 0;
}

static int ram_disk_write(block_device_t *dev, uint64_t lba, uint32_t count, const void *buf)
{
    (void)dev;
    if (lba + count > RAM_DISK_SECTOR_COUNT) return -EIO;
    memcpy(s_ram_disk + lba * RAM_DISK_SECTOR_SIZE, buf,
           count * RAM_DISK_SECTOR_SIZE);
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

/* ── FAT32 ─────────────────────────────────────────────────── */

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

/* ── ext2 ──────────────────────────────────────────────────── */

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

#endif /* OS01_VFS_RAM_DISK_H */
