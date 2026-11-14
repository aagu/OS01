#ifndef _FS_GPT_H
#define _FS_GPT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <block/blockdev.h>

#define GPT_PARTITION_MAX  16

typedef struct gpt_partition {
    char            name[40];
    uint8_t         type_guid[16];
    uint8_t         unique_guid[16];
    uint32_t        entry_number;
    uint64_t        start_lba;
    uint64_t        end_lba;
    block_device_t *parent;
    block_device_t *dev;
} gpt_partition_t;

typedef struct gpt_info {
    gpt_partition_t partitions[GPT_PARTITION_MAX];
    int             count;
} gpt_info_t;

struct gpt_table;

int gpt_read_table(block_device_t *disk, struct gpt_table **out);
void gpt_free_table(struct gpt_table *t);
size_t gpt_table_count(const struct gpt_table *t);
bool gpt_table_entry(const struct gpt_table *t, size_t ordinal,
                     gpt_partition_t *out);
gpt_info_t *gpt_publish_table(const struct gpt_table *t, bool strict);

gpt_info_t *gpt_scan(block_device_t *disk);

#endif
