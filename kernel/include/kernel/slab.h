#ifndef _KERNEL_SLAB_H
#define _KERNEL_SLAB_H

#include <kernel/pmm.h>
#include <list.h>

struct Slab
{
    struct List list;
    struct Page * page;

    uint64_t using_count;
    uint64_t free_count;

    void * address;

    uint64_t color_length;
    uint64_t color_count;

    uint64_t * color_map;
};

struct Slab_Cache
{
    uint64_t size;
    uint64_t total_using;
    uint64_t total_free;
    struct Slab * cache_pool;
    struct Slab * cache_dma_pool;
    void *(* contructor)(void * Vaddress, uint64_t arg);
    void *(* destructor)(void * Vaddress, uint64_t arg);
};

extern struct Slab_Cache kmalloc_cache_size[16];

#endif