#include <stdlib.h>
#include <malloc.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>

#if defined(__is_libk)
#include <memory/slab.h>
#else
// libc mode: heap management via SYS_brk. The header is x86_64 asm-only,
// so we only pull it in outside the kernel side (kernel side uses slab).
#include <sys/syscall.h>
#endif

// ── Freelist allocator ───────────────────────────────────────
// Each block (both free and allocated) is prefixed with a header.
// Free blocks contain an additional next pointer after the header.

#define MALLOC_ALIGN 16
#define HEADER_SIZE  ((sizeof(block_header_t) + MALLOC_ALIGN - 1) & ~(MALLOC_ALIGN - 1))
#define MIN_BLOCK_SIZE (HEADER_SIZE + 16)

typedef struct block_header {
    size_t size;       // total block size, header included (free and allocated alike)
    int    is_free;    // 1 if free, 0 if allocated
} block_header_t;

typedef struct free_block {
    block_header_t header;
    struct free_block *next;  // next free block in freelist
    // ... free space ...
} free_block_t;

static free_block_t *freelist = NULL;
static int heap_initialized = 0;

#ifndef __is_libk
// Query or set the program break
static uint64_t get_brk(void)
{
    int64_t ret = syscall(SYS_brk, 0, 0, 0);
    if (ret < 0) return 0;
    return (uint64_t)ret;
}



static int __attribute__((unused)) set_brk(uint64_t addr)
{
    int64_t actual = syscall(SYS_brk, addr, 0, 0);
    if (actual < 0) return -1;
    return ((uint64_t)actual >= addr) ? 0 : -1;
}

// Initialize the heap
static void __attribute__((unused)) heap_init(void)
{
    heap_initialized = 1;
    freelist = NULL;
}

// ── Checked size arithmetic (T2) ───────────────────────────────
// The rounding and the header addition are checked.  A request whose
// rounded size, or whose rounded size plus the block header, wraps
// around must be refused: a wrapped size would be handed out as a block
// that is far smaller than the caller asked for.

#define ALLOC_SIZE_MAX ((size_t)-1)

// Round `n` up to a multiple of `align` (a power of two).
static int alloc_align_up(size_t n, size_t align, size_t *out)
{
    if (n > ALLOC_SIZE_MAX - (align - 1))
        return -1;
    *out = (n + align - 1) & ~(align - 1);
    return 0;
}

// The total size of a block that serves `size` user bytes: the request
// rounded up to the allocator's alignment, plus the block header.
static int alloc_block_size(size_t size, size_t *out)
{
    size_t total;

    if (alloc_align_up(size, MALLOC_ALIGN, &total) != 0)
        return -1;
    if (total > ALLOC_SIZE_MAX - HEADER_SIZE)
        return -1;
    total += HEADER_SIZE;
    if (total < MIN_BLOCK_SIZE)
        total = MIN_BLOCK_SIZE;
    *out = total;
    return 0;
}

// Try to coalesce adjacent free blocks after a free.
// This is a simplified version — only coalesces with the next block
// if it's also free.  Full coalescing with previous block would require
// a doubly-linked list or boundary tags.
static void __attribute__((unused)) try_coalesce(free_block_t *block)
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

// ── Aligned allocation (T2) ─────────────────────────────────────
// An aligned block is a REAL block of the existing block protocol: its
// header sits immediately before the user pointer, so free() and realloc()
// work on it unchanged and no "preceding pointer" is introduced on the
// other free paths.
//
// A free block is split into
//
//   [ front free block ][ aligned block ][ back free block ]
//
// with the aligned block's header placed so that the user pointer is
// aligned.  The gap in front is either empty or a whole free block; a gap
// too small to be a free block of its own is not usable, so the next
// alignment slot is tried instead.

// True when `alignment` is a legal alignment: a power of two and a
// multiple of sizeof(void*) (POSIX).
static int alloc_alignment_valid(size_t alignment)
{
    if (alignment < sizeof(void *))
        return 0;
    if (alignment % sizeof(void *) != 0)
        return 0;
    if ((alignment & (alignment - 1)) != 0)
        return 0;
    return 1;
}

// Round a pointer up to a multiple of `align` (a power of two).
static uintptr_t alloc_align_up_ptr(uintptr_t value, size_t align)
{
    return (value + (uintptr_t)(align - 1)) & ~(uintptr_t)(align - 1);
}

// Allocate a block whose user pointer is aligned to `alignment`.
// `alignment` must pass alloc_alignment_valid().
static void *alloc_aligned(size_t alignment, size_t size)
{
    size_t need;
    size_t align;
    free_block_t *prev = NULL;
    free_block_t *block;

    if (size == 0)
        return NULL;
    if (alloc_block_size(size, &need) != 0)
        return NULL;

    if (!heap_initialized)
        heap_init();

    // A request weaker than the allocator's own alignment is satisfied by
    // an ordinary block: every block the allocator hands out is already
    // MALLOC_ALIGN aligned, and MALLOC_ALIGN is a multiple of every
    // alignment below it that this allocator accepts.
    align = alignment < MALLOC_ALIGN ? MALLOC_ALIGN : alignment;

    // 1. First fit in the free list.
    block = freelist;
    while (block) {
        uintptr_t base = (uintptr_t)block;
        uintptr_t end = base + block->header.size;
        uintptr_t user = alloc_align_up_ptr(base + HEADER_SIZE, align);
        uintptr_t head = user - HEADER_SIZE;
        size_t front;
        size_t back;
        int fits = 0;

        if (head >= base) {
            // Try the alignment slots until the gap in front is usable and
            // the block still fits.  A gap in front that is too small to be
            // a free block of its own is not usable, so the next slot (whose
            // gap is larger) is tried instead.
            for (;;) {
                if (head > end || need > end - head)
                    break;                       // the block does not fit
                if ((head - base) == 0 || (head - base) >= MIN_BLOCK_SIZE) {
                    fits = 1;
                    break;
                }
                if (head + align < head)
                    break;                       // address arithmetic wrapped
                user += align;
                head += align;
            }
        }
        if (!fits) {
            prev = block;
            block = block->next;
            continue;
        }

        front = (size_t)(head - base);
        back = (size_t)(end - (head + need));

        // A remainder smaller than MIN_BLOCK_SIZE cannot be a free block of
        // its own.  Dropping it would leave a HOLE in the heap: the block
        // that follows the free block still starts at `end`, but the
        // aligned block would claim to end at head+need, so the heap would
        // no longer tile.  try_coalesce walks the heap by address, so it
        // would later step into the hole and coalesce with whatever bytes
        // happen to be there - which is how an allocation's payload ended
        // up being read as a block header.  The remainder is therefore
        // absorbed by the aligned block, which then covers exactly the
        // space the free block used to cover.
        if (back != 0 && back < MIN_BLOCK_SIZE) {
            need += back;
            back = 0;
        }

        {
            free_block_t *old_next = block->next;
            free_block_t *back_free = NULL;
            block_header_t *head_hdr = (block_header_t *)head;

            if (back >= MIN_BLOCK_SIZE) {
                block_header_t *back_hdr = (block_header_t *)(head + need);
                back_hdr->size = back;
                back_hdr->is_free = 1;
                back_free = (free_block_t *)back_hdr;
                back_free->next = old_next;
            }

            head_hdr->size = need;
            head_hdr->is_free = 0;

            if (front != 0) {
                // The gap in front keeps the block's place in the free list.
                block->header.size = front;
                block->next = back_free ? back_free : old_next;
            } else if (back_free) {
                // The block itself becomes the aligned block; the gap in
                // back takes its place in the free list.
                if (prev)
                    prev->next = back_free;
                else
                    freelist = back_free;
            } else {
                // The whole block becomes the aligned block.
                if (prev)
                    prev->next = old_next;
                else
                    freelist = old_next;
            }
            return (void *)user;
        }
    }

    // 2. No free block fits: extend the heap.
    {
        uint64_t brk = get_brk();
        uintptr_t base;
        uintptr_t end;
        uintptr_t user;
        uintptr_t head;
        size_t front;

        if (brk == 0)
            return NULL;

        base = (uintptr_t)brk;
        user = alloc_align_up_ptr(base + HEADER_SIZE, align);
        head = user - HEADER_SIZE;
        // The gap in front must be empty or a whole free block.
        while ((head - base) != 0 && (head - base) < MIN_BLOCK_SIZE) {
            if (head + align < head)
                return NULL;                  // address arithmetic wrapped
            user += align;
            head += align;
        }
        front = (size_t)(head - base);

        if (head + need < head)
            return NULL;
        end = head + need;
        if ((uint64_t)end < (uint64_t)head)
            return NULL;
        if (set_brk((uint64_t)end) != 0) {
            errno = ENOMEM;
            return NULL;
        }

        if (front != 0) {
            // The gap in front of the aligned block is a free block.
            block_header_t *front_hdr = (block_header_t *)base;
            free_block_t *front_free = (free_block_t *)base;
            front_hdr->size = front;
            front_hdr->is_free = 1;
            front_free->next = freelist;
            freelist = front_free;
        }
        {
            block_header_t *head_hdr = (block_header_t *)head;
            head_hdr->size = need;
            head_hdr->is_free = 0;
        }
        return (void *)user;
    }
}
#endif /* __is_libk */

void *malloc(size_t size)
{
#if defined(__is_libk)
    return kmalloc(size);
#else
    size_t total;

    if (size == 0)
        return NULL;

    if (!heap_initialized)
        heap_init();

    // Align size and add header.  Both steps are checked: a request whose
    // rounded size, or whose rounded size plus the header, wraps around
    // must fail instead of handing out a block that is smaller than the
    // caller asked for.
    if (alloc_block_size(size, &total) != 0) {
        errno = ENOMEM;
        return NULL;
    }

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
    if (total > UINT64_MAX - brk) {
        errno = ENOMEM;
        return NULL;
    }
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
#if defined(__is_libk)
    /* Kernel slab allocator: no user-visible size header.  ksize() returns
     * the slab object size (an upper bound on the original request). */
    extern size_t ksize(void *);
    size_t old_usable = ksize(ptr);
#else
    /* Copy old data (up to min(old_usable, new_size)).
     * The size header sits HEADER_SIZE bytes before the user pointer.
     * The previous code read ((size_t*)ptr)[-1] — that is the is_free
     * flag + padding (0 for an allocated block) — so copy was always 0
     * and every realloc silently dropped the block contents (pwd, tail,
     * cut/paste/tac/expand/nl all read empty/zeroed buffers). */
    block_header_t *hdr = (block_header_t *)((uint8_t *)ptr - HEADER_SIZE);
    size_t old_usable = hdr->size - HEADER_SIZE;
#endif
    size_t copy = old_usable < size ? old_usable : size;
    memcpy(new, ptr, copy);
    free(ptr);
    return new;
}

char *realpath(const char *path, char *resolved) {
    (void)path; (void)resolved;
    return NULL;
}

#ifndef __is_libk
// ── posix_memalign / aligned_alloc (T2) ─────────────────────────
// User libc only.  The kernel side (libk) keeps the plain slab allocator
// and does not define these symbols; the public header hides the
// declarations in __is_libk mode.
//
// A zero size keeps the allocator's existing policy: no block is handed
// out, so posix_memalign reports ENOMEM and aligned_alloc returns NULL.

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    void *ptr;

    if (memptr == NULL)
        return EINVAL;
    if (!alloc_alignment_valid(alignment))
        return EINVAL;

    // POSIX: posix_memalign reports a failure through its return value and
    // never through errno, so the caller's errno is restored.
    {
        int saved = errno;
        ptr = alloc_aligned(alignment, size);
        errno = saved;
    }
    if (ptr == NULL)
        return ENOMEM;

    *memptr = ptr;
    return 0;
}

void *aligned_alloc(size_t alignment, size_t size)
{
    void *ptr;

    if (!alloc_alignment_valid(alignment)) {
        errno = EINVAL;
        return NULL;
    }
    // C11: the size must be an integral multiple of the alignment.
    if (size % alignment != 0) {
        errno = EINVAL;
        return NULL;
    }

    ptr = alloc_aligned(alignment, size);
    if (ptr == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    return ptr;
}
#endif /* __is_libk */
