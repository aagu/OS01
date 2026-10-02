/*
 * kernel/memory/pmm_boot.c — checked PMM metadata layout calculator
 * (aarch64 M1 plan Task 1).
 *
 * Extracted from production kernel/memory/pmm.c Step 3 into a single
 * checked function. The calculator mirrors the LP64 production math
 * byte-for-byte and adds overflow detection on every multiplication,
 * addition, and align step.
 *
 * The calculator is arch-neutral (only depends on <memory/pmm.h> for
 * sizeof(struct Page) / sizeof(struct Zone), <memory/memory_map.h> for
 * MEMORY_RANGE_MAX, and <errno.h> for return codes). Both kernel
 * architectures link the same object; the hosttest (test_m1_layout)
 * links the same source for end-to-end assertion.
 *
 * Offsets are RELATIVE to the caller's base_va (already aligned to 4
 * KiB). Production pmm.c does its own base alignment; the calculator
 * does not duplicate that work.
 */

#include <errno.h>

#include <memory/memory_map.h>   /* MEMORY_RANGE_MAX */
#include <memory/pmm.h>          /* struct Page / Zone */
#include <memory/pmm_boot.h>

/* Metadata tail reservation (production pmm.c Step 5: 32*sizeof(long)).
 * Kept as a single constant so the value lives in exactly one place. */
#define PMM_BOOT_TAIL_LONG_WORDS 32UL
#define PMM_BOOT_TAIL_BYTES      (PMM_BOOT_TAIL_LONG_WORDS * sizeof(unsigned long))

static inline int add_overflows(uint64_t a, uint64_t b)
{
    return a > UINT64_MAX - b;
}

static inline int mul_overflows(uint64_t a, uint64_t b)
{
    return b != 0 && a > UINT64_MAX / b;
}

/* align_up(x, a): smallest y >= x with y % a == 0. Sets *err on
 * overflow (returns 0 in that case — caller treats 0 as a fatal
 * overflow signal). */
static inline uint64_t align_up_checked(uint64_t x, uint64_t a, int *err)
{
    uint64_t rem = x % a;
    if (rem == 0) return x;
    uint64_t pad = a - rem;
    if (add_overflows(x, pad)) { *err = 1; return 0; }
    return x + pad;
}

static inline void zero_layout(struct pmm_layout *out)
{
    out->bits_map_off       = 0;
    out->bits_length        = 0;
    out->pages_struct_off   = 0;
    out->pages_length       = 0;
    out->zones_struct_off   = 0;
    out->zones_length       = 0;
    out->end_of_struct_off  = 0;
    out->total_bytes        = 0;
    out->metadata_end_pa    = 0;   /* populated by pmm_init after this
                                    * function returns; left at 0 by the
                                    * calculator since Virt_To_Phy is
                                    * arch-specific. */
}

int pmm_layout_calculate(uint64_t base_va, uint64_t span_pages,
                         struct pmm_layout *out)
{
    /* base_va is informational; the production code aligns start_brk
     * to 4 KiB before passing it. The calculator's offsets are
     * independent of base_va value (and base_va alignment). */
    (void)base_va;

    /* Brief contract: NULL returns -EINVAL with no out write; all
     * other failures zero out and return a negative errno. */
    if (out == NULL) return -EINVAL;
    zero_layout(out);

    if (span_pages == 0) return -EINVAL;

    /* ── bits_length: ((span_pages + 63) & ~63) / 8 ──────────── */
    if (add_overflows(span_pages, 63)) return -EOVERFLOW;
    uint64_t bits_qwords = (span_pages + 63UL) & ~63UL;
    uint64_t bits_length = bits_qwords / 8UL;

    /* ── pages_length: align_up(span_pages * sizeof(struct Page),
     *                         sizeof(long)) ───────────────── */
    if (mul_overflows(span_pages, sizeof(struct Page)))
        return -EOVERFLOW;
    uint64_t pages_bytes = span_pages * (uint64_t)sizeof(struct Page);
    int err = 0;
    uint64_t pages_length = align_up_checked(pages_bytes, sizeof(long), &err);
    if (err) return -EOVERFLOW;

    /* ── zones_length: align_up(MEMORY_RANGE_MAX * sizeof(struct Zone),
     *                         sizeof(long)) ──────────────── */
    if (mul_overflows((uint64_t)MEMORY_RANGE_MAX, sizeof(struct Zone)))
        return -EOVERFLOW;
    uint64_t zones_bytes = (uint64_t)MEMORY_RANGE_MAX * sizeof(struct Zone);
    err = 0;
    uint64_t zones_length = align_up_checked(zones_bytes, sizeof(long), &err);
    if (err) return -EOVERFLOW;

    /* ── bits_map_off = 0 (relative to aligned base_va) ──────── */
    uint64_t bits_map_off = 0;

    /* ── pages_struct_off: align_up_4k(bits_map_off + bits_length) */
    if (add_overflows(bits_map_off, bits_length)) return -EOVERFLOW;
    err = 0;
    uint64_t pages_struct_off =
        align_up_checked(bits_map_off + bits_length, 0x1000UL, &err);
    if (err) return -EOVERFLOW;

    /* ── zones_struct_off: align_up_4k(pages_struct_off + pages_length) */
    if (add_overflows(pages_struct_off, pages_length)) return -EOVERFLOW;
    err = 0;
    uint64_t zones_struct_off =
        align_up_checked(pages_struct_off + pages_length, 0x1000UL, &err);
    if (err) return -EOVERFLOW;

    /* ── end_of_struct_off: align_down(zones_struct_off + zones_length
     *                             + 32 * sizeof(unsigned long),
     *                             sizeof(long)) ────────────── */
    if (add_overflows(zones_struct_off, zones_length)) return -EOVERFLOW;
    if (add_overflows(zones_struct_off + zones_length, PMM_BOOT_TAIL_BYTES))
        return -EOVERFLOW;
    uint64_t end_of_struct_off =
        (zones_struct_off + zones_length + PMM_BOOT_TAIL_BYTES)
        & ~(sizeof(long) - 1UL);

    /* ── total_bytes: align_up_4k(end_of_struct_off) ────────── */
    err = 0;
    uint64_t total_bytes =
        align_up_checked(end_of_struct_off, 0x1000UL, &err);
    if (err) return -EOVERFLOW;

    /* Commit the layout. */
    out->bits_map_off      = bits_map_off;
    out->bits_length       = bits_length;
    out->pages_struct_off  = pages_struct_off;
    out->pages_length      = pages_length;
    out->zones_struct_off  = zones_struct_off;
    out->zones_length      = zones_length;
    out->end_of_struct_off = end_of_struct_off;
    out->total_bytes       = total_bytes;
    return 0;
}
