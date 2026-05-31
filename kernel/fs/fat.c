#include <fs/fat.h>
#include <block/blockdev.h>
#include <fs/vfs.h>
#include <kernel/printk.h>
#include <string.h>
#include <stdlib.h>

// ── Read a FAT entry ────────────────────────────────────
// Returns the next cluster in the chain, or >= FAT32_EOC_MIN if end.

uint32_t fat32_next_cluster(fat32_fs_t *fs, uint32_t current)
{
    if (current >= FAT32_EOC_MIN)
        return FAT32_EOC_MIN;

    // Each FAT entry is 4 bytes
    uint64_t fat_offset = (uint64_t)current * 4;
    uint64_t sector_lba = fs->fat_start_sector + (fat_offset / fs->bytes_per_sector);
    uint32_t entry_off  = fat_offset % fs->bytes_per_sector;

    // Read the FAT sector containing this entry
    uint8_t sector[512];
    if (block_device_read(fs->dev, sector_lba, 1, sector) != 0) {
        serial_printk("FAT: error reading FAT sector %lu\n", sector_lba);
        return FAT32_EOC_MIN;
    }

    uint32_t next = *(uint32_t *)(sector + entry_off) & FAT32_CLUSTER_MASK;
    return next;
}

// ── Cluster → LBA conversion ─────────────────────────────
// Cluster numbers start at 2.

uint64_t fat32_cluster_to_sector(fat32_fs_t *fs, uint32_t cluster)
{
    if (cluster < 2) {
        serial_printk("FAT: invalid cluster %u\n", cluster);
        return fs->first_data_sector;
    }
    return fs->first_data_sector
           + ((uint64_t)(cluster - 2) * fs->sectors_per_cluster);
}

// ── Read a directory entry by index ──────────────────────
// Walks the cluster chain starting at dir_cluster.
// Returns 0 on success, -1 if index exceeds directory size.

int fat32_read_entry(fat32_fs_t *fs, uint32_t dir_cluster,
                     uint64_t index, vfs_dirent_t *entry)
{
    if (!entry) return -1;

    // Calculate total entries per cluster
    uint32_t entries_per_cluster = (fs->sectors_per_cluster * fs->bytes_per_sector) / 32;

    // Determine which cluster in the chain and which entry within
    // Walk the cluster chain to find the right cluster
    // For root directory: just use root_cluster directly (may be multiple clusters)
    // For subdirectories: walk the chain
    // Since directories can span multiple clusters, we need to traverse the FAT chain

    uint64_t cluster_index = 0;
    uint32_t current_cluster = dir_cluster;

    while (current_cluster < FAT32_EOC_MIN) {
        uint64_t entries_in_this_cluster = entries_per_cluster;

        if (index < cluster_index + entries_in_this_cluster) {
            // Entry is in this cluster
            uint64_t local_idx = index - cluster_index;
            uint64_t offset_in_cluster = local_idx * 32;

            // Calculate which sector within the cluster
            uint32_t sector_in_cluster = (uint32_t)(offset_in_cluster / fs->bytes_per_sector);
            uint32_t offset_in_sector  = offset_in_cluster % fs->bytes_per_sector;

            uint64_t sector_lba = fat32_cluster_to_sector(fs, current_cluster) + sector_in_cluster;

            // Read the sector
            uint8_t sector[512];
            if (block_device_read(fs->dev, sector_lba, 1, sector) != 0)
                return -1;

            FAT32_DIRENT *dirent = (FAT32_DIRENT *)(sector + offset_in_sector);

            // Check for end of directory
            if (dirent->name[0] == 0x00) {
                entry->name[0] = '\0';
                return 0;
            }

            // Skip deleted entries
            if (dirent->name[0] == 0xE5)
                goto skip;

            // Check for LFN — skip LFN entries (we use short names only for now)
            if (dirent->attr == FAT_ATTR_LFN)
                goto skip;

            // Skip volume label
            if (dirent->attr & FAT_ATTR_VOLUME_ID)
                goto skip;

            // ── Parse the short (8.3) name ──────────
            uint8_t *name8 = dirent->name;
            int name_len = 0;

            // Copy name (8 chars max, trim trailing spaces)
            int i;
            for (i = 0; i < 8 && name8[i] != ' '; i++)
                entry->name[name_len++] = name8[i];

            // If there's an extension, add '.'
            for (i = 0; i < 3 && name8[8 + i] != ' '; i++) {
                if (i == 0) entry->name[name_len++] = '.';
                entry->name[name_len++] = name8[8 + i];
            }
            entry->name[name_len] = '\0';

            // Convert to lowercase if the filename case bits indicate
            // (FAT32 has NT_RES flags for lowercase name/ext)
            // For now, leave as-is.

            // ── Fill the entry ──────────────────────
            uint32_t cluster = (uint32_t)dirent->first_cluster_lo
                             | ((uint32_t)dirent->first_cluster_hi << 16);
            entry->ino  = cluster;
            entry->size = dirent->file_size;

            if (dirent->attr & FAT_ATTR_DIRECTORY)
                entry->type = VFS_DIR;
            else
                entry->type = VFS_FILE;

            return 0;
        }

        cluster_index += entries_in_this_cluster;
        current_cluster = fat32_next_cluster(fs, current_cluster);
    }

    // Past end of directory
    entry->name[0] = '\0';
    return 0;

skip:
    // Caller should try the next index (entry is LFN, deleted, or volume label)
    return -1;
}

// ── Read data from a cluster chain ───────────────────────
// Reads `size` bytes starting at `offset` from the file whose
// first cluster is `first_cluster`.

int fat32_read_data(fat32_fs_t *fs, uint32_t first_cluster,
                    uint64_t offset, uint64_t size, void *buffer)
{
    if (size == 0) return 0;
    if (first_cluster < 2) return -1;

    uint32_t cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;
    uint8_t *buf = (uint8_t *)buffer;
    uint64_t bytes_remaining = size;
    uint64_t file_offset = offset;

    // Walk the cluster chain
    uint32_t cluster = first_cluster;

    while (cluster < FAT32_EOC_MIN && bytes_remaining > 0) {
        uint64_t cluster_lba = fat32_cluster_to_sector(fs, cluster);

        // If offset is within this cluster
        if (file_offset < cluster_size) {
            uint64_t cluster_offset = file_offset;
            uint64_t to_read = cluster_size - cluster_offset;
            if (to_read > bytes_remaining)
                to_read = bytes_remaining;

            // Read the sectors for this part of the cluster
            uint32_t start_sector = (uint32_t)(cluster_offset / fs->bytes_per_sector);
            uint32_t sector_count = (uint32_t)((to_read + fs->bytes_per_sector - 1)
                                               / fs->bytes_per_sector);

            for (uint32_t s = 0; s < sector_count; s++) {
                uint8_t temp[512];
                if (block_device_read(fs->dev, cluster_lba + start_sector + s, 1, temp) != 0)
                    return -1;

                uint32_t copy_start = (s == 0) ? (cluster_offset % fs->bytes_per_sector) : 0;
                uint32_t copy_size = fs->bytes_per_sector - copy_start;
                if (copy_size > bytes_remaining)
                    copy_size = (uint32_t)bytes_remaining;

                memcpy(buf, temp + copy_start, copy_size);
                buf += copy_size;
                bytes_remaining -= copy_size;
                if (bytes_remaining == 0) break;
            }

            file_offset += to_read;
            if (bytes_remaining == 0) break;
        } else {
            // Skip this cluster
            file_offset -= cluster_size;
        }

        cluster = fat32_next_cluster(fs, cluster);
    }

    return (bytes_remaining == 0) ? (int)(size) : -1;
}

// ── VFS operations for FAT32 ──────────────────────────────

static int fat_read(struct vfs_node *node, uint64_t offset,
                    uint64_t size, void *buffer)
{
    if (!node || !node->fs_data || !node->mount) return -1;
    fat32_fs_t *fs = (fat32_fs_t *)node->mount->fs_data;
    uint32_t cluster = (uint32_t)(uintptr_t)node->fs_data;
    return fat32_read_data(fs, cluster, offset, size, buffer);
}

static int fat_readdir(struct vfs_node *node, uint64_t index,
                       vfs_dirent_t *entry)
{
    if (!node || !node->mount) return -1;
    fat32_fs_t *fs = (fat32_fs_t *)node->mount->fs_data;

    // For root directory, use root_cluster from BPB
    // For subdirectories, node->fs_data holds the cluster number
    uint32_t dir_cluster;
    if (node->fs_data)
        dir_cluster = (uint32_t)(uintptr_t)node->fs_data;
    else
        dir_cluster = fs->root_cluster;

    return fat32_read_entry(fs, dir_cluster, index, entry);
}

struct vfs_ops fat_vfs_ops = {
    .read    = fat_read,
    .readdir = fat_readdir,
};

// ── Mount a FAT32 volume ─────────────────────────────────

fat32_fs_t *fat32_mount(block_device_t *dev)
{
    if (!dev) return NULL;

    // Read sector 0 (BPB)
    uint8_t sector[512];
    if (block_device_read(dev, 0, 1, sector) != 0) {
        serial_printk("FAT: failed to read boot sector\n");
        return NULL;
    }

    FAT32_BPB *bpb = (FAT32_BPB *)sector;

    // Validate BPB: check for 0x55 0xAA signature (bytes at offset 510-511)
    if (sector[510] != 0x55 || sector[511] != 0xAA) {
        serial_printk("FAT: boot sector signature missing (%02x %02x)\n",
                       sector[510], sector[511]);
        return NULL;
    }

    // For FAT32: sectors_per_fat_16 must be 0, and sectors_per_fat_32 > 0
    if (bpb->sectors_per_fat_16 != 0) {
        // Could be FAT12/16 — check total sectors
        uint32_t total = (bpb->total_sectors_16 != 0)
                         ? bpb->total_sectors_16 : bpb->total_sectors_32;
        serial_printk("FAT: not FAT32 (FAT16=%u, total=%u)\n",
                       bpb->sectors_per_fat_16, total);
        return NULL;
    }

    if (bpb->sectors_per_fat_32 == 0) {
        serial_printk("FAT: invalid FAT32: sectors_per_fat=0\n");
        return NULL;
    }

    // Allocate private data
    fat32_fs_t *fs = (fat32_fs_t *)calloc(sizeof(fat32_fs_t));
    if (!fs) return NULL;

    memcpy(&fs->bpb, bpb, sizeof(FAT32_BPB));
    fs->dev = dev;
    fs->bytes_per_sector = bpb->bytes_per_sector;
    fs->sectors_per_cluster = bpb->sectors_per_cluster;
    fs->root_cluster = bpb->root_cluster;

    // Calculate sector offsets (absolute LBAs)
    // hidden_sectors is the partition offset, 0 for superfloppy (no MBR)
    uint32_t partition_start = 0;  // disk.img is raw FAT32, no partition table

    fs->fat_start_sector = partition_start + bpb->reserved_sectors;
    fs->first_data_sector = partition_start
                          + bpb->reserved_sectors
                          + ((uint32_t)bpb->num_fats * bpb->sectors_per_fat_32);

    serial_printk("FAT32: mounted (BpS=%u, SpC=%u, FATs=%u, FATsec=%u, rootClus=%u)\n",
                   fs->bytes_per_sector, fs->sectors_per_cluster,
                   bpb->num_fats, bpb->sectors_per_fat_32, fs->root_cluster);
    serial_printk("FAT32: FAT start=%u, data start=%u\n",
                   fs->fat_start_sector, fs->first_data_sector);

    // Validate the root cluster
    serial_printk("FAT32: root directory at cluster %u (LBA %lu)\n",
                   fs->root_cluster,
                   fat32_cluster_to_sector(fs, fs->root_cluster));

    return fs;
}
