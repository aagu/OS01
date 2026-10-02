#ifndef _KERNEL_PMM_BOOT_H
#define _KERNEL_PMM_BOOT_H

/*
 * kernel/include/memory/pmm_boot.h — checked PMM metadata layout
 * calculator. aarch64 M1 plan Task 1.
 *
 * Extracts the sizing math that production kernel/memory/pmm.c
 * performs inline at pmm_init() (the original "Step 3" block in pmm.c)
 * into a single, overflow-checked entry point that the kernel and the
 * aarch64 preflight both consume. The struct fields are RELATIVE
 * offsets from `base_va` (the caller-aligned bits_map address);
 * `total_bytes` is the buffer size with trailing 4 KiB slack included.
 *
 * Layout contract (mirrors production pmm.c Step 3):
 *   bits_map_off      = 0
 *   bits_length       = ((span_pages + 63) & ~63) / 8
 *   pages_struct_off  = align_up_4k(bits_map_off + bits_length)
 *   pages_length      = align_up(span_pages * sizeof(struct Page), sizeof(long))
 *   zones_struct_off  = align_up_4k(pages_struct_off + pages_length)
 *   zones_length      = align_up(MEMORY_RANGE_MAX * sizeof(struct Zone), sizeof(long))
 *   end_of_struct_off = align_down(zones_struct_off + zones_length +
 *                                   32 * sizeof(unsigned long), sizeof(long))
 *   total_bytes       = align_up_4k(end_of_struct_off)
 *
 * The 32*sizeof(unsigned long) tail matches the existing pmm.c Step 5
 * reservation. Page count tracks the SPAN, not the RAM total — sparse
 * zones with a wide PA span still produce a Page array sized for the
 * span.
 *
 * Return contract:
 *   - 0: layout written; out holds valid, consistent offsets.
 *   - -EINVAL: out == NULL or span_pages == 0. (NULL writes nothing;
 * -EINVAL clears out per brief: "其余失败清零 out".)
 *   - -EOVERFLOW: arithmetic overflow or align overflow; out is
 * zeroed (the zero layout is NOT usable — see "失败不产生可使用 layout").
 *
 * The calculator does NOT validate base_va alignment; the caller is
 * expected to align base_va up to 4 KiB before passing it in (mirrors
 * production pmm.c which does `(start_brk + 0xFFF) & ~0xFFF`). Offsets
 * are independent of base_va value.
 */

#include <stdint.h>

struct pmm_layout {
    uint64_t bits_map_off;
    uint64_t bits_length;
    uint64_t pages_struct_off;
    uint64_t pages_length;
    uint64_t zones_struct_off;
    uint64_t zones_length;
    uint64_t end_of_struct_off;
    uint64_t total_bytes;
};

int pmm_layout_calculate(uint64_t base_va, uint64_t span_pages,
                         struct pmm_layout *out);

#endif /* _KERNEL_PMM_BOOT_H */
