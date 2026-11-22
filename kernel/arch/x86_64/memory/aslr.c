#include <arch/x86_64/aslr.h>
#include <arch/x86_64/user_layout.h>
#include <random/random.h>
#include <sched/task.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

_Static_assert(X86_USER_MMAP_RND_BASE >= X86_USER_STACK_END,
               "x86_64 ASLR mmap base must sit above user stack");
_Static_assert((X86_USER_MMAP_RND_BASE & 0xFFFULL) == 0,
               "x86_64 ASLR mmap base must be 4 KiB aligned");
_Static_assert(X86_USER_MMAP_RND_BITS >= 20U && X86_USER_MMAP_RND_BITS <= 32U,
               "x86_64 ASLR entropy width out of supported range");
_Static_assert(X86_USER_MMAP_RND_END > X86_USER_MMAP_RND_BASE,
               "x86_64 ASLR window must not wrap");
_Static_assert(X86_USER_MMAP_RND_END < X86_USER_MMAP_MAX_ADDR,
               "x86_64 ASLR window must stay below canonical mmap ceiling");
_Static_assert(X86_USER_MMAP_MAX_ADDR <= X86_USER_ADDR_LIMIT,
               "x86_64 mmap ceiling must not exceed user addr_limit");
_Static_assert((X86_USER_MMAP_MAX_ADDR & 0xFFFULL) == 0,
               "x86_64 mmap ceiling must be 4 KiB aligned");
_Static_assert(X86_USER_ADDR_LIMIT < 0x0000800000000000ULL,
               "x86_64 user addr_limit must stay in canonical low half");

_Static_assert(X86_USER_PIE_LINK_END > 0 && (X86_USER_PIE_LINK_END & 0xFFFULL) == 0,
               "x86_64 PIE link end must be nonzero and 4 KiB aligned");
_Static_assert(X86_USER_PIE_BIAS_BASE >= X86_USER_MIN_ADDR &&
               (X86_USER_PIE_BIAS_BASE & 0xFFFULL) == 0,
               "x86_64 PIE bias base must be >= X86_USER_MIN_ADDR and 4 KiB aligned");
_Static_assert(X86_USER_PIE_BIAS_BITS == 16U &&
               X86_USER_PIE_BIAS_END > X86_USER_PIE_BIAS_BASE,
               "x86_64 PIE bias window invalid");
_Static_assert(X86_USER_PIE_MAX_END <= X86_USER_HEAP_LIMIT,
               "x86_64 PIE max runtime end must not exceed heap limit");

uint64_t x86_aslr_derive_mmap_base(uint64_t raw_entropy)
{
    uint64_t mask = (UINT64_C(1) << X86_USER_MMAP_RND_BITS) - 1U;
    return X86_USER_MMAP_RND_BASE + ((raw_entropy & mask) << 12U);
}

static void aslr_wipe_bytes(uint8_t *buf, size_t len)
{
    volatile uint8_t *p = buf;
    for (size_t i = 0; i < len; i++)
        p[i] = 0;
}

int x86_aslr_init_mmap_base(mm_t *mm)
{
    if (!mm)
        return -EINVAL;

    uint8_t entropy[32];
    if (!kernel_random_get_strong(entropy)) {
        /* WEAK fallback paths may have written DRBG bytes into entropy[]. */
        aslr_wipe_bytes(entropy, sizeof(entropy));
        return -EAGAIN;
    }

    uint64_t raw = 0;
    for (size_t i = 0; i < 8; i++)
        raw |= ((uint64_t)entropy[i]) << (i * 8U);

    aslr_wipe_bytes(entropy, sizeof(entropy));
    mm->mmap_base = x86_aslr_derive_mmap_base(raw);
    return 0;
}

int x86_aslr_get_pie_bias(uint64_t *out_bias)
{
    if (!out_bias)
        return -EINVAL;

    uint8_t entropy[32];
    if (!kernel_random_get_strong(entropy)) {
        aslr_wipe_bytes(entropy, sizeof(entropy));
        return -EAGAIN;
    }

    uint64_t raw = ((uint64_t)entropy[0]) | (((uint64_t)entropy[1]) << 8U);
    aslr_wipe_bytes(entropy, sizeof(entropy));
    *out_bias = X86_USER_PIE_BIAS_BASE + ((raw & 0xFFFFULL) << 12U);
    return 0;
}
