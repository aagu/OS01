/*
 * hosttests/cases/test_libc_alloc.c — OS01 libc completion, alloc group (T2).
 *
 * The alloc group checks the OS01 allocator's CONTRACTS:
 *
 *   calloc_product_wrap              calloc refuses a request whose element
 *                                    count times element size wraps around
 *   malloc_roundup_wrap              malloc refuses a request whose rounded
 *                                    size plus the block header wraps around,
 *                                    and reports a heap that cannot grow
 *   reallocarray_failure_preserves_bytes
 *                                    a failing reallocarray leaves the old
 *                                    block byte-for-byte intact
 *   aligned_block_lifecycle          an aligned block is a real block: it is
 *                                    aligned, it survives realloc, it does not
 *                                    disturb its neighbours, and it is freed
 *                                    in any order
 *   posix_memalign_bad_alignment     a bad alignment is EINVAL, and neither
 *                                    the output pointer nor errno changes
 *   aligned_alloc_bad_multiple       a size that is not a multiple of the
 *                                    alignment is EINVAL
 *
 * The binary links the REAL production allocator objects (libc/stdlib/
 * malloc.c, calloc.c and reallocarray.c) under their `os01_` prefix (see
 * hosttests/mk/common.mk), so a call in this file can never resolve to the
 * host libc's same-named implementation.  The allocator's heap is the mock
 * in hosttests/mock/libc_heap_runtime.h: the production allocator grows its
 * heap with SYS_brk, and under OS01_HOST_TEST every syscall is routed
 * through the `host_test_syscall` hook this file implements.
 *
 * The new entry points are declared WEAK below, so the binary links even
 * before the implementation exists; a missing entry point is then reported
 * as a test failure instead of a link error, and the behavioural checks of
 * the other cases still run.
 */
#include "test_framework.h"
#include "libc_heap_runtime.h"

#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#define fork os01_sys_fork
#include <sys/syscall.h>
#undef fork
#include <sys/wait.h>
#include <unistd.h>

/* The allocator entry points.  The production objects are compiled with the
 * same `os01_` prefix, so these declarations bind to the production
 * implementations and never to the host libc's. */
extern void *malloc(size_t size);
extern void *calloc(size_t n, size_t size);
extern void free(void *ptr);
extern void *realloc(void *ptr, size_t size);

/* Weak: a missing implementation must be a test failure, not a link error. */
extern void *reallocarray(void *ptr, size_t nmemb, size_t size)
    __attribute__((weak));
extern int posix_memalign(void **memptr, size_t alignment, size_t size)
    __attribute__((weak));
extern void *aligned_alloc(size_t alignment, size_t size)
    __attribute__((weak));

/* errno is a plain global in the OS01 libc; the host glibc does not export
 * one, so the test provides it (the production objects reference it). */
int errno = 0;

/* ── the mock heap ───────────────────────────────────────────────
 * The production allocator grows its heap with SYS_brk.  The mock answers
 * that one syscall and nothing else: every other syscall is -ENOSYS.
 * The heap is a static, 16-byte aligned array, so the allocator's block
 * headers land on the alignment it assumes. */

static unsigned char heap_storage[LIBC_HEAP_RUNTIME_SIZE]
    __attribute__((aligned(16)));
static size_t heap_break;      /* the program break, as an offset */
static int heap_fail_grow;     /* fault injection: fail the next growth */

void libc_heap_runtime_reset(void)
{
    heap_break = 0;
    heap_fail_grow = 0;
}

void libc_heap_runtime_fail_next_grow(void)
{
    heap_fail_grow = 1;
}

uint64_t libc_heap_runtime_brk(void)
{
    return (uint64_t)(heap_storage + heap_break);
}

uint64_t libc_heap_runtime_base(void)
{
    return (uint64_t)heap_storage;
}

int64_t host_test_syscall(uint64_t nr, uint64_t arg1, uint64_t arg2, uint64_t arg3)
{
    uintptr_t lo = (uintptr_t)heap_storage;
    uintptr_t hi = lo + LIBC_HEAP_RUNTIME_SIZE;
    uintptr_t cur = lo + heap_break;

    (void)arg2;
    (void)arg3;

    if (nr != SYS_brk) {
        return -38;                       /* -ENOSYS */
    }
    if (arg1 == 0) {
        return (int64_t)cur;              /* query the program break */
    }
    if ((uintptr_t)arg1 > cur && heap_fail_grow) {
        heap_fail_grow = 0;
        return -12;                       /* -ENOMEM */
    }
    if ((uintptr_t)arg1 > hi) {
        return (int64_t)cur;              /* the heap cannot grow that far */
    }
    heap_break = (size_t)((uintptr_t)arg1 - lo);
    return (int64_t)(uintptr_t)arg1;
}

/* ── helpers ──────────────────────────────────────────────────── */

#define GUARD_BYTES 32
#define GUARD_PATTERN 0x5A
#define LIVE_PATTERN  0xA5
#define POISON_PATTERN 0xDE

/* True when every byte of the buffer holds `value`. */
static int buffer_is(const unsigned char *buf, size_t len, unsigned char value)
{
    size_t i;
    for (i = 0; i < len; i++) {
        if (buf[i] != value) {
            return 0;
        }
    }
    return 1;
}

/* ── case: calloc refuses a wrapping multiplication ───────────── */

static void calloc_product_wrap(void)
{
    void *ptr;

    TEST_SUITE("calloc_product_wrap");

    /* The element count times the element size wraps around: the request
     * cannot be honoured and must be refused. */
    errno = 0;
    ptr = calloc(SIZE_MAX / 2 + 1, 2);
    assert_null(ptr);
    assert_eq(ENOMEM, errno);

    /* A request that does not wrap must still work, and the memory must
     * come back zeroed. */
    errno = 0;
    ptr = calloc(8, 8);
    assert_not_null(ptr);
    assert_eq(0, errno);
    if (ptr) {
        assert_true(buffer_is((unsigned char *)ptr, 64, 0));
        free(ptr);
    }

    /* The product fits in a size_t, but the allocation itself is
     * impossible: that is an out-of-memory failure, not a silent success
     * on a wrapped size. */
    errno = 0;
    ptr = calloc(SIZE_MAX / 2, 1);
    assert_null(ptr);
    assert_eq(ENOMEM, errno);
}

/* ── case: malloc refuses a wrapping size ─────────────────────── */

static void malloc_roundup_wrap(void)
{
    void *ptr;

    TEST_SUITE("malloc_roundup_wrap");

    /* The heap cannot grow: the allocator must report the failure
     * instead of handing out a block it does not own. */
    libc_heap_runtime_fail_next_grow();
    errno = 0;
    ptr = malloc(256 * 1024);
    assert_null(ptr);
    assert_eq(ENOMEM, errno);

    /* The same request succeeds once the heap may grow again: the
     * injection above really was the reason for the failure. */
    errno = 0;
    ptr = malloc(256 * 1024);
    assert_not_null(ptr);
    assert_eq(0, errno);
    if (ptr) {
        memset(ptr, 0x33, 256 * 1024);
        assert_true(buffer_is((unsigned char *)ptr, 256 * 1024, 0x33));
        free(ptr);
    }

    /* Rounding the request up wraps around. */
    errno = 0;
    ptr = malloc(SIZE_MAX);
    assert_null(ptr);
    assert_eq(ENOMEM, errno);

    /* The request itself fits, but the rounded size plus the block
     * header wraps around. */
    errno = 0;
    ptr = malloc(SIZE_MAX - 15);
    assert_null(ptr);
    assert_eq(ENOMEM, errno);

    /* An ordinary request still works. */
    errno = 0;
    ptr = malloc(64);
    assert_not_null(ptr);
    assert_eq(0, errno);
    if (ptr) {
        memset(ptr, 0x77, 64);
        assert_true(buffer_is((unsigned char *)ptr, 64, 0x77));
        free(ptr);
    }
}

/* ── case: a failing reallocarray keeps the old block ─────────── */

static void reallocarray_failure_preserves_bytes(void)
{
    unsigned char *block;
    unsigned char *grown;
    void *failed;

    TEST_SUITE("reallocarray_failure_preserves_bytes");

    if (!reallocarray) {
        printf("  [FAIL] reallocarray is not implemented\n");
        assert_true(0);
        return;
    }

    block = (unsigned char *)malloc(32);
    assert_not_null(block);
    if (!block) {
        return;
    }
    memset(block, 0xA5, 32);

    /* The multiplication wraps around: the call fails and the old block
     * is left exactly as it was. */
    errno = 0;
    failed = reallocarray(block, SIZE_MAX / 2 + 1, 2);
    assert_null(failed);
    assert_eq(ENOMEM, errno);
    assert_true(buffer_is(block, 32, 0xA5));

    /* The old block is still a valid block: it can be freed. */
    free(block);

    /* A request that does not wrap grows the block and keeps the old
     * content.  The region carries guard bytes around the payload, so a
     * write past the payload would be visible. */
    {
        size_t payload = 32;
        size_t total = GUARD_BYTES + payload + GUARD_BYTES;
        unsigned char *region = (unsigned char *)malloc(total);
        assert_not_null(region);
        if (!region) {
            return;
        }
        memset(region, GUARD_PATTERN, total);
        memset(region + GUARD_BYTES, 0xA5, payload);

        grown = (unsigned char *)reallocarray(region, 1, total + 64);
        assert_not_null(grown);
        if (grown) {
            assert_true(buffer_is(grown + GUARD_BYTES, payload, 0xA5));
            assert_true(buffer_is(grown, GUARD_BYTES, GUARD_PATTERN));
            free(grown);
        }
    }
}

/* ── case: the aligned block lifecycle ────────────────────────── */

static void aligned_block_lifecycle(void)
{
    static const size_t alignments[] = { 16, 64, 4096 };
    size_t i;

    TEST_SUITE("aligned_block_lifecycle");

    if (!posix_memalign || !aligned_alloc) {
        printf("  [FAIL] posix_memalign/aligned_alloc are not implemented\n");
        assert_true(0);
        return;
    }

    for (i = 0; i < sizeof(alignments) / sizeof(alignments[0]); i++) {
        size_t align = alignments[i];
        unsigned char *before;
        unsigned char *after;
        void *aligned = NULL;
        void *aligned2 = NULL;
        void *plain;
        int rc;

        /* A live neighbour in front of the aligned block: it must not be
         * disturbed by the split. */
        before = (unsigned char *)malloc(128);
        assert_not_null(before);
        if (!before) {
            return;
        }
        memset(before, LIVE_PATTERN, 128);

        rc = posix_memalign(&aligned, align, 100);
        assert_eq(0, rc);
        assert_not_null(aligned);
        if (!aligned) {
            free(before);
            return;
        }
        assert_true(((uintptr_t)aligned % align) == 0);
        assert_true(aligned != (void *)before);

        /* A live neighbour behind the aligned block. */
        after = (unsigned char *)malloc(128);
        assert_not_null(after);
        if (!after) {
            free(aligned);
            free(before);
            return;
        }
        memset(after, LIVE_PATTERN, 128);

        /* Neither neighbour was touched by the split. */
        assert_true(buffer_is(before, 128, LIVE_PATTERN));
        assert_true(buffer_is(after, 128, LIVE_PATTERN));

        /* The aligned block is a real block: it can be written, grown
         * and freed like any other. */
        memset(aligned, 0x3C, 100);
        {
            void *grown = realloc(aligned, 200);
            assert_not_null(grown);
            if (grown) {
                assert_true(buffer_is((unsigned char *)grown, 100, 0x3C));
                aligned = grown;
            }
        }

        /* A second aligned block of the same alignment, freed in the
         * other order, must work just as well. */
        rc = posix_memalign(&aligned2, align, 64);
        assert_eq(0, rc);
        assert_not_null(aligned2);
        if (aligned2) {
            assert_true(((uintptr_t)aligned2 % align) == 0);
            assert_true(aligned2 != aligned);
        }

        /* Free in a mixed order; the allocator must stay consistent. */
        free(aligned2);
        free(before);
        free(aligned);
        free(after);

        /* The heap is still usable after the mixed free order. */
        plain = malloc(64);
        assert_not_null(plain);
        if (plain) {
            memset(plain, 0x77, 64);
            assert_true(buffer_is((unsigned char *)plain, 64, 0x77));
            free(plain);
        }
    }

    /* A freed block is poisoned, so that a later hand-out of the same
     * memory to a still live block would be visible.  The poison starts
     * after the allocator's own free-list link, which occupies the first
     * bytes of a free block. */
    {
        unsigned char *live = (unsigned char *)malloc(128);
        unsigned char *doomed = (unsigned char *)malloc(128);
        void *fresh;

        assert_not_null(live);
        assert_not_null(doomed);
        if (!live || !doomed) {
            free(live);
            free(doomed);
            return;
        }
        memset(live, LIVE_PATTERN, 128);
        memset(doomed, LIVE_PATTERN, 128);

        free(doomed);
        memset(doomed + 128 - 16, POISON_PATTERN, 16);

        /* The live block is untouched by the poison. */
        assert_true(buffer_is(live, 128, LIVE_PATTERN));

        /* The allocator still works, and never hands the same memory to
         * two live blocks. */
        fresh = malloc(128);
        assert_not_null(fresh);
        if (fresh) {
            assert_true(fresh != (void *)live);
            memset(fresh, 0x44, 128);
            assert_true(buffer_is(live, 128, LIVE_PATTERN));
            free(fresh);
        }
        free(live);
    }
}

/* ── case: a bad alignment is refused ─────────────────────────── */

static void posix_memalign_bad_alignment(void)
{
    void *out = (void *)0x1234;
    int rc;

    TEST_SUITE("posix_memalign_bad_alignment");

    if (!posix_memalign) {
        printf("  [FAIL] posix_memalign is not implemented\n");
        assert_true(0);
        return;
    }

    /* Not a multiple of sizeof(void*). */
    errno = 0;
    rc = posix_memalign(&out, 3, 64);
    assert_eq(EINVAL, rc);
    assert_true(out == (void *)0x1234);
    assert_eq(0, errno);

    /* Half of sizeof(void*): still not a multiple of it. */
    out = (void *)0x1234;
    errno = 0;
    rc = posix_memalign(&out, sizeof(void *) / 2, 64);
    assert_eq(EINVAL, rc);
    assert_true(out == (void *)0x1234);
    assert_eq(0, errno);

    /* Zero is not a valid alignment either. */
    out = (void *)0x1234;
    errno = 0;
    rc = posix_memalign(&out, 0, 64);
    assert_eq(EINVAL, rc);
    assert_true(out == (void *)0x1234);
    assert_eq(0, errno);
}

/* ── case: aligned_alloc needs a multiple of the alignment ─────── */

static void aligned_alloc_bad_multiple(void)
{
    void *ptr;

    TEST_SUITE("aligned_alloc_bad_multiple");

    if (!aligned_alloc) {
        printf("  [FAIL] aligned_alloc is not implemented\n");
        assert_true(0);
        return;
    }

    /* The size is not a multiple of the alignment. */
    errno = 0;
    ptr = aligned_alloc(64, 100);
    assert_null(ptr);
    assert_eq(EINVAL, errno);

    /* A size that IS a multiple works and is aligned. */
    errno = 0;
    ptr = aligned_alloc(64, 128);
    assert_not_null(ptr);
    assert_eq(0, errno);
    if (ptr) {
        assert_true(((uintptr_t)ptr % 64) == 0);
        memset(ptr, 0x5B, 128);
        assert_true(buffer_is((unsigned char *)ptr, 128, 0x5B));
        free(ptr);
    }
}


/* ── the heap must stay tiled ───────────────────────────────────────────
 *
 * The allocator's blocks tile the heap: walking from the heap base by
 * each block's size must land exactly on the program break.  A hole in the
 * heap breaks that walk, and the allocator's coalescing step - which finds
 * the next block by ADDRESS - would then read whatever bytes happen to be
 * in the hole as a block header.  That is how an allocation's payload was
 * once read as a block header, and how the allocator ended up handing out
 * overlapping blocks.
 *
 * The regression: carving an aligned block out of a free block whose tail
 * remainder was too small to be a free block of its own used to DROP the
 * remainder, leaving a hole.  The remainder is now absorbed by the aligned
 * block, which keeps the heap tiled.
 *
 * The block header layout mirrored here is libc/stdlib/malloc.c's: a size_t
 * size (the TOTAL block size, header included) followed by an int is_free,
 * magic, req_size, and canary (32 bytes total).
 */
#define ALLOC_BLOCK_HEADER_SIZE 32
#define ALLOC_BLOCK_ALIGN        16

static int alloc_heap_tiles(void)
{
    uint64_t base = libc_heap_runtime_base();
    uint64_t brk = libc_heap_runtime_brk();
    uint64_t p = base;
    int blocks = 0;

    if (brk <= base)
        return 1;
    while (p < brk) {
        uint64_t size = *(uint64_t *)p;
        if (size < ALLOC_BLOCK_HEADER_SIZE || (size % ALLOC_BLOCK_ALIGN) != 0)
            return 0;                       /* not a block header at all */
        if (size > brk - p)
            return 0;                       /* the block runs past the break */
        p += size;
        blocks++;
        if (blocks > 100000)
            return 0;
    }
    return p == brk;
}

/* A live allocation must be backed by exactly one block whose header sits
 * ALLOC_BLOCK_HEADER_SIZE bytes before the user pointer, and that block must
 * be marked allocated. */
static int alloc_live_is_backed(unsigned char *ptr)
{
    uint64_t base = libc_heap_runtime_base();
    uint64_t brk = libc_heap_runtime_brk();
    uint64_t want = (uint64_t)(uintptr_t)ptr - ALLOC_BLOCK_HEADER_SIZE;
    uint64_t p = base;

    while (p < brk) {
        uint64_t size = *(uint64_t *)p;
        if (size < ALLOC_BLOCK_HEADER_SIZE || (size % ALLOC_BLOCK_ALIGN) != 0)
            return 0;
        if (p + size > brk)
            return 0;
        if (p == want)
            return *(int *)(p + 8) == 0;   /* is_free must be clear */
        p += size;
    }
    return 0;
}

#define ALLOC_STRESS_MAX_LIVE 64

static void alloc_stress_heap_tiles(void)
{
    static const size_t sizes[] = { 1, 7, 16, 17, 64, 100, 200, 512 };
    static const size_t aligns[] = { 16, 32, 64, 256 };
    struct { unsigned char *ptr; size_t len; unsigned char pat; int alive; }
        live[ALLOC_STRESS_MAX_LIVE];
    uint32_t seed = 0x5eed1234u;
    unsigned char pat = 0xA5;
    int i;
    int op;

    TEST_SUITE("alloc_stress_heap_tiles");

    for (i = 0; i < ALLOC_STRESS_MAX_LIVE; i++) {
        live[i].ptr = NULL;
        live[i].len = 0;
        live[i].pat = 0;
        live[i].alive = 0;
    }

    /* A fixed seed keeps the case deterministic: a regression must fail
     * the same way on every run. */
    for (op = 0; op < 4000; op++) {
        uint32_t r = seed * 1103515245u + 12345u;
        unsigned choice;
        seed = r;

        r = seed * 1103515245u + 12345u;
        seed = r;
        choice = (r >> 8) % 100;

        if (choice < 30) {
            /* ordinary allocation */
            size_t sz = sizes[(r >> 16) % (sizeof(sizes) / sizeof(sizes[0]))];
            unsigned char *p = malloc(sz);
            if (!p) {
                printf("  [FAIL] malloc(%zu) failed after %d operations\n",
                       sz, op);
                assert_true(0);
                return;
            }
            {
                for (i = 0; i < ALLOC_STRESS_MAX_LIVE; i++) {
                    if (!live[i].alive) {
                        live[i].ptr = p;
                        live[i].len = sz;
                        live[i].pat = pat++;
                        live[i].alive = 1;
                        memset(p, live[i].pat, sz);
                        break;
                    }
                }
                if (i == ALLOC_STRESS_MAX_LIVE)
                    free(p);
            }
        } else if (choice < 50) {
            /* aligned allocation: aligned_alloc requires the size to be a
             * multiple of the alignment, so the size is rounded DOWN to
             * one.  (Without that, every pair but the lucky one was
             * EINVAL and this branch never really ran.) */
            size_t al = aligns[(r >> 20) % (sizeof(aligns) / sizeof(aligns[0]))];
            size_t sz = sizes[(r >> 16) % (sizeof(sizes) / sizeof(sizes[0]))];
            /* aligned_alloc requires the size to be a multiple of the
             * alignment, so the size is rounded DOWN to one.  (Without
             * that, every pair but the lucky one was EINVAL and this
             * branch never really ran.)  The ROUNDED size is the one the
             * block holds, so it is also the one the payload uses. */
            size_t want = (sz / al) * al;
            if (!want)
                want = al;
            unsigned char *p = aligned_alloc(al, want);
            if (!p) {
                printf("  [FAIL] aligned_alloc(%zu,%zu) failed after %d "
                       "operations\n", al, want, op);
                assert_true(0);
                return;
            }
            {
                if (((uintptr_t)p % al) != 0) {
                    printf("  [FAIL] aligned_alloc(%zu,%zu) is not %zu-aligned\n",
                           al, sz, al);
                    assert_true(0);
                    return;
                }
                for (i = 0; i < ALLOC_STRESS_MAX_LIVE; i++) {
                    if (!live[i].alive) {
                        live[i].ptr = p;
                        live[i].len = want;
                        live[i].pat = pat++;
                        live[i].alive = 1;
                        memset(p, live[i].pat, want);
                        break;
                    }
                }
                if (i == ALLOC_STRESS_MAX_LIVE)
                    free(p);
            }
        } else if (choice < 65) {
            /* aligned allocation through the POSIX entry point */
            size_t sz = sizes[(r >> 16) % (sizeof(sizes) / sizeof(sizes[0]))];
            size_t al = aligns[(r >> 20) % (sizeof(aligns) / sizeof(aligns[0]))];
            void *out = NULL;
            int rc = posix_memalign(&out, al, sz);
            if (rc != 0 || !out) {
                printf("  [FAIL] posix_memalign(%zu,%zu) failed (rc=%d) after "
                       "%d operations\n", al, sz, rc, op);
                assert_true(0);
                return;
            }
            {
                unsigned char *p = out;
                if (((uintptr_t)p % al) != 0) {
                    printf("  [FAIL] posix_memalign(%zu,%zu) is not %zu-aligned\n",
                           al, sz, al);
                    assert_true(0);
                    return;
                }
                for (i = 0; i < ALLOC_STRESS_MAX_LIVE; i++) {
                    if (!live[i].alive) {
                        live[i].ptr = p;
                        live[i].len = sz;
                        live[i].pat = pat++;
                        live[i].alive = 1;
                        memset(p, live[i].pat, sz);
                        break;
                    }
                }
                if (i == ALLOC_STRESS_MAX_LIVE)
                    free(p);
            }
        } else if (choice < 85) {
            /* reallocation */
            i = (int)((r >> 16) % ALLOC_STRESS_MAX_LIVE);
            if (live[i].alive) {
                size_t sz = sizes[(r >> 20) % (sizeof(sizes) / sizeof(sizes[0]))];
                unsigned char *np = realloc(live[i].ptr, sz);
                if (np) {
                    live[i].ptr = np;
                    live[i].len = sz;
                    live[i].pat = pat++;
                    memset(np, live[i].pat, sz);
                }
            }
        } else {
            /* release */
            i = (int)((r >> 16) % ALLOC_STRESS_MAX_LIVE);
            if (live[i].alive) {
                free(live[i].ptr);
                live[i].alive = 0;
                live[i].ptr = NULL;
            }
        }

        /* The heap must still tile after EVERY operation: that is the
         * invariant a dropped remainder breaks. */
        if (!alloc_heap_tiles()) {
            printf("  [FAIL] the heap no longer tiles after operation %d\n", op);
            assert_true(0);
            return;
        }
        /* Every live allocation must still be backed by its own block. */
        for (i = 0; i < ALLOC_STRESS_MAX_LIVE; i++) {
            if (live[i].alive && !alloc_live_is_backed(live[i].ptr)) {
                printf("  [FAIL] live block %d (%p) is not backed by a block "
                       "header after operation %d\n", i, live[i].ptr, op);
                assert_true(0);
                return;
            }
        }
        /* The payload of every live allocation must be untouched. */
        if ((op % 64) == 0) {
            for (i = 0; i < ALLOC_STRESS_MAX_LIVE; i++) {
                if (!live[i].alive)
                    continue;
                if (!buffer_is(live[i].ptr, live[i].len, live[i].pat)) {
                    printf("  [FAIL] live block %d was overwritten after "
                           "operation %d\n", i, op);
                    assert_true(0);
                    return;
                }
            }
        }
    }
}

/* ── heap hardening: double-free, overflow, underflow, invalid free ──── */

static int wait_child_sigabrt(int64_t pid)
{
    int st = 0;
    if (pid < 0)
        return 0;
    if (waitpid(pid, &st, 0) < 0)
        return 0;
    return WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT;
}

static void double_free_aborts(void)
{
    void *p;
    int64_t pid;

    TEST_SUITE("double_free_aborts");

    p = malloc(32);
    assert_not_null(p);
    if (!p)
        return;
    free(p);

    fflush(NULL);
    pid = fork();
    if (pid == 0) {
        free(p);
        _exit(0);
    }
    assert_true(wait_child_sigabrt(pid));
}

static void heap_overflow_aborts(void)
{
    unsigned char *p;
    int64_t pid;

    TEST_SUITE("heap_overflow_aborts");

    p = (unsigned char *)malloc(10);
    assert_not_null(p);
    if (!p)
        return;
    memset(p, 0x11, 10);

    /* 1-byte tail overflow detected on free(). */
    fflush(NULL);
    pid = fork();
    if (pid == 0) {
        p[10] = 0xAA;
        free(p);
        _exit(0);
    }
    assert_true(wait_child_sigabrt(pid));

    /* 1-byte tail overflow detected on realloc(). */
    fflush(NULL);
    pid = fork();
    if (pid == 0) {
        p[10] = 0xBB;
        (void)realloc(p, 64);
        _exit(0);
    }
    assert_true(wait_child_sigabrt(pid));

    free(p);
}

static void heap_underflow_aborts(void)
{
    unsigned char *p;
    int64_t pid;

    TEST_SUITE("heap_underflow_aborts");

    p = (unsigned char *)malloc(32);
    assert_not_null(p);
    if (!p)
        return;

    fflush(NULL);
    pid = fork();
    if (pid == 0) {
        p[-1] = 0xCC;
        free(p);
        _exit(0);
    }
    assert_true(wait_child_sigabrt(pid));

    free(p);
}

static void invalid_free_aborts(void)
{
    unsigned char stack_buf[32];
    unsigned char *p;
    int64_t pid;

    TEST_SUITE("invalid_free_aborts");

    /* free(NULL) must remain a legal no-op. */
    free(NULL);

    /* Freeing a stack pointer must abort. */
    memset(stack_buf, 0, sizeof(stack_buf));
    fflush(NULL);
    pid = fork();
    if (pid == 0) {
        free(stack_buf);
        _exit(0);
    }
    assert_true(wait_child_sigabrt(pid));

    /* Freeing an unaligned interior pointer must abort. */
    p = (unsigned char *)malloc(64);
    assert_not_null(p);
    if (!p)
        return;

    fflush(NULL);
    pid = fork();
    if (pid == 0) {
        free(p + 8);
        _exit(0);
    }
    assert_true(wait_child_sigabrt(pid));

    free(p);
}

static void corrupt_freelist_aborts(void)
{
    unsigned char *p;
    uint64_t *hdr;
    int64_t pid;

    TEST_SUITE("corrupt_freelist_aborts");

    p = (unsigned char *)malloc(64);
    assert_not_null(p);
    if (!p)
        return;
    free(p);
    hdr = (uint64_t *)(p - ALLOC_BLOCK_HEADER_SIZE);

    /* Corrupted free block size/canary caught by malloc() before reuse. */
    fflush(NULL);
    pid = fork();
    if (pid == 0) {
        hdr[0] = (uint64_t)-16;
        (void)malloc(32);
        _exit(0);
    }
    assert_true(wait_child_sigabrt(pid));

    /* Corrupted free block caught by aligned_alloc() before reuse. */
    fflush(NULL);
    pid = fork();
    if (pid == 0) {
        hdr[3] ^= 0x1ULL;
        (void)aligned_alloc(16, 32);
        _exit(0);
    }
    assert_true(wait_child_sigabrt(pid));
}

static void corrupt_coalesce_aborts(void)
{
    unsigned char *a;
    unsigned char *b;
    unsigned char *c;
    uint64_t *b_hdr;
    int64_t pid;

    TEST_SUITE("corrupt_coalesce_aborts");

    a = (unsigned char *)malloc(65536);
    b = (unsigned char *)malloc(65536);
    c = (unsigned char *)malloc(65536);
    assert_not_null(a);
    assert_not_null(b);
    assert_not_null(c);
    if (!a || !b || !c)
        return;

    b_hdr = (uint64_t *)(b - ALLOC_BLOCK_HEADER_SIZE);

    /* Unlinked block forged as free with a valid canary next to `a`:
     * try_coalesce must abort on the missing freelist entry. */
    fflush(NULL);
    pid = fork();
    if (pid == 0) {
        uint64_t sz = b_hdr[0];
        uint64_t rot_sz = (sz << 13) | (sz >> (64 - 13));
        b_hdr[1] = ((uint64_t)0xF4EEB10CU << 32) | 1ULL;
        b_hdr[2] = 0;
        b_hdr[3] = ((uint64_t)(uintptr_t)b_hdr) ^ rot_sz ^ 0x9E3779B97F4A7C15ULL;
        free(a);
        _exit(0);
    }
    assert_true(wait_child_sigabrt(pid));

    /* Free `b`, then corrupt its canary: try_coalesce on free(a) must abort. */
    free(b);
    fflush(NULL);
    pid = fork();
    if (pid == 0) {
        b_hdr[3] ^= 0x55ULL;
        free(a);
        _exit(0);
    }
    assert_true(wait_child_sigabrt(pid));

    free(a);
    free(c);
}

static void wraparound_size_aborts(void)
{
    unsigned char *p;
    uint64_t *hdr;
    int64_t pid;

    TEST_SUITE("wraparound_size_aborts");

    p = (unsigned char *)malloc(64);
    assert_not_null(p);
    if (!p)
        return;
    hdr = (uint64_t *)(p - ALLOC_BLOCK_HEADER_SIZE);

    /* A wraparound size (hdr + size wraps past brk) must be caught. */
    fflush(NULL);
    pid = fork();
    if (pid == 0) {
        hdr[0] = (uint64_t)-16;
        free(p);
        _exit(0);
    }
    assert_true(wait_child_sigabrt(pid));

    free(p);
}

TEST_LIST_BEGIN
    TEST_ENTRY(calloc_product_wrap),
    TEST_ENTRY(malloc_roundup_wrap),
    TEST_ENTRY(reallocarray_failure_preserves_bytes),
    TEST_ENTRY(aligned_block_lifecycle),
    TEST_ENTRY(posix_memalign_bad_alignment),
    TEST_ENTRY(aligned_alloc_bad_multiple),
    TEST_ENTRY(alloc_stress_heap_tiles),
    TEST_ENTRY(double_free_aborts),
    TEST_ENTRY(heap_overflow_aborts),
    TEST_ENTRY(heap_underflow_aborts),
    TEST_ENTRY(invalid_free_aborts),
    TEST_ENTRY(corrupt_freelist_aborts),
    TEST_ENTRY(corrupt_coalesce_aborts),
    TEST_ENTRY(wraparound_size_aborts),
TEST_LIST_END

int main(void)
{
    /* The mock heap starts at its base; the allocator keeps its own
     * state in statics, so this is the only place the reset may happen. */
    libc_heap_runtime_reset();

    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
