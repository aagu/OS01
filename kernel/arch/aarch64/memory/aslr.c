#include <arch/aarch64/aslr.h>
#include <arch/aarch64/mmu.h>
#include <memory/memory.h>
#include <random/random.h>
#include <sched/task.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AARCH64_USER_MMAP_FLOOR UINT64_C(0x40000000)

_Static_assert(AARCH64_USER_MMAP_RND_BASE >= AARCH64_USER_MMAP_FLOOR,
               "aarch64 ASLR mmap base must sit at or above USER_VM_MMAP_BASE");
_Static_assert((AARCH64_USER_MMAP_RND_BASE & 0xFFFULL) == 0,
               "aarch64 ASLR mmap base must be 4 KiB aligned");
_Static_assert(AARCH64_USER_MMAP_RND_BITS >= 20U && AARCH64_USER_MMAP_RND_BITS <= 32U,
               "aarch64 ASLR mmap entropy width out of supported range");
_Static_assert(AARCH64_USER_MMAP_RND_END > AARCH64_USER_MMAP_RND_BASE,
               "aarch64 ASLR mmap window must not wrap");
_Static_assert(AARCH64_USER_MMAP_RND_END < AARCH64_USER_LIMIT,
               "aarch64 ASLR mmap window must stay below AARCH64_USER_LIMIT");

_Static_assert(AARCH64_USER_STACK_RND_BASE >= AARCH64_USER_STACK_BASE,
               "aarch64 ASLR stack base must sit at or above floor");
_Static_assert((AARCH64_USER_STACK_RND_BASE & 0xFFFULL) == 0,
               "aarch64 ASLR stack base must be 4 KiB aligned");
_Static_assert(AARCH64_USER_STACK_RND_BITS >= 12U && AARCH64_USER_STACK_RND_BITS <= 20U,
               "aarch64 ASLR stack entropy width out of supported range");
_Static_assert(AARCH64_USER_STACK_RND_END > AARCH64_USER_STACK_RND_BASE,
               "aarch64 ASLR stack window must not wrap");
_Static_assert(AARCH64_USER_STACK_RND_END +
               (uint64_t)AARCH64_PRODUCT_STACK_PAGES * UINT64_C(4096) <=
               AARCH64_USER_MMAP_FLOOR,
               "aarch64 ASLR stack window must stay below USER_VM_MMAP_BASE");

static void aarch64_aslr_wipe_bytes(uint8_t *buf, size_t len)
{
    volatile uint8_t *p = buf;
    for (size_t i = 0; i < len; i++)
        p[i] = 0;
}

static int aarch64_aslr_sample_strong_u64(uint64_t *out_raw)
{
    uint8_t entropy[32];
    if (!kernel_random_get_strong(entropy)) {
        /* WEAK fallback paths may have written DRBG bytes into entropy[]. */
        aarch64_aslr_wipe_bytes(entropy, sizeof(entropy));
        return -EAGAIN;
    }

    uint64_t raw = 0;
    for (size_t i = 0; i < 8; i++)
        raw |= ((uint64_t)entropy[i]) << (i * 8U);

    aarch64_aslr_wipe_bytes(entropy, sizeof(entropy));
    *out_raw = raw;
    return 0;
}

uint64_t aarch64_aslr_derive_mmap_base(uint64_t raw_entropy)
{
    uint64_t mask = (UINT64_C(1) << AARCH64_USER_MMAP_RND_BITS) - 1U;
    return AARCH64_USER_MMAP_RND_BASE + ((raw_entropy & mask) << 12U);
}

int aarch64_aslr_init_mmap_base(mm_t *mm)
{
    if (!mm)
        return -EINVAL;

    uint64_t raw = 0;
    int rc = aarch64_aslr_sample_strong_u64(&raw);
    if (rc != 0)
        return rc;

    mm->mmap_base = aarch64_aslr_derive_mmap_base(raw);
    return 0;
}

uint64_t aarch64_aslr_derive_stack_base(uint64_t raw_entropy)
{
    uint64_t mask = (UINT64_C(1) << AARCH64_USER_STACK_RND_BITS) - 1U;
    return AARCH64_USER_STACK_RND_BASE + ((raw_entropy & mask) << 12U);
}

int aarch64_aslr_init_stack_base(mm_t *mm)
{
    if (!mm)
        return -EINVAL;

    uint64_t raw = 0;
    int rc = aarch64_aslr_sample_strong_u64(&raw);
    if (rc != 0)
        return rc;

    mm->start_stack = aarch64_aslr_derive_stack_base(raw);
    return 0;
}
