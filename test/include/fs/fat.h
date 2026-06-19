#ifndef _FS_FAT_H
#define _FS_FAT_H

#include <stdint.h>
#include <block/blockdev.h>
#include <fs/vfs.h>

// ── FAT32 BIOS Parameter Block ────────────────────────────
typedef struct {
    uint8_t  jmp[3];                // 0x00
    char     oem[8];                // 0x03
    uint16_t bytes_per_sector;      // 0x0B
    uint8_t  sectors_per_cluster;   // 0x0D
    uint16_t reserved_sectors;      // 0x0E
    uint8_t  num_fats;              // 0x10
    uint16_t root_entries;          // 0x11 (0 for FAT32)
    uint16_t total_sectors_16;      // 0x13
    uint8_t  media;                 // 0x15
    uint16_t sectors_per_fat_16;    // 0x16 (0 for FAT32)
    uint16_t sectors_per_track;     // 0x18
    uint16_t num_heads;             // 0x1A
    uint32_t hidden_sectors;        // 0x1C
    uint32_t total_sectors_32;      // 0x20
    // FAT32 extended fields
    uint32_t sectors_per_fat_32;    // 0x24
    uint16_t ext_flags;             // 0x28
    uint16_t fs_version;            // 0x2A
    uint32_t root_cluster;          // 0x2C
    uint16_t fs_info;               // 0x30
    uint16_t backup_boot;           // 0x32
    uint8_t  reserved[12];          // 0x34
    uint8_t  drive_number;          // 0x40
    uint8_t  reserved1;             // 0x41
    uint8_t  boot_sig;              // 0x42
    uint32_t volume_id;             // 0x43
    char     volume_label[11];      // 0x47
    char     fs_type[8];            // 0x52
} __attribute__((packed)) FAT32_BPB;

// ── Short (8.3) directory entry ──────────────────────────
typedef struct {
    uint8_t  name[11];              // 0x00
    uint8_t  attr;                  // 0x0B
    uint8_t  nt_reserved;           // 0x0C
    uint8_t  creation_tenths;       // 0x0D
    uint16_t creation_time;         // 0x0E
    uint16_t creation_date;         // 0x10
    uint16_t access_date;           // 0x12
    uint16_t first_cluster_hi;      // 0x14
    uint16_t write_time;            // 0x16
    uint16_t write_date;            // 0x18
    uint16_t first_cluster_lo;      // 0x1A
    uint32_t file_size;             // 0x1C
} __attribute__((packed)) FAT32_DIRENT;

// ── Long file name entry (precedes short entry) ──────────
typedef struct {
    uint8_t  order;                 // 0x00 (0x40 = last LFN entry)
    uint16_t name1[5];              // 0x01 chars 1-5
    uint8_t  attr;                  // 0x0B always 0x0F
    uint8_t  type;                  // 0x0C
    uint8_t  checksum;              // 0x0D
    uint16_t name2[6];              // 0x0E chars 6-11
    uint16_t first_cluster;         // 0x1A always 0
    uint16_t name3[2];              // 0x1C chars 12-13
} __attribute__((packed)) FAT32_LDIR;

// ── Directory entry attributes ────────────────────────────
#define FAT_ATTR_READ_ONLY  0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLUME_ID  0x08
#define FAT_ATTR_DIRECTORY  0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LFN        0x0F   // Long File Name entry

// ── Special cluster values ───────────────────────────────
#define FAT32_CLUSTER_FREE      0x00000000
#define FAT32_CLUSTER_RESERVED  0x00000001
#define FAT32_EOC_MIN           0x0FFFFFF8
#define FAT32_EOC_MAX           0x0FFFFFFF
#define FAT32_CLUSTER_MASK      0x0FFFFFFF

// ── FAT32 filesystem private data ─────────────────────────
typedef struct {
    block_device_t *dev;
    FAT32_BPB       bpb;
    uint32_t        fat_start_sector;     // absolute LBA of first FAT
    uint32_t        first_data_sector;    // absolute LBA of cluster 2
    uint32_t        sectors_per_cluster;
    uint16_t        bytes_per_sector;
    uint32_t        root_cluster;
} fat32_fs_t;

// ── FAT32 API ─────────────────────────────────────────────

// Mount a FAT32 filesystem from a block device.
// Returns the fat32_fs_t pointer (for VFS mount's fs_data), or NULL on error.
fat32_fs_t *fat32_mount(block_device_t *dev);

// Get the next cluster in a chain. Returns FAT32_EOC_MIN or higher if end.
uint32_t fat32_next_cluster(fat32_fs_t *fs, uint32_t current);

// Convert cluster number to absolute LBA
uint64_t fat32_cluster_to_sector(fat32_fs_t *fs, uint32_t cluster);

// Read a directory entry at a given index from a directory cluster chain
int fat32_read_entry(fat32_fs_t *fs, uint32_t dir_cluster,
                     uint64_t index, vfs_dirent_t *entry);

// Read file data from a cluster chain
int fat32_read_data(fat32_fs_t *fs, uint32_t first_cluster,
                    uint64_t offset, uint64_t size, void *buffer);

// VFS operations for FAT32
extern struct vfs_ops fat_vfs_ops;

// Create a new regular file in a directory
struct vfs_node *fat_create(struct vfs_node *dir, const char *name);

#endif // _FS_FAT_H
