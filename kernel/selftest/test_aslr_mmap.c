#ifdef OS01_SELFTEST

#include <core/selftest.h>
#include <core/printk.h>
#include <arch/x86_64/aslr.h>
#include <arch/x86_64/user_layout.h>
#include <arch/random.h>
#include <random/random.h>
#include <sched/task.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

#define ASLR_FAIL(fmt, ...) do {                                    \
    serial_printk("[selftest] aslr_mmap: " fmt "\n", ##__VA_ARGS__); \
    kernel_random_test_set_pool_quality(saved_q, saved_ready);      \
    return -1;                                                      \
} while (0)

int test_aslr_mmap(void)
{
    arch_entropy_source_t saved_q = random_get_pool_quality();
    bool saved_ready = random_is_ready();

    uint8_t probe[32];
    bool arch_has_strong = arch_random_get_strong(probe);
    memset(probe, 0, sizeof(probe));

    /* 1. NULL mm_t rejected without crashing */
    if (x86_aslr_init_mmap_base(NULL) != -EINVAL)
        ASLR_FAIL("x86_aslr_init_mmap_base(NULL) did not return -EINVAL");
    if (x86_aslr_init_stack_base(NULL) != -EINVAL)
        ASLR_FAIL("x86_aslr_init_stack_base(NULL) did not return -EINVAL");

    /* 2. STRONG pool: 8 consecutive initializations produce varied 4 KiB-aligned
     *    bases inside [X86_USER_MMAP_RND_BASE, X86_USER_MMAP_RND_END). */
    kernel_random_test_set_pool_quality(ARCH_ENTROPY_STRONG, true);
    uint64_t bases[8];
    int unique = 0;
    for (int i = 0; i < 8; i++) {
        mm_t mm;
        memset(&mm, 0, sizeof(mm));
        mm.mmap_base = 0x40000000ULL;
        int rc = x86_aslr_init_mmap_base(&mm);
        if (rc != 0)
            ASLR_FAIL("STRONG pool iter %d returned %d", i, rc);
        if ((mm.mmap_base & 0xFFFULL) != 0)
            ASLR_FAIL("STRONG pool iter %d unaligned: %lx", i, mm.mmap_base);
        if (mm.mmap_base < X86_USER_MMAP_RND_BASE ||
            mm.mmap_base >= X86_USER_MMAP_RND_END)
            ASLR_FAIL("STRONG pool iter %d out of window: %lx", i, mm.mmap_base);

        bases[i] = mm.mmap_base;
        bool seen = false;
        for (int j = 0; j < i; j++) {
            if (bases[j] == bases[i]) {
                seen = true;
                break;
            }
        }
        if (!seen)
            unique++;
    }
    if (unique < 7)
        ASLR_FAIL("STRONG pool produced too few unique bases: %d/8", unique);

    /* 3. WEAK and NONE pools: fail-closed when !arch_has_strong, else fallback */
    const arch_entropy_source_t weak_modes[2] = { ARCH_ENTROPY_WEAK, ARCH_ENTROPY_NONE };
    const bool weak_ready[2] = { true, false };
    for (int m = 0; m < 2; m++) {
        kernel_random_test_set_pool_quality(weak_modes[m], weak_ready[m]);
        mm_t mm;
        memset(&mm, 0, sizeof(mm));
        mm.mmap_base = 0x40000000ULL;
        int rc = x86_aslr_init_mmap_base(&mm);
        if (!arch_has_strong) {
            if (rc != -EAGAIN)
                ASLR_FAIL("mode %d without hw strong returned %d (expected -EAGAIN)", m, rc);
            if (mm.mmap_base != 0x40000000ULL)
                ASLR_FAIL("mode %d without hw strong mutated mmap_base to %lx", m, mm.mmap_base);
        } else {
            if (rc != 0)
                ASLR_FAIL("mode %d with hw strong returned %d (expected 0)", m, rc);
            if ((mm.mmap_base & 0xFFFULL) != 0 ||
                mm.mmap_base < X86_USER_MMAP_RND_BASE ||
                mm.mmap_base >= X86_USER_MMAP_RND_END)
                ASLR_FAIL("mode %d with hw strong bad base %lx", m, mm.mmap_base);
        }
    }

    kernel_random_test_set_pool_quality(saved_q, saved_ready);

    /* 4. Stack base ASLR: 8 consecutive initializations produce varied
     *    2 MiB-aligned bases inside [X86_USER_STACK_RND_BASE, X86_USER_STACK_RND_END),
     *    and WEAK/NONE pools fail closed when !arch_has_strong. */
    kernel_random_test_set_pool_quality(ARCH_ENTROPY_STRONG, true);
    uint64_t sbases[8];
    int sunique = 0;
    for (int i = 0; i < 8; i++) {
        mm_t mm;
        memset(&mm, 0, sizeof(mm));
        mm.start_stack = X86_USER_STACK_BASE;
        int rc = x86_aslr_init_stack_base(&mm);
        if (rc != 0)
            ASLR_FAIL("stack STRONG pool iter %d returned %d", i, rc);
        if ((mm.start_stack & (X86_USER_STACK_SIZE - 1U)) != 0)
            ASLR_FAIL("stack STRONG pool iter %d unaligned: %lx", i, mm.start_stack);
        if (mm.start_stack < X86_USER_STACK_RND_BASE ||
            mm.start_stack >= X86_USER_STACK_RND_END)
            ASLR_FAIL("stack STRONG pool iter %d out of window: %lx", i, mm.start_stack);

        sbases[i] = mm.start_stack;
        bool seen = false;
        for (int j = 0; j < i; j++) {
            if (sbases[j] == sbases[i]) {
                seen = true;
                break;
            }
        }
        if (!seen)
            sunique++;
    }
    if (sunique < 7)
        ASLR_FAIL("stack STRONG pool produced too few unique bases: %d/8", sunique);

    for (int m = 0; m < 2; m++) {
        kernel_random_test_set_pool_quality(weak_modes[m], weak_ready[m]);
        mm_t mm;
        memset(&mm, 0, sizeof(mm));
        mm.start_stack = X86_USER_STACK_BASE;
        int rc = x86_aslr_init_stack_base(&mm);
        if (!arch_has_strong) {
            if (rc != -EAGAIN)
                ASLR_FAIL("stack mode %d without hw strong returned %d (expected -EAGAIN)", m, rc);
            if (mm.start_stack != X86_USER_STACK_BASE)
                ASLR_FAIL("stack mode %d without hw strong mutated start_stack to %lx", m, mm.start_stack);
        } else if (rc != 0 || (mm.start_stack & (X86_USER_STACK_SIZE - 1U)) != 0 ||
                   mm.start_stack < X86_USER_STACK_RND_BASE ||
                   mm.start_stack >= X86_USER_STACK_RND_END) {
            ASLR_FAIL("stack mode %d with hw strong bad rc=%d base=%lx", m, rc, mm.start_stack);
        }
    }

    kernel_random_test_set_pool_quality(saved_q, saved_ready);
    return 0;
}

#endif /* OS01_SELFTEST */
