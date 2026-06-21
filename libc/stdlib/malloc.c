#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/syscall.h>
#include <stdint.h>

#if defined(__is_libk)
#include <kernel/slab.h>
#endif

// ── Freelist allocator ───────────────────────────────────────
// Each block (both free and allocated) is prefixed with a header.
// Free blocks contain an additional next pointer after the header.

#define MALLOC_ALIGN 16
#define HEADER_SIZE  ((sizeof(block_header_t) + MALLOC_ALIGN - 1) & ~(MALLOC_ALIGN - 1))
#define MIN_BLOCK_SIZE (HEADER_SIZE + 16)

typedef struct block_header {
    size_t size;       // total block size including header (allocated) or usable size (free)
    int    is_free;    // 1 if free, 0 if allocated
} block_header_t;

typedef struct free_block {
    block_header_t header;
    struct free_block *next;  // next free block in freelist
    // ... free space ...
} free_block_t;

static free_block_t *freelist = NULL;
static int heap_initialized = 0;

// Query or set the program break
static uint64_t get_brk(void)
{
    int64_t ret = syscall(SYS_brk, 0, 0, 0);
    if (ret < 0) return 0;
    return (uint64_t)ret;
}

static int set_brk(uint64_t addr)
{
    int64_t actual = syscall(SYS_brk, addr, 0, 0);
    if (actual < 0) return -1;
    return ((uint64_t)actual >= addr) ? 0 : -1;
}

// Initialize the heap
static void heap_init(void)
{
    heap_initialized = 1;
    freelist = NULL;
}

// Round up to alignment
static inline size_t align_up(size_t n, size_t align)
{
    return (n + align - 1) & ~(align - 1);
}

// Try to coalesce adjacent free blocks after a free.
// This is a simplified version — only coalesces with the next block
// if it's also free.  Full coalescing with previous block would require
// a doubly-linked list or boundary tags.
static void try_coalesce(free_block_t *block)
{
    uint8_t *b = (uint8_t *)block;
    size_t block_size = block->header.size;

    // Find next block in memory
    uint8_t *next_addr = b + block_size;
    uint64_t brk = get_brk();

    if (next_addr >= (uint8_t *)brk)
        return;

    block_header_t *next_hdr = (block_header_t *)next_addr;
    if (next_hdr->is_free) {
        // Coalesce
        block->header.size += next_hdr->size;

        // Remove next from freelist
        free_block_t *next_free = (free_block_t *)next_hdr;
        if (freelist == next_free) {
            freelist = next_free->next;
        } else {
            free_block_t *prev = freelist;
            while (prev && prev->next != next_free)
                prev = prev->next;
            if (prev)
                prev->next = next_free->next;
        }
    }
}

void *malloc(size_t size)
{
#if defined(__is_libk)
    return kmalloc(size);
#else
    if (size == 0)
        return NULL;

    if (!heap_initialized)
        heap_init();

    // Align size and add header
    size_t total = align_up(size, MALLOC_ALIGN) + HEADER_SIZE;
    if (total < MIN_BLOCK_SIZE)
        total = MIN_BLOCK_SIZE;

    // Search freelist for a suitable block (first-fit)
    free_block_t *prev = NULL;
    free_block_t *block = freelist;

    while (block) {
        if (block->header.size >= total) {
            // Found a block
            size_t remain = block->header.size - total;

            if (remain >= MIN_BLOCK_SIZE) {
                // Split the block: create a new free block from the remainder
                uint8_t *b = (uint8_t *)block;
                block_header_t *new_hdr = (block_header_t *)(b + total);
                new_hdr->size = remain;
                new_hdr->is_free = 1;

                free_block_t *new_free = (free_block_t *)new_hdr;
                new_free->next = block->next;

                block->header.size = total;

                // Replace block in freelist
                if (prev)
                    prev->next = new_free;
                else
                    freelist = new_free;
            } else {
                // Use the entire block
                if (prev)
                    prev->next = block->next;
                else
                    freelist = block->next;
            }

            block->header.is_free = 0;
            return (void *)((uint8_t *)block + HEADER_SIZE);
        }
        prev = block;
        block = block->next;
    }

    // No suitable free block — extend the heap
    uint64_t brk = get_brk();
    if (brk == 0)
        return NULL;

    // Check if we can extend the last block (coalesce with end-of-heap)
    // For simplicity, just extend the heap
    if (set_brk(brk + total) != 0) {
        errno = ENOMEM;
        return NULL;
    }

    block_header_t *hdr = (block_header_t *)brk;
    hdr->size = total;
    hdr->is_free = 0;

    return (void *)((uint8_t *)hdr + HEADER_SIZE);
#endif
}

void free(void *ptr)
{
#if defined(__is_libk)
    kfree(ptr);
    return;
#else
    if (!ptr)
        return;
    if (!heap_initialized)
        return;

    uint8_t *b = (uint8_t *)ptr - HEADER_SIZE;
    block_header_t *hdr = (block_header_t *)b;

    if (hdr->is_free) {
        // Double free — ignore
        return;
    }

    hdr->is_free = 1;
    free_block_t *fb = (free_block_t *)hdr;

    // Insert at head of freelist
    fb->next = freelist;
    freelist = fb;

    // Try to coalesce with the next block
    try_coalesce(fb);
#endif
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    void *new = malloc(size);
    if (!new) return NULL;
    /* Copy old data (up to min(old_size, new_size)) */
    size_t old_size = ((size_t*)ptr)[-1];
    size_t copy = old_size < size ? old_size : size;
    for (size_t i = 0; i < copy; i++)
        ((unsigned char*)new)[i] = ((unsigned char*)ptr)[i];
    free(ptr);
    return new;
}

char *realpath(const char *path, char *resolved) {
    (void)path; (void)resolved;
    return NULL;
}
