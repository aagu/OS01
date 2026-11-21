/* kernel/arch/aarch64/memory/user_asid.c — AArch64 8-bit user ASID pool.
 *
 * Manages tags 1..255 under an IRQ-safe spinlock.  A released tag stays
 * occupied in the bitmap until inner-shareable TLB invalidation finishes,
 * and the pool lock is never held across TLBI.
 */

#include <arch/aarch64/user_asid.h>
#include <arch/spinlock.h>
#include <stdbool.h>
#include <stdint.h>
#ifdef AARCH64_USER_RUNTIME_TEST
#include <selftest/aarch64_user_runtime.h>
#endif

static uint64_t g_asid_bitmap[4] = { 1ULL, 0ULL, 0ULL, 0ULL }; /* bit 0 reserved */
static spinlock_T g_asid_lock = { .lock = 1UL };

static inline bool asid_is_set_locked(uint8_t asid)
{
    return (g_asid_bitmap[asid >> 6] & (UINT64_C(1) << (asid & 63))) != 0;
}

static inline void asid_set_locked(uint8_t asid)
{
    g_asid_bitmap[asid >> 6] |= (UINT64_C(1) << (asid & 63));
}

static inline void asid_clear_locked(uint8_t asid)
{
    g_asid_bitmap[asid >> 6] &= ~(UINT64_C(1) << (asid & 63));
}

__attribute__((weak)) void aarch64_asid_tlbi(uint8_t asid)
{
#ifdef __aarch64__
    if (asid == 0) {
        __asm__ __volatile__(
            "dsb ishst\n\t"
            "tlbi vmalle1is\n\t"
            "dsb ish\n\t"
            "isb"
            ::: "memory");
    } else {
        uint64_t operand = ((uint64_t)asid) << 48;
        __asm__ __volatile__(
            "dsb ishst\n\t"
            "tlbi aside1is, %0\n\t"
            "dsb ish\n\t"
            "isb"
            :: "r"(operand) : "memory");
    }
#else
    (void)asid;
#endif
#ifdef AARCH64_USER_RUNTIME_TEST
    aarch64_user_runtime_test_note_asid_tlbi(asid);
#endif
#ifdef AARCH64_USER_RUNTIME_CASE_MIGRATION
    aarch64_user_mig_note_tlbi(asid, asid == 0);
#endif
}

#ifdef AARCH64_USER_RUNTIME_CASE_MIGRATION
unsigned aarch64_asid_test_in_use_count(void)
{
    uint64_t daif = spin_lock_irqsave(&g_asid_lock);
    unsigned cnt = 0;
    for (unsigned word = 0; word < 4; word++) {
        uint64_t bits = g_asid_bitmap[word];
        if (word == 0)
            bits &= ~1ULL; /* exclude reserved ASID 0 */
        cnt += (unsigned)__builtin_popcountll(bits);
    }
    spin_unlock_irqrestore(&g_asid_lock, daif);
    return cnt;
}
#endif

uint8_t aarch64_asid_alloc(void)
{
    uint64_t daif = spin_lock_irqsave(&g_asid_lock);
    for (unsigned word = 0; word < 4; word++) {
        uint64_t free_mask = ~g_asid_bitmap[word];
        if (free_mask == 0)
            continue;
        unsigned bit = (unsigned)__builtin_ctzll(free_mask);
        uint8_t asid = (uint8_t)((word << 6) + bit);
        if (asid == 0)
            continue;
        asid_set_locked(asid);
        spin_unlock_irqrestore(&g_asid_lock, daif);
        return asid;
    }
    spin_unlock_irqrestore(&g_asid_lock, daif);
    return 0;
}

void aarch64_asid_release_quiesced(uint8_t asid)
{
    if (asid == 0)
        return;

    uint64_t daif = spin_lock_irqsave(&g_asid_lock);
    asid_clear_locked(asid);
    spin_unlock_irqrestore(&g_asid_lock, daif);
#ifdef AARCH64_USER_RUNTIME_TEST
    aarch64_user_runtime_test_note_asid_release(asid);
#endif
}

void aarch64_asid_release(uint8_t asid)
{
    if (asid == 0)
        return;

    uint64_t daif = spin_lock_irqsave(&g_asid_lock);
    bool owned = asid_is_set_locked(asid);
    spin_unlock_irqrestore(&g_asid_lock, daif);
    if (!owned)
        return;

    /* Slot remains occupied in g_asid_bitmap until TLBI completes. */
    aarch64_asid_tlbi(asid);
    aarch64_asid_release_quiesced(asid);
}
