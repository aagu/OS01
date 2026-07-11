#ifndef _FS_GPT_H
#define _FS_GPT_H

#include <stdint.h>
#include <block/blockdev.h>

#define GPT_PARTITION_MAX  16

typedef struct gpt_partition {
    char            name[40];
    uint8_t         type_guid[16];
    uint64_t        start_lba;
    uint64_t        end_lba;
    block_device_t *parent;
    block_device_t *dev;
} gpt_partition_t;

typedef struct gpt_info {
    gpt_partition_t partitions[GPT_PARTITION_MAX];
    int             count;
} gpt_info_t;

gpt_info_t *gpt_scan(block_device_t *disk);

#endif
