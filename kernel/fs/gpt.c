// kernel/fs/gpt.c
#include <fs/gpt.h>
#include <fs/devfs.h>
#include <block/blockdev.h>
#include <kernel/debug.h>
#include <kernel/slab.h>     // kmalloc, kfree
#include <string.h>
#include <stdlib.h>          // calloc

// ── CRC32 (standard reflected, polynomial 0xEDB88320) ────
static uint32_t gpt_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320 : 0);
    }
    return crc ^ 0xFFFFFFFF;
}

// ── Partition block device wrapper ──────────────────────
typedef struct partition_ctx {
    block_device_t *parent;
    uint64_t        offset_lba;
    uint64_t        length;
} partition_ctx_t;

static int partition_read(block_device_t *dev, uint64_t lba,
                          uint32_t count, void *buf)
{
    partition_ctx_t *ctx = (partition_ctx_t *)dev->private_data;
    if (lba + count > ctx->length) {
        debug_block("gpt: read past end of partition\n");
        return -1;
    }
    return ctx->parent->read(ctx->parent, ctx->offset_lba + lba, count, buf);
}

static int partition_write(block_device_t *dev, uint64_t lba,
                           uint32_t count, const void *buf)
{
    partition_ctx_t *ctx = (partition_ctx_t *)dev->private_data;
    if (lba + count > ctx->length) {
        debug_block("gpt: write past end of partition\n");
        return -1;
    }
    return ctx->parent->write(ctx->parent, ctx->offset_lba + lba, count, buf);
}

// Create a partition wrapper block device.
// Name: parent name + partition index (1-based), e.g. "hda1", "hda2".
static block_device_t *block_device_create_partition(
    block_device_t *parent, uint64_t offset_lba, uint64_t length, int part_idx)
{
    partition_ctx_t *ctx = kmalloc(sizeof(partition_ctx_t));
    if (!ctx) return NULL;
    ctx->parent     = parent;
    ctx->offset_lba = offset_lba;
    ctx->length     = length;

    // Build name: parent->name + partition index (1-based)
    char name[16];
    int nlen = strlen(parent->name);
    memcpy(name, parent->name, nlen);
    int digit_start = nlen;
    int p = part_idx;
    // Convert part_idx to string
    char tmp[8]; int ti = 0;
    do { tmp[ti++] = '0' + (p % 10); p /= 10; } while (p > 0);
    while (ti > 0) name[digit_start++] = tmp[--ti];
    name[digit_start] = '\0';

    block_device_t *dev = block_device_register_raw(name, length, ctx);
    if (!dev) { kfree(ctx); return NULL; }

    // Set custom hooks AFTER register_raw (which does not overwrite)
    dev->read  = partition_read;
    dev->write = partition_write;
    return dev;
}

// ── GPT helpers ──────────────────────────────────────────
static int guid_is_zero(const uint8_t *guid)
{
    for (int i = 0; i < 16; i++)
        if (guid[i] != 0) return 0;
    return 1;
}

static void gpt_extract_name(const uint8_t *entry, int entry_size,
                              char *out, size_t out_len)
{
    if (entry_size < 128) { out[0] = '\0'; return; }
    const uint8_t *name_field = entry + 56;
    size_t pos = 0;
    for (int i = 0; i < 36 && pos < out_len - 1; i++) {
        uint16_t wc = (uint16_t)name_field[i * 2]
                    | ((uint16_t)name_field[i * 2 + 1] << 8);
        if (wc == 0) break;
        out[pos++] = (wc < 0x80) ? (char)wc : '?';
    }
    out[pos] = '\0';
}

// ── gpt_scan — main entry point ──────────────────────────
gpt_info_t *gpt_scan(block_device_t *disk)
{
    if (!disk || !disk->present) return NULL;

    // Phase 1: Read GPT header (LBA 1)
    uint8_t hdr[512];
    if (block_device_read(disk, 1, 1, hdr) != 0) {
        debug_block("gpt: failed to read header\n");
        return NULL;
    }
    if (memcmp(hdr, "EFI PART", 8) != 0) {
        debug_block("gpt: no EFI PART signature\n");
        return NULL;
    }
    uint32_t revision = *(uint32_t *)(hdr + 8);
    if (revision != 0x00010000) {
        debug_block("gpt: unsupported revision %#x\n", revision);
        return NULL;
    }

    // Phase 2: Dynamic parameters from header (don't hardcode LBA 2 / 128 / 128!)
    uint32_t header_size    = *(uint32_t *)(hdr + 12);
    uint64_t entry_lba      = *(uint64_t *)(hdr + 72);
    uint32_t num_entries    = *(uint32_t *)(hdr + 80);
    uint32_t entry_size     = *(uint32_t *)(hdr + 84);
    uint32_t hdr_crc_stored = *(uint32_t *)(hdr + 16);

    if (header_size < 92 || entry_size < 128) {
        debug_block("gpt: bad header/entry size\n");
        return NULL;
    }
    uint64_t array_size = (uint64_t)num_entries * entry_size;
    if (array_size > 1024 * 1024) {  // sanity: reject > 1MB partition table
        debug_block("gpt: partition table too large\n");
        return NULL;
    }

    // Phase 3: Validate header CRC32
    uint32_t crc_saved = hdr_crc_stored;
    memset(hdr + 16, 0, 4);  // zero out CRC field for computation
    uint32_t crc_computed = gpt_crc32(hdr, header_size);
    if (crc_computed != crc_saved) {
        debug_block("gpt: header CRC mismatch\n");
        return NULL;
    }

    // Phase 4: Read + validate partition entry array
    uint32_t array_sectors = (uint32_t)((array_size + 511) / 512);
    uint8_t *entries = kmalloc(array_sectors * 512);
    if (!entries) return NULL;
    if (block_device_read(disk, entry_lba, array_sectors, entries) != 0) {
        debug_block("gpt: failed to read partition entries\n");
        kfree(entries); return NULL;
    }
    uint32_t entries_crc_stored = *(uint32_t *)(hdr + 88);
    uint32_t entries_crc_computed = gpt_crc32(entries, (uint32_t)array_size);
    if (entries_crc_computed != entries_crc_stored) {
        debug_block("gpt: partition entries CRC mismatch\n");
        kfree(entries); return NULL;
    }

    // Phase 5: Allocate result + enumerate partitions
    gpt_info_t *info = calloc(1, sizeof(gpt_info_t));
    if (!info) { kfree(entries); return NULL; }

    int part_idx = 1;  // 1-based partition index for naming
    for (uint32_t i = 0; i < num_entries && info->count < GPT_PARTITION_MAX; i++) {
        uint8_t *entry = entries + (uint64_t)i * entry_size;
        if (guid_is_zero(entry)) continue;

        uint64_t start_lba = *(uint64_t *)(entry + 32);
        uint64_t end_lba   = *(uint64_t *)(entry + 40);
        if (end_lba < start_lba) continue;

        uint64_t length = end_lba - start_lba + 1;
        gpt_partition_t *part = &info->partitions[info->count];

        memcpy(part->type_guid, entry, 16);
        part->start_lba = start_lba;
        part->end_lba   = end_lba;
        part->parent    = disk;
        gpt_extract_name(entry, (int)entry_size, part->name, sizeof(part->name));

        part->dev = block_device_create_partition(disk, start_lba, length, part_idx++);
        if (!part->dev) {
            debug_block("gpt: failed to create partition device\n");
            continue;
        }

        devfs_register_blkdev(part->dev->name, part->dev);

        debug_block("gpt: partition '%s' LBA %lu-%lu (%lu sectors)\n",
                    part->name, start_lba, end_lba, length);
        info->count++;
    }

    kfree(entries);
    debug_block("gpt: found %d partitions\n", info->count);
    return info;
}
