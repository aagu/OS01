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

            // Reset file_offset — after the first partial cluster,
            // subsequent clusters are read from offset 0.
            file_offset = 0;
        } else {
            // Skip this cluster
            file_offset -= cluster_size;
        }

        cluster = fat32_next_cluster(fs, cluster);
    }

    return (bytes_remaining == 0) ? (int)(size) : -1;
}

// ── Write/read a single FAT entry ─────────────────────────
// Reads the FAT sector, modifies one 4-byte entry, writes it
// to both FAT copies.
static int fat32_write_fat_entry(fat32_fs_t *fs, uint32_t cluster, uint32_t value)
{
    if (cluster < 2 || cluster >= FAT32_EOC_MIN) return -1;

    uint64_t fat_offset = (uint64_t)cluster * 4;
    uint64_t sector_lba = fs->fat_start_sector + (fat_offset / fs->bytes_per_sector);
    uint32_t entry_off  = fat_offset % fs->bytes_per_sector;

    uint8_t sector[512];

    // Write FAT1
    if (block_device_read(fs->dev, sector_lba, 1, sector) != 0)
        return -1;
    *(uint32_t *)(sector + entry_off) = value & FAT32_CLUSTER_MASK;
    if (block_device_write(fs->dev, sector_lba, 1, sector) != 0)
        return -1;

    // Write FAT2 (second copy for redundancy)
    uint64_t fat2_lba = sector_lba + (uint64_t)fs->bpb.sectors_per_fat_32;
    if (block_device_write(fs->dev, fat2_lba, 1, sector) != 0)
        return -1;

    return 0;
}

// ── Read a single FAT entry ───────────────────────────────
static uint32_t fat32_read_fat_entry(fat32_fs_t *fs, uint32_t cluster)
{
    if (cluster < 2 || cluster >= FAT32_EOC_MIN)
        return FAT32_EOC_MIN;

    uint64_t fat_offset = (uint64_t)cluster * 4;
    uint64_t sector_lba = fs->fat_start_sector + (fat_offset / fs->bytes_per_sector);
    uint32_t entry_off  = fat_offset % fs->bytes_per_sector;

    uint8_t sector[512];
    if (block_device_read(fs->dev, sector_lba, 1, sector) != 0)
        return FAT32_EOC_MIN;

    return *(uint32_t *)(sector + entry_off) & FAT32_CLUSTER_MASK;
}

// ── Find a free cluster ───────────────────────────────────
// Scans the FAT starting from cluster 2.
static uint32_t fat32_find_free_cluster(fat32_fs_t *fs)
{
    for (uint32_t cl = 2; cl < 0x0FFFFFF0; cl++) {
        uint32_t val = fat32_read_fat_entry(fs, cl);
        if (val == FAT32_CLUSTER_FREE)
            return cl;
        // Avoid reading every entry individually — batch by sectors
        if ((cl & 0x7F) == 0x7F) {
            // Check next sector's range
            uint64_t fat_offset = (uint64_t)(cl + 1) * 4;
            uint64_t sector_lba = fs->fat_start_sector + (fat_offset / fs->bytes_per_sector);
            // Read this sector and scan it
            uint8_t sector[512];
            if (block_device_read(fs->dev, sector_lba, 1, sector) != 0)
                return 0;
            for (int j = 0; j < 128; j++) {
                uint32_t check_cl = cl + 1 + (uint32_t)j;
                if (check_cl >= 0x0FFFFFF0) break;
                uint32_t v = *(uint32_t *)(sector + j * 4) & FAT32_CLUSTER_MASK;
                if (v == FAT32_CLUSTER_FREE)
                    return check_cl;
            }
            cl += 127; // skip past scanned range
        }
    }
    return 0; // no free clusters
}

// ── Allocate a new cluster at end of chain ────────────────
// If prev_cluster != 0, links the previous cluster to the new one.
// Returns the newly allocated cluster, or 0 on failure.
static uint32_t fat32_alloc_cluster(fat32_fs_t *fs, uint32_t prev_cluster)
{
    uint32_t new_cl = fat32_find_free_cluster(fs);
    if (new_cl == 0) {
        serial_printk("FAT: no free clusters\n");
        return 0;
    }

    // Mark as end-of-chain
    if (fat32_write_fat_entry(fs, new_cl, FAT32_EOC_MIN) != 0)
        return 0;

    // Link from previous cluster
    if (prev_cluster != 0) {
        if (fat32_write_fat_entry(fs, prev_cluster, new_cl) != 0) {
            fat32_write_fat_entry(fs, new_cl, FAT32_CLUSTER_FREE);
            return 0;
        }
    }

    return new_cl;
}

// ── Write data to a cluster chain ─────────────────────────
// Writes `size` bytes starting at `offset`.  If the write extends
// past the current end of chain, additional clusters are allocated.
// Returns bytes written, or -1 on error.
static int fat32_write_data(fat32_fs_t *fs, uint32_t first_cluster,
                            uint64_t offset, uint64_t size, const void *buffer)
{
    if (size == 0) return 0;
    if (first_cluster < 2) return -1;

    uint32_t cluster_size = fs->sectors_per_cluster * fs->bytes_per_sector;
    const uint8_t *src = (const uint8_t *)buffer;
    uint64_t bytes_remaining = size;
    uint64_t file_offset = offset;
    uint32_t prev_cluster = 0;
    uint32_t cluster = first_cluster;

    // Walk to the cluster containing `offset`
    while (cluster < FAT32_EOC_MIN && file_offset >= cluster_size) {
        prev_cluster = cluster;
        file_offset -= cluster_size;
        cluster = fat32_read_fat_entry(fs, cluster);
    }

    while (bytes_remaining > 0) {
        // If past end of chain, extend
        if (cluster >= FAT32_EOC_MIN) {
            uint32_t new_cl = fat32_alloc_cluster(fs, prev_cluster);
            if (new_cl == 0) break;
            cluster = new_cl;
        }

        uint64_t cluster_lba = fat32_cluster_to_sector(fs, cluster);
        uint64_t cluster_offset = file_offset;
        uint64_t to_write = cluster_size - cluster_offset;
        if (to_write > bytes_remaining)
            to_write = bytes_remaining;

        // Write sector by sector (read-modify-write for partial sectors)
        uint32_t start_sector = (uint32_t)(cluster_offset / fs->bytes_per_sector);
        uint32_t end_sector   = (uint32_t)((cluster_offset + to_write + fs->bytes_per_sector - 1)
                                           / fs->bytes_per_sector);
        uint64_t written_in_cluster = 0;

        for (uint32_t s = start_sector; s < end_sector; s++) {
            uint8_t temp[512];
            uint64_t sector_lba = cluster_lba + s;

            // Read-modify-write
            if (block_device_read(fs->dev, sector_lba, 1, temp) != 0)
                return -1;

            uint32_t copy_start = (s == start_sector) ? (uint32_t)(cluster_offset % fs->bytes_per_sector) : 0;
            uint32_t copy_size = fs->bytes_per_sector - copy_start;
            if (copy_size > to_write - written_in_cluster)
                copy_size = (uint32_t)(to_write - written_in_cluster);

            memcpy(temp + copy_start, src, copy_size);
            if (block_device_write(fs->dev, sector_lba, 1, temp) != 0)
                return -1;

            src += copy_size;
            written_in_cluster += copy_size;
            bytes_remaining -= copy_size;
            if (bytes_remaining == 0) break;
        }

        file_offset = 0;  // subsequent clusters start at offset 0
        prev_cluster = cluster;
        cluster = fat32_read_fat_entry(fs, cluster);
    }

    return (int)(size - bytes_remaining);
}

// ── Locate a directory entry by index and return its disk position ──
// On success, sets *sector_lba and *entry_off so the caller can
// write to that location.  Returns 0 on success, -1 on error.
// If the entry at `index` is past the end of the directory,
// *sector_lba and *entry_off point to the first free slot (0x00 marker).
static int fat32_locate_entry(fat32_fs_t *fs, uint32_t dir_cluster,
                              uint64_t index, uint64_t *out_lba,
                              uint32_t *out_off)
{
    uint32_t entries_per_cluster = (fs->sectors_per_cluster * fs->bytes_per_sector) / 32;
    uint64_t remaining = index;
    uint32_t cluster = dir_cluster;

    while (cluster < FAT32_EOC_MIN) {
        if (remaining < entries_per_cluster) {
            uint64_t offset_in_cluster = remaining * 32;
            uint32_t sector_in_cluster = (uint32_t)(offset_in_cluster / fs->bytes_per_sector);
            uint32_t offset_in_sector  = offset_in_cluster % fs->bytes_per_sector;

            *out_lba = fat32_cluster_to_sector(fs, cluster) + sector_in_cluster;
            *out_off = offset_in_sector;
            return 0;
        }
        remaining -= entries_per_cluster;
        cluster = fat32_read_fat_entry(fs, cluster);
    }
    return -1; // index beyond directory
}

// ── Find a free directory slot ────────────────────────────
// Scans the directory cluster chain for the first free entry
// (0x00 = never used, or 0xE5 = deleted).
// Returns the index, or -1 if directory is full.
static int64_t fat32_find_free_slot(fat32_fs_t *fs, uint32_t dir_cluster)
{
    uint32_t entries_per_cluster = (fs->sectors_per_cluster * fs->bytes_per_sector) / 32;
    uint64_t index = 0;
    uint32_t cluster = dir_cluster;

    while (cluster < FAT32_EOC_MIN) {
        for (uint64_t local = 0; local < entries_per_cluster; local++) {
            uint64_t offset_in_cluster = local * 32;
            uint32_t sector_in_cluster = (uint32_t)(offset_in_cluster / fs->bytes_per_sector);
            uint32_t offset_in_sector  = offset_in_cluster % fs->bytes_per_sector;

            uint64_t sector_lba = fat32_cluster_to_sector(fs, cluster) + sector_in_cluster;
            uint8_t sector[512];
            if (block_device_read(fs->dev, sector_lba, 1, sector) != 0)
                return -1;

            uint8_t first_byte = sector[offset_in_sector];
            if (first_byte == 0x00 || first_byte == 0xE5)
                return (int64_t)index;

            index++;
        }
        cluster = fat32_read_fat_entry(fs, cluster);
    }
    return -1;
}

// ── Generate an 8.3 name from a user-supplied filename ────
static void fat32_make_83_name(const char *name, uint8_t out[11])
{
    // Fill with spaces
    for (int i = 0; i < 11; i++) out[i] = ' ';

    // Find extension separator
    const char *dot = NULL;
    int name_len = 0;
    while (name[name_len] != '\0' && name[name_len] != '.')
        name_len++;

    if (name[name_len] == '.') {
        dot = &name[name_len];
        name_len = (int)(dot - name);
    }

    // Copy name (up to 8 chars, uppercase)
    int n = name_len;
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        out[i] = (uint8_t)c;
    }

    // Copy extension (up to 3 chars, uppercase)
    if (dot && dot[1] != '\0') {
        const char *ext = dot + 1;
        n = 0;
        while (ext[n] != '\0' && n < 3) n++;
        if (n > 3) n = 3;
        for (int i = 0; i < n; i++) {
            char c = ext[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            out[8 + i] = (uint8_t)c;
        }
    }
}

// ── Write a directory entry at a specific location ────────
static int fat32_write_entry_at(fat32_fs_t *fs, uint64_t lba, uint32_t off,
                                const FAT32_DIRENT *dirent)
{
    uint8_t sector[512];
    if (block_device_read(fs->dev, lba, 1, sector) != 0)
        return -1;
    memcpy(sector + off, dirent, sizeof(FAT32_DIRENT));
    return block_device_write(fs->dev, lba, 1, sector);
}

// ── Read a directory entry at a specific location ─────────
static int fat32_read_entry_at(fat32_fs_t *fs, uint64_t lba, uint32_t off,
                               FAT32_DIRENT *dirent)
{
    uint8_t sector[512];
    if (block_device_read(fs->dev, lba, 1, sector) != 0)
        return -1;
    memcpy(dirent, sector + off, sizeof(FAT32_DIRENT));
    return 0;
}

// ── Find a directory entry by name ────────────────────────
// Returns the index if found, or -1 if not found.
// If found, optionally fills out_lba/out_off for later modification.
static int64_t fat32_find_by_name(fat32_fs_t *fs, uint32_t dir_cluster,
                                  const char *name,
                                  uint64_t *out_lba, uint32_t *out_off)
{
    uint8_t target[11];
    fat32_make_83_name(name, target);

    uint32_t entries_per_cluster = (fs->sectors_per_cluster * fs->bytes_per_sector) / 32;
    uint64_t index = 0;
    uint32_t cluster = dir_cluster;

    while (cluster < FAT32_EOC_MIN) {
        for (uint64_t local = 0; local < entries_per_cluster; local++) {
            uint64_t offset_in_cluster = local * 32;
            uint32_t sector_in_cluster = (uint32_t)(offset_in_cluster / fs->bytes_per_sector);
            uint32_t offset_in_sector  = offset_in_cluster % fs->bytes_per_sector;

            uint64_t sector_lba = fat32_cluster_to_sector(fs, cluster) + sector_in_cluster;
            uint8_t sector[512];
            if (block_device_read(fs->dev, sector_lba, 1, sector) != 0)
                return -1;

            uint8_t first_byte = sector[offset_in_sector];
            if (first_byte == 0x00) break; // end of directory
            if (first_byte == 0xE5) { index++; continue; } // deleted

            if (sector[offset_in_sector + 0x0B] == FAT_ATTR_LFN) {
                index++;
                continue; // skip LFN
            }

            // Compare 11-byte name
            if (memcmp(sector + offset_in_sector, target, 11) == 0) {
                if (out_lba) *out_lba = sector_lba;
                if (out_off) *out_off = offset_in_sector;
                return (int64_t)index;
            }
            index++;
        }
        cluster = fat32_read_fat_entry(fs, cluster);
    }
    return -1;
}

// ── Create a new directory entry ──────────────────────────
// Returns the first cluster of the new file (0 if just a directory entry
// with no allocated space), or 0 on error (file already exists).
static uint32_t fat32_create_entry(fat32_fs_t *fs, uint32_t dir_cluster,
                                    const char *name, uint8_t attr)
{
    // Check if name already exists
    if (fat32_find_by_name(fs, dir_cluster, name, NULL, NULL) >= 0)
        return 0; // already exists

    // Find a free slot
    int64_t free_idx = fat32_find_free_slot(fs, dir_cluster);
    if (free_idx < 0) return 0;

    // Get the disk location of that slot
    uint64_t lba;
    uint32_t off;
    if (fat32_locate_entry(fs, dir_cluster, (uint64_t)free_idx, &lba, &off) != 0)
        return 0;

    // Build the directory entry
    FAT32_DIRENT dirent;
    memset(&dirent, 0, sizeof(dirent));
    fat32_make_83_name(name, dirent.name);
    dirent.attr = attr;
    dirent.first_cluster_lo = 0;
    dirent.first_cluster_hi = 0;
    dirent.file_size = 0;

    // Write it
    if (fat32_write_entry_at(fs, lba, off, &dirent) != 0)
        return 0;

    serial_printk("FAT: created '%s' at index %ld\n", name, (long)free_idx);

    // Return a non-zero "ino" — for new files we use the entry index
    // as a temporary identifier. The first data cluster will be allocated
    // on first write.  For the VFS node we store the dir_entry LBA + offset
    // so fat_write/update can find it.
    // Actually, for simplicity, return 1 to indicate success.
    return 1;
}

// ── Update an existing directory entry ────────────────────
// Given a vfs_node, writes back current size and first cluster.
static int fat32_update_entry(fat32_fs_t *fs, vfs_node_t *node)
{
    if (!node || !node->parent) return -1;

    // Find the entry by name in the parent directory
    uint32_t dir_cluster = node->parent->fs_data
        ? (uint32_t)(uintptr_t)node->parent->fs_data
        : fs->root_cluster;

    uint64_t lba;
    uint32_t off;
    int64_t idx = fat32_find_by_name(fs, dir_cluster, node->name, &lba, &off);
    if (idx < 0) {
        serial_printk("FAT: update_entry: '%s' not found in parent\n", node->name);
        return -1;
    }

    FAT32_DIRENT dirent;
    if (fat32_read_entry_at(fs, lba, off, &dirent) != 0)
        return -1;

    uint32_t first_cl = (uint32_t)(uintptr_t)node->fs_data;
    dirent.first_cluster_lo = (uint16_t)(first_cl & 0xFFFF);
    dirent.first_cluster_hi = (uint16_t)((first_cl >> 16) & 0xFFFF);
    dirent.file_size = (uint32_t)node->size;

    return fat32_write_entry_at(fs, lba, off, &dirent);
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

static int fat_write(struct vfs_node *node, uint64_t offset,
                     uint64_t size, void *buffer)
{
    if (!node || !node->mount) return -1;
    fat32_fs_t *fs = (fat32_fs_t *)node->mount->fs_data;

    // For newly created empty files (fs_data == 0), allocate first cluster
    if (node->fs_data == NULL) {
        uint32_t cl = fat32_alloc_cluster(fs, 0);
        if (cl == 0) return -1;
        node->fs_data = (void *)(uintptr_t)cl;
    }

    uint32_t cluster = (uint32_t)(uintptr_t)node->fs_data;
    int written = fat32_write_data(fs, cluster, offset, size, buffer);

    // Update file size if we extended it
    if (written > 0 && offset + (uint64_t)written > node->size) {
        node->size = offset + (uint64_t)written;
        fat32_update_entry(fs, node);
    }

    return written;
}

static int fat_readdir(struct vfs_node *node, uint64_t index,
                       vfs_dirent_t *entry)
{
    if (!node || !node->mount) return -1;
    fat32_fs_t *fs = (fat32_fs_t *)node->mount->fs_data;

    uint32_t dir_cluster;
    if (node->fs_data)
        dir_cluster = (uint32_t)(uintptr_t)node->fs_data;
    else
        dir_cluster = fs->root_cluster;

    return fat32_read_entry(fs, dir_cluster, index, entry);
}

// ── Create a new regular file ─────────────────────────────
// Returns a vfs_node_t* or NULL on error.
struct vfs_node *fat_create(vfs_node_t *dir, const char *name)
{
    if (!dir || !dir->mount || !name) return NULL;
    fat32_fs_t *fs = (fat32_fs_t *)dir->mount->fs_data;

    uint32_t dir_cluster;
    if (dir->fs_data)
        dir_cluster = (uint32_t)(uintptr_t)dir->fs_data;
    else
        dir_cluster = fs->root_cluster;

    // Create the directory entry
    if (fat32_create_entry(fs, dir_cluster, name, FAT_ATTR_ARCHIVE) == 0)
        return NULL;

    // Allocate a vfs_node for the caller (no data cluster yet)
    vfs_node_t *node = (vfs_node_t *)calloc(sizeof(vfs_node_t));
    if (!node) return NULL;

    size_t nlen = strlen(name);
    if (nlen >= VFS_NAME_MAX) nlen = VFS_NAME_MAX - 1;
    memcpy(node->name, name, nlen);
    node->name[nlen] = '\0';
    node->type = VFS_FILE;
    node->mount = dir->mount;
    node->parent = dir;
    node->ops = dir->ops;
    node->fs_data = NULL;  // no cluster yet — allocated on first write
    node->size = 0;
    node->refcount = 1;

    return node;
}

struct vfs_ops fat_vfs_ops = {
    .read    = fat_read,
    .write   = fat_write,
    .readdir = fat_readdir,
    .create  = fat_create,
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
