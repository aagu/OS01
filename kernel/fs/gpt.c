// kernel/fs/gpt.c
#include <fs/gpt.h>
#include <fs/devfs.h>
#include <block/blockdev.h>
#include <core/debug.h>
#include <log/log.h>
#include <memory/slab.h>     // kmalloc, kfree
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

#include <errno.h>

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
    if (!ctx || !ctx->parent) return -EINVAL;
    if (lba + count < lba || lba + count > ctx->length) {
        debug_block("gpt: read past end of partition\n");
        return -EINVAL;
    }
    return block_device_read(ctx->parent, ctx->offset_lba + lba, count, buf);
}

static int partition_write(block_device_t *dev, uint64_t lba,
                           uint32_t count, const void *buf)
{
    partition_ctx_t *ctx = (partition_ctx_t *)dev->private_data;
    if (!ctx || !ctx->parent) return -EINVAL;
    if (lba + count < lba || lba + count > ctx->length) {
        debug_block("gpt: write past end of partition\n");
        return -EINVAL;
    }
    return block_device_write(ctx->parent, ctx->offset_lba + lba, count, buf);
}

static int partition_flush(block_device_t *dev)
{
    partition_ctx_t *ctx = (partition_ctx_t *)dev->private_data;
    if (!ctx || !ctx->parent) return -EINVAL;
    return block_device_flush(ctx->parent);
}

static const struct block_device_ops partition_ops = {
    .read  = partition_read,
    .write = partition_write,
    .flush = partition_flush,
};

// Create a partition wrapper block device.
// Name: parent name + partition index (1-based), e.g. "hda1", "hda2".
static block_device_t *block_device_create_partition(
    block_device_t *parent, uint64_t offset_lba, uint64_t length, int part_idx)
{
    if (!parent || parent->sector_size != 512) return NULL;
    if (offset_lba + length < offset_lba || offset_lba + length > parent->sector_count)
        return NULL;

    partition_ctx_t *ctx = kmalloc(sizeof(partition_ctx_t));
    if (!ctx) return NULL;
    ctx->parent     = parent;
    ctx->offset_lba = offset_lba;
    ctx->length     = length;

    // Build name: parent->name + partition index (1-based)
    char name[BLOCKDEV_NAME_MAX];
    int nlen = strlen(parent->name);
    memcpy(name, parent->name, nlen);
    int digit_start = nlen;
    int p = part_idx;
    // Convert part_idx to string
    char tmp[8]; int ti = 0;
    do { tmp[ti++] = '0' + (p % 10); p /= 10; } while (p > 0);
    while (ti > 0 && digit_start < BLOCKDEV_NAME_MAX - 1) name[digit_start++] = tmp[--ti];
    name[digit_start] = '\0';

    struct block_device_desc desc = {
        .name = name,
        .sector_count = length,
        .sector_size = 512,
        .ops = &partition_ops,
        .private_data = ctx,
        .parent = parent,
        .kind = BLOCK_PARTITION,
    };

    block_device_t *dev = NULL;
    int rc = block_device_register(&desc, &dev);
    if (rc != 0 || !dev) {
        kfree(ctx);
        return NULL;
    }

    return dev;
}


// ── GPT helpers ──────────────────────────────────────────
static uint32_t gpt_read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t gpt_read_le64(const uint8_t *p)
{
    uint64_t val = 0;
    for (int i = 0; i < 8; i++)
        val |= (uint64_t)p[i] << (8 * i);
    return val;
}

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

struct gpt_table {
    block_device_t *disk;
    uint8_t        *entries;
    uint32_t        num_entries;
    uint32_t        entry_size;
    uint32_t       *valid_slots;
    size_t          valid_count;
};

static int gpt_read_table_internal(block_device_t *disk, bool strict_entries,
                                   struct gpt_table **out)
{
    if (!out)
        return -EINVAL;
    *out = NULL;
    if (!disk || !disk->present)
        return -EINVAL;
    if (disk->sector_size != 512)
        return -E2BIG;
    if (disk->sector_count < 2)
        return -ENOENT;

    uint8_t *hdr = kmalloc(512);
    if (!hdr)
        return -ENOMEM;

    if (block_device_read(disk, 1, 1, hdr) != 0) {
        debug_block("gpt: failed to read header\n");
        kfree(hdr);
        return -EIO;
    }
    if (memcmp(hdr, "EFI PART", 8) != 0) {
        debug_block("gpt: no EFI PART signature\n");
        kfree(hdr);
        return -ENOENT;
    }

    uint32_t revision = gpt_read_le32(hdr + 8);
    if (revision != 0x00010000u) {
        debug_block("gpt: unsupported revision %#x\n", revision);
        kfree(hdr);
        return -E2BIG;
    }

    uint32_t header_size        = gpt_read_le32(hdr + 12);
    uint32_t hdr_crc_stored     = gpt_read_le32(hdr + 16);
    uint64_t entry_lba          = gpt_read_le64(hdr + 72);
    uint32_t num_entries        = gpt_read_le32(hdr + 80);
    uint32_t entry_size         = gpt_read_le32(hdr + 84);
    uint32_t entries_crc_stored = gpt_read_le32(hdr + 88);

    if (header_size < 92u || header_size > 512u ||
        num_entries == 0u || entry_size < 128u) {
        debug_block("gpt: bad header/entry size\n");
        kfree(hdr);
        return -EINVAL;
    }

    uint64_t array_size = (uint64_t)num_entries * (uint64_t)entry_size;
    if (array_size > 1024u * 1024u) {
        debug_block("gpt: partition table too large\n");
        kfree(hdr);
        return -E2BIG;
    }

    uint64_t array_sectors = (array_size + 511u) / 512u;
    if (entry_lba < 2u ||
        entry_lba + array_sectors < entry_lba ||
        entry_lba + array_sectors > disk->sector_count) {
        debug_block("gpt: entry array out of disk bounds\n");
        kfree(hdr);
        return -EINVAL;
    }

    memset(hdr + 16, 0, 4);
    uint32_t crc_computed = gpt_crc32(hdr, header_size);
    kfree(hdr);
    if (crc_computed != hdr_crc_stored) {
        debug_block("gpt: header CRC mismatch\n");
        return -EINVAL;
    }

    uint8_t *entries = kmalloc((size_t)(array_sectors * 512u));
    if (!entries)
        return -ENOMEM;

    if (block_device_read(disk, entry_lba, (uint32_t)array_sectors, entries) != 0) {
        debug_block("gpt: failed to read partition entries\n");
        kfree(entries);
        return -EIO;
    }

    uint32_t entries_crc_computed = gpt_crc32(entries, (size_t)array_size);
    if (entries_crc_computed != entries_crc_stored) {
        debug_block("gpt: partition entries CRC mismatch\n");
        kfree(entries);
        return -EINVAL;
    }

    size_t valid_count = 0;
    for (uint32_t i = 0; i < num_entries; i++) {
        const uint8_t *entry = entries + (size_t)i * entry_size;
        if (guid_is_zero(entry))
            continue;

        uint64_t start_lba = gpt_read_le64(entry + 32);
        uint64_t end_lba   = gpt_read_le64(entry + 40);
        if (end_lba < start_lba || end_lba >= disk->sector_count) {
            if (strict_entries) {
                kfree(entries);
                return -EINVAL;
            }
            continue;
        }
        valid_count++;
    }

    struct gpt_table *t = kmalloc(sizeof(*t));
    if (!t) {
        kfree(entries);
        return -ENOMEM;
    }

    uint32_t *slots = NULL;
    if (valid_count > 0) {
        slots = kmalloc(valid_count * sizeof(uint32_t));
        if (!slots) {
            kfree(t);
            kfree(entries);
            return -ENOMEM;
        }
        size_t pos = 0;
        for (uint32_t i = 0; i < num_entries; i++) {
            const uint8_t *entry = entries + (size_t)i * entry_size;
            if (guid_is_zero(entry))
                continue;
            uint64_t start_lba = gpt_read_le64(entry + 32);
            uint64_t end_lba   = gpt_read_le64(entry + 40);
            if (end_lba < start_lba || end_lba >= disk->sector_count)
                continue;
            slots[pos++] = i;
        }
    }

    t->disk = disk;
    t->entries = entries;
    t->num_entries = num_entries;
    t->entry_size = entry_size;
    t->valid_slots = slots;
    t->valid_count = valid_count;
    *out = t;
    return 0;
}

int gpt_read_table(block_device_t *disk, struct gpt_table **out)
{
    return gpt_read_table_internal(disk, true, out);
}

void gpt_free_table(struct gpt_table *t)
{
    if (!t)
        return;
    kfree(t->valid_slots);
    kfree(t->entries);
    kfree(t);
}

size_t gpt_table_count(const struct gpt_table *t)
{
    return t ? t->valid_count : 0;
}

bool gpt_table_entry(const struct gpt_table *t, size_t ordinal,
                     gpt_partition_t *out)
{
    if (!t || !out || ordinal >= t->valid_count)
        return false;

    uint32_t slot = t->valid_slots[ordinal];
    const uint8_t *entry = t->entries + (size_t)slot * t->entry_size;

    memset(out, 0, sizeof(*out));
    memcpy(out->type_guid, entry, 16);
    memcpy(out->unique_guid, entry + 16, 16);
    out->entry_number = slot + 1u;
    out->start_lba = gpt_read_le64(entry + 32);
    out->end_lba = gpt_read_le64(entry + 40);
    out->parent = t->disk;
    out->dev = NULL;
    gpt_extract_name(entry, (int)t->entry_size, out->name, sizeof(out->name));
    return true;
}

static void gpt_rollback_published(gpt_info_t *info)
{
    for (int i = 0; i < info->count; i++) {
        block_device_t *dev = info->partitions[i].dev;
        if (dev) {
            void *ctx = dev->private_data;
            block_device_unregister_boot(dev);
            kfree(ctx);
            info->partitions[i].dev = NULL;
        }
    }
    info->count = 0;
}

gpt_info_t *gpt_publish_table(const struct gpt_table *t, bool strict)
{
    if (!t || !t->disk)
        return NULL;

    gpt_info_t *info = kmalloc(sizeof(gpt_info_t));
    if (!info)
        return NULL;
    memset(info, 0, sizeof(*info));

    int part_idx = 1;
    for (size_t i = 0; i < t->valid_count && info->count < GPT_PARTITION_MAX; i++) {
        gpt_partition_t part;
        if (!gpt_table_entry(t, i, &part))
            continue;

        uint64_t length = part.end_lba - part.start_lba + 1u;
        part.dev = block_device_create_partition(t->disk, part.start_lba,
                                                 length, part_idx++);
        if (!part.dev) {
            debug_block("gpt: failed to create partition device\n");
            if (strict) {
                gpt_rollback_published(info);
                kfree(info);
                return NULL;
            }
            continue;
        }

        int ret = devfs_register_blkdev(part.dev->name, part.dev);
        if (ret != 0 && (strict || ret != -EEXIST)) {
            log_err("gpt: failed to register partition '%s' in devfs (%d)\n",
                    part.dev->name, ret);
            void *ctx = part.dev->private_data;
            block_device_unregister_boot(part.dev);
            kfree(ctx);
            part.dev = NULL;
            if (strict) {
                gpt_rollback_published(info);
                kfree(info);
                return NULL;
            }
            continue;
        }

        debug_block("gpt: partition '%s' LBA %lu-%lu (%lu sectors)\n",
                    part.name, part.start_lba, part.end_lba, length);
        info->partitions[info->count++] = part;
    }

    debug_block("gpt: found %d partitions\n", info->count);
    return info;
}

// ── gpt_scan — compatibility entry point ─────────────────
gpt_info_t *gpt_scan(block_device_t *disk)
{
    struct gpt_table *t = NULL;
    if (gpt_read_table_internal(disk, false, &t) != 0)
        return NULL;

    gpt_info_t *info = gpt_publish_table(t, false);
    gpt_free_table(t);
    return info;
}

#ifdef OS01_SELFTEST
// Test registered by selftest_run_all() via forward declaration in selftest.c

int gpt_selftest_crc32(void)
{
    // CRC32("123456789") = 0xCBF43926 (standard test vector)
    const char *test = "123456789";
    uint32_t crc = gpt_crc32((const uint8_t *)test, 9);
    if (crc != 0xCBF43926) return -1;
    return 0;
}
#endif
