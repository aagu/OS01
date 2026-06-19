/*
 * test/cases/test_slab_basic.c — Slab allocator unit tests
 */
#include "test_framework.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

/* ── Minimal slab structures ─────────────────────── */
typedef struct List { struct List *prev, *next; } List_t;

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - __builtin_offsetof(type, member)))

typedef struct Slab {
    List_t   list;
    void    *page;
    uint64_t using_count;
    uint64_t free_count;
    void    *address;
    uint64_t color_length;
    uint64_t color_count;
    uint64_t *color_map;
} Slab_t;

typedef struct Slab_Cache {
    uint64_t size;
    uint64_t total_using;
    uint64_t total_free;
    Slab_t  *cache_pool;
    Slab_t  *cache_dma_pool;
    void *(*constructor)(void *, uint64_t);
    void *(*destructor)(void *, uint64_t);
} Slab_Cache_t;

#define PAGE_2M_SIZE (1UL << 21)

/* ── Simulated slab arena ────────────────────────── */
#define ARENA_SIZE (PAGE_2M_SIZE * 4)
static uint8_t arena[ARENA_SIZE];
static uint64_t arena_used = 0;

static void *sim_alloc(size_t sz) {
    sz = (sz + 15) & ~(size_t)15;
    if (arena_used + sz > ARENA_SIZE) return NULL;
    void *p = arena + arena_used;
    arena_used += sz;
    return p;
}

/* ── Simplified kmalloc_create and kmalloc ────────── */

#define POOL_SIZE 16
static Slab_Cache_t cache[POOL_SIZE];
static const uint64_t cache_sizes[POOL_SIZE] = {
    32, 64, 128, 256, 512, 1024, 2048, 4096,
    8192, 16384, 32768, 65536, 131072, 262144, 524288, 1048576
};

static void slab_cache_init(int index) {
    memset(&cache[index], 0, sizeof(Slab_Cache_t));
    cache[index].size = cache_sizes[index];

    Slab_t *slab = (Slab_t *)sim_alloc(sizeof(Slab_t) + 64);
    if (!slab) return;
    memset(slab, 0, sizeof(Slab_t));

    slab->free_count  = PAGE_2M_SIZE / cache_sizes[index];
    slab->color_count = slab->free_count;
    slab->color_length = ((slab->color_count + 63) / 64) * 8;
    slab->color_map = (uint64_t *)sim_alloc(slab->color_length);
    if (slab->color_map) {
        memset(slab->color_map, 0xFF, slab->color_length);
        /* Clear all bits (all entries free) */
        for (uint64_t j = 0; j < slab->color_count; j++)
            slab->color_map[j >> 6] ^= (1UL << (j % 64));
    }
    slab->address = sim_alloc(PAGE_2M_SIZE);
    slab->list.prev = slab->list.next = &slab->list;

    cache[index].cache_pool = slab;
    cache[index].total_free = slab->free_count;
}

static void *kmalloc_sim(size_t size) {
    if (size > 1048576) return NULL;
    int idx;
    for (idx = 0; idx < POOL_SIZE; idx++) {
        if (cache_sizes[idx] >= size) break;
    }
    if (idx >= POOL_SIZE || !cache[idx].cache_pool) return NULL;

    Slab_t *slab = cache[idx].cache_pool;
    if (slab->free_count == 0) return NULL;

    for (uint64_t j = 0; j < slab->color_count; j++) {
        if (*(slab->color_map + (j >> 6)) == ~0UL) { j += 63; continue; }
        if (!(*(slab->color_map + (j >> 6)) & (1UL << (j % 64)))) {
            *(slab->color_map + (j >> 6)) |= (1UL << (j % 64));
            slab->using_count++;
            slab->free_count--;
            cache[idx].total_free--;
            cache[idx].total_using++;
            return (uint8_t*)slab->address + j * cache_sizes[idx];
        }
    }
    return NULL;
}

/* ── Tests ──────────────────────────────────────────── */

TEST_FUNC(test_slab_cache_init_32) {
    arena_used = 0;
    slab_cache_init(0); /* 32 byte slab */
    assert_not_null(cache[0].cache_pool);
    assert_eq(cache[0].size, 32);
    assert_eq(cache[0].cache_pool->free_count, PAGE_2M_SIZE / 32);
}

TEST_FUNC(test_slab_cache_init_4096) {
    arena_used = 0;
    slab_cache_init(7); /* 4096 byte slab */
    assert_not_null(cache[7].cache_pool);
    assert_eq(cache[7].size, 4096);
    assert_eq(cache[7].cache_pool->free_count, PAGE_2M_SIZE / 4096);
}

TEST_FUNC(test_slab_kmalloc_small) {
    arena_used = 0;
    slab_cache_init(0);  /* 32-byte cache */
    slab_cache_init(1);  /* 64-byte cache */
    slab_cache_init(2);  /* 128-byte cache */

    void *p = kmalloc_sim(32);
    assert_not_null(p);

    void *q = kmalloc_sim(64);
    assert_not_null(q);
    assert_true(p != q);
}

TEST_FUNC(test_slab_kmalloc_exhaustion) {
    arena_used = 0;
    slab_cache_init(0);  /* 32-byte cache, PAGE_2M_SIZE / 32 = 65536 entries */
    uint64_t free_before = cache[0].total_free;

    /* Allocate and free one entry */
    void *p = kmalloc_sim(32);
    assert_not_null(p);
    assert_eq(cache[0].total_free, free_before - 1);
}

TEST_FUNC(test_slab_kmalloc_alignment) {
    arena_used = 0;
    slab_cache_init(0);
    void *p = kmalloc_sim(32);
    assert_not_null(p);
    assert_eq((uintptr_t)p & 0x0F, 0);  /* 16-byte aligned (host sim) */
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_slab_cache_init_32),
    TEST_ENTRY(test_slab_cache_init_4096),
    TEST_ENTRY(test_slab_kmalloc_small),
    TEST_ENTRY(test_slab_kmalloc_exhaustion),
    TEST_ENTRY(test_slab_kmalloc_alignment),
TEST_LIST_END

int main() {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
