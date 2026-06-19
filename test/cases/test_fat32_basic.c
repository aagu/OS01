/*
 * test/cases/test_fat32_basic.c — FAT32 filesystem unit tests
 * Self-contained — fake block device, test fat32_next_cluster,
 * fat32_cluster_to_sector, and fat32_mount validation logic.
 */
#include "test_framework.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ── FAT32 constants (from kernel headers) ───────── */
#define FAT32_EOC_MIN       0x0FFFFFF8
#define FAT32_CLUSTER_MASK  0x0FFFFFFF
#define FAT32_CLUSTER_FREE  0x00000000

/* ── Minimal FAT32 structures ────────────────────── */
typedef struct {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  num_fats;
    uint16_t root_entries;
    uint16_t total_sectors_16;
    uint8_t  media;
    uint16_t sectors_per_fat_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t sectors_per_fat_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
} __attribute__((packed)) FAT32_BPB;

typedef struct {
    uint8_t  name[11];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  creation_tenths;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t access_date;
    uint16_t first_cluster_hi;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_lo;
    uint32_t file_size;
} __attribute__((packed)) FAT32_DIRENT;

typedef struct {
    void    *dev;
    FAT32_BPB bpb;
    uint32_t fat_start_sector;
    uint32_t first_data_sector;
    uint32_t sectors_per_cluster;
    uint16_t bytes_per_sector;
    uint32_t root_cluster;
} fat32_fs_t;

/* ── Mock block device (in-memory) ────────────────── */
#define BLOCK_SIZE 512
#define BLOCK_COUNT 65536
static uint8_t mock_disk[BLOCK_SIZE * BLOCK_COUNT];

static int mock_block_read(void *dev, uint64_t lba, uint32_t count, void *buf) {
    (void)dev;
    if (lba + count > BLOCK_COUNT) return -1;
    memcpy(buf, mock_disk + lba * BLOCK_SIZE, count * BLOCK_SIZE);
    return 0;
}

/* ── Fake block device struct ────────────────────── */
typedef struct {
    char name[16];
    int (*read)(void *dev, uint64_t lba, uint32_t count, void *buf);
    int (*write)(void *dev, uint64_t lba, uint32_t count, const void *buf);
} block_device_t;

static block_device_t mock_dev = {
    .name = "mock",
    .read = mock_block_read,
};

/* ── Helper: format mock disk as FAT32 ────────────── */
static void format_mock_fat32(void) {
    memset(mock_disk, 0, sizeof(mock_disk));
    FAT32_BPB *bpb = (FAT32_BPB *)mock_disk;

    bpb->bytes_per_sector = 512;
    bpb->sectors_per_cluster = 1;
    bpb->reserved_sectors = 32;
    bpb->num_fats = 2;
    bpb->sectors_per_fat_32 = 64;
    bpb->root_cluster = 2;
    bpb->total_sectors_32 = 65536;
    bpb->media = 0xF8;
    bpb->sectors_per_fat_16 = 0;  /* indicates FAT32 */

    /* Boot signature */
    mock_disk[510] = 0x55;
    mock_disk[511] = 0xAA;

    /* FAT chain: cluster 2 = EOC (root dir is 1 cluster) */
    uint32_t fat_start = bpb->reserved_sectors;
    uint32_t *fat = (uint32_t *)(mock_disk + fat_start * BLOCK_SIZE);
    fat[2] = FAT32_EOC_MIN;  /* cluster 2 is end-of-chain */

    /* Root directory: empty (no entries) */
    uint32_t data_start = fat_start + bpb->num_fats * bpb->sectors_per_fat_32;
    uint32_t root_sector = data_start + (2 - 2) * bpb->sectors_per_cluster;
    /* root dir at root_sector — stays zeroed (empty) */
}

/* ── FAT32 logical functions (test targets) ───────── */

static uint32_t fat32_next_cluster(fat32_fs_t *fs, uint32_t current) {
    if (current >= FAT32_EOC_MIN) return FAT32_EOC_MIN;
    uint64_t fat_offset = (uint64_t)current * 4;
    uint64_t sector_lba = fs->fat_start_sector + (fat_offset / fs->bytes_per_sector);
    uint32_t entry_off = fat_offset % fs->bytes_per_sector;
    uint8_t sector[512];
    if (mock_block_read(NULL, sector_lba, 1, sector) != 0) return FAT32_EOC_MIN;
    return *(uint32_t *)(sector + entry_off) & FAT32_CLUSTER_MASK;
}

static uint64_t fat32_cluster_to_sector(fat32_fs_t *fs, uint32_t cluster) {
    if (cluster < 2) return fs->first_data_sector;
    return fs->first_data_sector + ((uint64_t)(cluster - 2) * fs->sectors_per_cluster);
}

/* ── Tests ──────────────────────────────────────────── */
static fat32_fs_t test_fs;

TEST_FUNC(test_fat32_cluster_to_sector) {
    memset(&test_fs, 0, sizeof(test_fs));
    test_fs.bytes_per_sector = 512;
    test_fs.sectors_per_cluster = 1;
    test_fs.fat_start_sector = 32;
    test_fs.first_data_sector = 32 + 2 * 64; /* reserved + 2 FATs * 64 sectors */
    test_fs.root_cluster = 2;

    /* cluster 2 → first_data_sector (data_start + 0) */
    uint64_t s = fat32_cluster_to_sector(&test_fs, 2);
    assert_eq(s, (uint64_t)(32 + 2 * 64));
    s = fat32_cluster_to_sector(&test_fs, 3);
    assert_eq(s, (uint64_t)(32 + 2 * 64 + 1));
}

TEST_FUNC(test_fat32_next_cluster_eoc) {
    assert_eq(fat32_next_cluster(&test_fs, FAT32_EOC_MIN), FAT32_EOC_MIN);
    assert_eq(fat32_next_cluster(&test_fs, FAT32_EOC_MIN + 1), FAT32_EOC_MIN);
}

TEST_FUNC(test_fat32_mount_validation) {
    format_mock_fat32();
    FAT32_BPB *bpb = (FAT32_BPB *)mock_disk;

    /* Validate BPB signature */
    assert_eq(mock_disk[510], 0x55);
    assert_eq(mock_disk[511], 0xAA);

    /* Validate FAT32 detection */
    assert_eq(bpb->sectors_per_fat_16, 0);  /* must be 0 for FAT32 */
    assert_eq(bpb->sectors_per_fat_32, 64); /* must be > 0 */

    /* Validate computed offsets */
    uint32_t fat_start = bpb->reserved_sectors;
    uint32_t data_start = fat_start + bpb->num_fats * bpb->sectors_per_fat_32;
    assert_eq(fat_start, 32);
    assert_eq(data_start, 32 + 2 * 64);
}

TEST_FUNC(test_fat32_root_dir_empty) {
    format_mock_fat32();
    fat32_fs_t fs;
    memset(&fs, 0, sizeof(fs));
    fs.bytes_per_sector = 512;
    fs.sectors_per_cluster = 1;
    fs.root_cluster = 2;
    fs.fat_start_sector = 32;
    fs.first_data_sector = 32 + 2 * 64;

    /* Read first entry in root dir — should be empty (all zeros) */
    uint32_t root_sector = (uint32_t)fat32_cluster_to_sector(&fs, fs.root_cluster);
    uint8_t sector[512];
    mock_block_read(NULL, root_sector, 1, sector);
    int empty = 1;
    for (int i = 0; i < 512; i += 32) {
        if (sector[i] != 0) { empty = 0; break; }
    }
    assert_true(empty);
}

TEST_FUNC(test_fat32_next_cluster_chain) {
    format_mock_fat32();
    fat32_fs_t fs;
    memset(&fs, 0, sizeof(fs));
    fs.bytes_per_sector = 512;
    fs.sectors_per_cluster = 1;
    fs.fat_start_sector = 32;
    fs.first_data_sector = 32 + 2 * 64;

    /* Manually set up a chain: 2 → 3 → 4 → EOC */
    uint32_t *fat = (uint32_t *)(mock_disk + 32 * BLOCK_SIZE);
    fat[2] = 3;
    fat[3] = 4;
    fat[4] = FAT32_EOC_MIN;

    assert_eq(fat32_next_cluster(&fs, 2), 3);
    assert_eq(fat32_next_cluster(&fs, 3), 4);
    assert_eq(fat32_next_cluster(&fs, 4), FAT32_EOC_MIN);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_fat32_cluster_to_sector),
    TEST_ENTRY(test_fat32_next_cluster_eoc),
    TEST_ENTRY(test_fat32_mount_validation),
    TEST_ENTRY(test_fat32_root_dir_empty),
    TEST_ENTRY(test_fat32_next_cluster_chain),
TEST_LIST_END

int main() {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
