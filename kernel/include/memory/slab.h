#ifndef _KERNEL_SLAB_H
#define _KERNEL_SLAB_H

#include <memory/pmm.h>
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

/* slab_layout_compute() is a pure function: it reads kmalloc_cache_size[].size
 * and reports the metadata byte total plus the 8 reserved 2 MiB pages.
 * Per spec §3.2 the formula is:
 *   meta = Σ_{i=0..15} [ sizeof(struct Slab) + 10*sizeof(long)
 *                      + align8(PAGE_2M / size_i / 8) + 10*sizeof(long) ]
 * Implementation lives in kernel/memory/slab.c; declared here as inline
 * so hosttests can compile without dragging in slab.c's spinlock/irq deps.
 */
struct slab_layout {
    uint64_t meta_bytes;
    uint64_t reserved_2m_pages;
};

static inline struct slab_layout slab_layout_compute(void) {
    struct slab_layout l;
    uint64_t meta = 0;
    for (int i = 0; i < 16; i++) {
        uint64_t entries = (uint64_t)PAGE_2M_SIZE / kmalloc_cache_size[i].size;
        /* align8: (entries/8 + 7) / 8 * 8 */
        uint64_t bm = ((entries / 8 + 7) / 8) * 8;
        meta += sizeof(struct Slab) + 10 * sizeof(long)
              + bm + 10 * sizeof(long);
    }
    l.meta_bytes = meta;
    l.reserved_2m_pages = 8;
    return l;
}

void * kmalloc(size_t size);
void * kzalloc(size_t size);
size_t ksize(void * address);
size_t kfree(void * address);
size_t slab_init();

#endif
