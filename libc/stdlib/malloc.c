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
#include <unistd.h>
#include <signal.h>
#endif

// ── Freelist allocator ───────────────────────────────────────
// Each block (both free and allocated) is prefixed with a header.
// Free blocks contain an additional next pointer after the header.
// Allocated blocks carry a magic tag, requested size, header canary,
// and a tail redzone after the requested payload for overflow/double-free
// detection.

#define MALLOC_ALIGN        16
#define MALLOC_REDZONE_SZ   16
#define MALLOC_MAGIC_ALLOC  0xA110CA7EU
#define MALLOC_MAGIC_FREE   0xF4EEB10CU
#define MALLOC_REDZONE_BYTE 0xFD
#define MALLOC_POISON_BYTE  0xDE
#define MALLOC_CANARY_SALT  0x9E3779B97F4A7C15ULL

#define HEADER_SIZE  ((sizeof(block_header_t) + MALLOC_ALIGN - 1) & ~(MALLOC_ALIGN - 1))
#define MIN_BLOCK_SIZE (HEADER_SIZE + 16)

typedef struct block_header {
    size_t   size;       // total block size, header included (free and allocated alike)
    uint32_t is_free;    // 1 if free, 0 if allocated
    uint32_t magic;      // MALLOC_MAGIC_ALLOC or MALLOC_MAGIC_FREE
    size_t   req_size;   // exact requested payload size (0 when free)
    uint64_t canary;     // integrity canary immediately preceding user pointer
} block_header_t;

typedef struct free_block {
    block_header_t header;
    struct free_block *next;  // next free block in freelist
    // ... free space ...
} free_block_t;

static free_block_t *freelist = NULL;
static int heap_initialized = 0;
static uintptr_t heap_base = 0;

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
    heap_base = (uintptr_t)get_brk();
}

// ── Heap hardening helpers ────────────────────────────────────

__attribute__((noreturn, cold))
static void malloc_abort(const char *msg, size_t len)
{
    (void)!write(2, msg, len);
    raise(SIGABRT);
    _exit(127);
}

#define MALLOC_ABORT(lit) malloc_abort((lit), sizeof(lit) - 1)

static uint64_t alloc_rotl64(uint64_t x, unsigned shift)
{
    return (x << shift) | (x >> (64 - shift));
}

static uint64_t alloc_canary(const block_header_t *hdr)
{
    uint64_t s = (uint64_t)hdr->size;
    uint64_t r = (uint64_t)hdr->req_size;
    return ((uint64_t)(uintptr_t)hdr) ^
           alloc_rotl64(s, 13) ^
           alloc_rotl64(r, 29) ^
           MALLOC_CANARY_SALT;
}

static void alloc_check_hdr_bounds(const block_header_t *hdr, uint64_t brk)
{
    uintptr_t h = (uintptr_t)hdr;

    if ((h & (MALLOC_ALIGN - 1)) != 0)
        MALLOC_ABORT("*** malloc: unaligned block header ***\n");

    if (heap_base != 0 && h < heap_base)
        MALLOC_ABORT("*** malloc: block header below heap ***\n");

    if (brk != 0) {
        if (h >= (uintptr_t)brk || (uintptr_t)brk - h < HEADER_SIZE)
            MALLOC_ABORT("*** malloc: block header past heap break ***\n");
    }

    if (hdr->size < MIN_BLOCK_SIZE || (hdr->size & (MALLOC_ALIGN - 1)) != 0)
        MALLOC_ABORT("*** malloc: corrupted block size ***\n");

    if (brk != 0 && hdr->size > (size_t)((uintptr_t)brk - h))
        MALLOC_ABORT("*** malloc: block extends past heap break ***\n");
}

static void alloc_validate_free_block(const free_block_t *fb, uint64_t brk)
{
    const block_header_t *hdr = &fb->header;

    alloc_check_hdr_bounds(hdr, brk);

    if (hdr->magic != MALLOC_MAGIC_FREE || hdr->is_free != 1 || hdr->req_size != 0)
        MALLOC_ABORT("*** malloc: corrupted free block header ***\n");

    if (hdr->canary != alloc_canary(hdr))
        MALLOC_ABORT("*** malloc: free block canary corrupted ***\n");
}

static void alloc_init_free_block(block_header_t *hdr, size_t size)
{
    hdr->size = size;
    hdr->is_free = 1;
    hdr->magic = MALLOC_MAGIC_FREE;
    hdr->req_size = 0;
    hdr->canary = alloc_canary(hdr);
}

static void *alloc_finish_block(block_header_t *hdr, size_t total_size, size_t req_size)
{
    uint8_t *user = (uint8_t *)hdr + HEADER_SIZE;
    size_t tail_len = (total_size - HEADER_SIZE) - req_size;

    hdr->size = total_size;
    hdr->is_free = 0;
    hdr->magic = MALLOC_MAGIC_ALLOC;
    hdr->req_size = req_size;
    hdr->canary = alloc_canary(hdr);

    if (tail_len > 0)
        memset(user + req_size, MALLOC_REDZONE_BYTE, tail_len);

    return (void *)user;
}

static block_header_t *alloc_validate_live_block(void *ptr)
{
    if (!heap_initialized)
        MALLOC_ABORT("*** malloc: invalid free (heap uninitialized) ***\n");

    if (((uintptr_t)ptr & (MALLOC_ALIGN - 1)) != 0)
        MALLOC_ABORT("*** malloc: invalid pointer (unaligned) ***\n");

    uint64_t brk = get_brk();
    if (heap_base != 0 && (uintptr_t)ptr < heap_base + HEADER_SIZE)
        MALLOC_ABORT("*** malloc: invalid pointer (below heap) ***\n");
    if (brk != 0 && (uintptr_t)ptr >= (uintptr_t)brk)
        MALLOC_ABORT("*** malloc: invalid pointer (above heap) ***\n");

    block_header_t *hdr = (block_header_t *)((uint8_t *)ptr - HEADER_SIZE);

    if (hdr->magic == MALLOC_MAGIC_FREE || hdr->is_free == 1)
        MALLOC_ABORT("*** malloc: double free detected ***\n");

    if (hdr->magic != MALLOC_MAGIC_ALLOC || hdr->is_free != 0)
        MALLOC_ABORT("*** malloc: corrupted block header or invalid pointer ***\n");

    alloc_check_hdr_bounds(hdr, brk);

    if (hdr->req_size > hdr->size - HEADER_SIZE - MALLOC_REDZONE_SZ)
        MALLOC_ABORT("*** malloc: corrupted requested size ***\n");

    if (hdr->canary != alloc_canary(hdr))
        MALLOC_ABORT("*** malloc: header canary corrupted (underflow) ***\n");

    const uint8_t *tail = (const uint8_t *)ptr + hdr->req_size;
    size_t tail_len = (hdr->size - HEADER_SIZE) - hdr->req_size;
    for (size_t i = 0; i < tail_len; i++) {
        if (tail[i] != MALLOC_REDZONE_BYTE)
            MALLOC_ABORT("*** malloc: heap buffer overflow detected ***\n");
    }

    if (brk != 0) {
        size_t remain = (size_t)((uintptr_t)brk - (uintptr_t)hdr);
        if (remain > hdr->size) {
            uint8_t *next_addr = (uint8_t *)hdr + hdr->size;
            if ((size_t)((uintptr_t)brk - (uintptr_t)next_addr) < HEADER_SIZE)
                MALLOC_ABORT("*** malloc: corrupted next block header ***\n");
            block_header_t *next_hdr = (block_header_t *)next_addr;
            if (next_hdr->magic != MALLOC_MAGIC_ALLOC &&
                next_hdr->magic != MALLOC_MAGIC_FREE) {
                MALLOC_ABORT("*** malloc: next block header corrupted ***\n");
            }
        }
    }

    return hdr;
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
// plus the tail redzone, rounded up to the allocator's alignment, plus
// the block header.
static int alloc_block_size(size_t size, size_t *out)
{
    size_t with_redzone;
    size_t total;

    if (size > ALLOC_SIZE_MAX - MALLOC_REDZONE_SZ)
        return -1;
    with_redzone = size + MALLOC_REDZONE_SZ;
    if (alloc_align_up(with_redzone, MALLOC_ALIGN, &total) != 0)
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
    uint64_t brk = get_brk();

    if (brk == 0 || (uintptr_t)b >= (uintptr_t)brk)
        return;

    // Check remaining space up to brk using subtraction to avoid overflow.
    if ((size_t)((uintptr_t)brk - (uintptr_t)b) <= block_size)
        return;

    uint8_t *next_addr = b + block_size;
    if ((size_t)((uintptr_t)brk - (uintptr_t)next_addr) < HEADER_SIZE)
        MALLOC_ABORT("*** malloc: corrupted next block header on coalesce ***\n");

    block_header_t *next_hdr = (block_header_t *)next_addr;
    if (next_hdr->magic == MALLOC_MAGIC_FREE || next_hdr->is_free == 1) {
        free_block_t *next_free = (free_block_t *)next_hdr;
        alloc_validate_free_block(next_free, brk);

        // Unlink next from freelist BEFORE committing the coalesced size.
        if (freelist == next_free) {
            freelist = next_free->next;
        } else {
            free_block_t *prev = freelist;
            while (prev && prev->next != next_free)
                prev = prev->next;
            if (!prev)
                MALLOC_ABORT("*** malloc: unlinked free block on coalesce ***\n");
            prev->next = next_free->next;
        }

        block->header.size += next_hdr->size;
        block->header.canary = alloc_canary(&block->header);
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
    uint64_t brk_now = block ? get_brk() : 0;
    while (block) {
        alloc_validate_free_block(block, brk_now);
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
                alloc_init_free_block(back_hdr, back);
                back_free = (free_block_t *)back_hdr;
                back_free->next = old_next;
            }

            if (front != 0) {
                // The gap in front keeps the block's place in the free list.
                alloc_init_free_block(&block->header, front);
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
            return alloc_finish_block(head_hdr, need, size);
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
            alloc_init_free_block(front_hdr, front);
            front_free->next = freelist;
            freelist = front_free;
        }
        return alloc_finish_block((block_header_t *)head, need, size);
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
    uint64_t brk_now = block ? get_brk() : 0;

    while (block) {
        alloc_validate_free_block(block, brk_now);
        if (block->header.size >= total) {
            // Found a block
            size_t remain = block->header.size - total;

            if (remain >= MIN_BLOCK_SIZE) {
                // Split the block: create a new free block from the remainder
                uint8_t *b = (uint8_t *)block;
                block_header_t *new_hdr = (block_header_t *)(b + total);
                alloc_init_free_block(new_hdr, remain);

                free_block_t *new_free = (free_block_t *)new_hdr;
                new_free->next = block->next;

                // Replace block in freelist
                if (prev)
                    prev->next = new_free;
                else
                    freelist = new_free;
                return alloc_finish_block(&block->header, total, size);
            } else {
                // Use the entire block
                size_t full_size = block->header.size;
                if (prev)
                    prev->next = block->next;
                else
                    freelist = block->next;
                return alloc_finish_block(&block->header, full_size, size);
            }
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

    return alloc_finish_block((block_header_t *)brk, total, size);
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

    block_header_t *hdr = alloc_validate_live_block(ptr);

    alloc_init_free_block(hdr, hdr->size);
    if (hdr->size > sizeof(free_block_t)) {
        memset((uint8_t *)hdr + sizeof(free_block_t), MALLOC_POISON_BYTE,
               hdr->size - sizeof(free_block_t));
    }

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
#if defined(__is_libk)
    void *new = malloc(size);
    if (!new) return NULL;
    /* Kernel slab allocator: no user-visible size header.  ksize() returns
     * the slab object size (an upper bound on the original request). */
    extern size_t ksize(void *);
    size_t old_usable = ksize(ptr);
#else
    block_header_t *hdr = alloc_validate_live_block(ptr);
    size_t old_usable = hdr->req_size;
    void *new = malloc(size);
    if (!new) return NULL;
#endif
    size_t copy = old_usable < size ? old_usable : size;
    memcpy(new, ptr, copy);
    free(ptr);
    return new;
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
