/* kernel/arch/x86_64/pmm_arch.c — strong overrides for x86_64.
 *
 * Translates E820 entries to MEMORY_RANGE[] with kernel-LMA
 * (Virt_To_Phy(_text/_edata)), boot handoff (0x60000..0x64000),
 * and SMP trampoline (TRAMPOLINE_BASE..TRAMPOLINE_BASE+blob size)
 * excluded. Surviving fragments rounded inward to 2 MiB,
 * sorted/merged. Returns 0 on input error or capacity overflow.
 */

#include <stdint.h>
#include <stddef.h>

#include <core/bootinfo.h>
#include <arch/x86_64/bootinfo_x86.h>   /* struct E820_ENTRY, BOOT_MEMORY_FORMAT_E820 (x86_64-only) */
#include <memory/memory.h>            /* Virt_To_Phy macro (existing x86_64 helper) */
#include <memory/memory_map.h>
#include <arch/x86_64/handoff_layout.h>
#include <arch/x86_64/trampoline.h>

#define E820_TYPE_RAM  1

static uint64_t round_up(uint64_t v, uint64_t a) {
    return (v + a - 1) & ~(a - 1);
}
static uint64_t round_down(uint64_t v, uint64_t a) {
    return v & ~(a - 1);
}

/* Strong override of the weak default in kernel/memory/pmm_arch.c.
 * No attribute: the linker resolves by symbol-name match against the
 * weak default, and a strong definition automatically wins. */
size_t pmm_arch_normalize(const struct boot_context *ctx,
                          struct MEMORY_RANGE *out)
{
    if (!ctx || !out) return 0;
    if ((ctx->flags & BOOT_CONTEXT_HAS_MEMORY_MAP) == 0) return 0;
    if (ctx->memory.format != BOOT_MEMORY_FORMAT_E820) return 0;
    if (ctx->memory.entry_size < sizeof(struct E820_ENTRY)) return 0;
    if (ctx->memory.entry_count == 0) return 0;

    /* Compute exclusion intervals. The kernel-LMA exclude spans
     * _text to _edata (text + rodata + data); BSS pages between
     * _edata and _end are deliberately left in the output range so
     * that Step 7's `end_of_struct` walk has somewhere to mark
     * them allocated. Excluding them with `_end` would create a
     * sparse pages_struct that the walk can't reach. */
    uint64_t kernel_lma_start = Virt_To_Phy((uint64_t)&_text);
    uint64_t kernel_lma_end   = Virt_To_Phy((uint64_t)&_edata);
    if (kernel_lma_start >= kernel_lma_end) return 0;
    uint64_t handoff_start = X86_64_HANDOFF_BASE;
    uint64_t handoff_end   = X86_64_HANDOFF_END;
    uint64_t tramp_start = TRAMPOLINE_BASE;
    uint64_t tramp_end   = TRAMPOLINE_BASE +
        ((uint64_t)&_binary_arch_x86_64_trampoline_bin_end -
         (uint64_t)&_binary_arch_x86_64_trampoline_bin_start);

    struct E820_ENTRY *entries =
        (struct E820_ENTRY *)(uintptr_t)ctx->memory.entries;
    size_t out_count = 0;

    for (uint32_t i = 0; i < ctx->memory.entry_count; i++) {
        enum MEMORY_TYPE t;
        switch (entries[i].type) {
        case E820_TYPE_RAM: t = MEMORY_TYPE_RAM; break;
        case 2: t = MEMORY_TYPE_RESERVED; break;
        case 3: t = MEMORY_TYPE_ACPI_RECLAIM; break;
        case 4: t = MEMORY_TYPE_ACPI_NVS; break;
        default: t = MEMORY_TYPE_RESERVED; break;
        }
        /* pmm_init's Step 2 walks only MEMORY_TYPE_RAM ranges, so the
         * non-RAM entries below are reserved for future consumers
         * (e.g. ACPI reclaim after init). They are still emitted so the
         * full MEMORY_RANGE[] surface is available. */
        uint64_t s = entries[i].address;
        uint64_t e = entries[i].address + entries[i].length;
        /* Walk through up to 4 exclusions (kernel, handoff, trampoline) */
        struct { uint64_t s, e; } frags[8];
        size_t fcount = 1;
        frags[0].s = s; frags[0].e = e;
        const struct { uint64_t s, e; } excl[3] = {
            {kernel_lma_start, kernel_lma_end},
            {handoff_start, handoff_end},
            {tramp_start, tramp_end},
        };
        for (size_t k = 0; k < 3 && fcount > 0; k++) {
            /* NOTE: 'next' MUST share the anonymous-struct type of
             * 'frags' above so that element-wise assignment
             * type-checks (two distinct anonymous struct types with
             * identical fields are NOT assignment-compatible in C). */
            struct { uint64_t s, e; } next[16];
            size_t ncount = 0;
            for (size_t j = 0; j < fcount; j++) {
                uint64_t a = frags[j].s, b = frags[j].e;
                if (b <= excl[k].s || a >= excl[k].e) {
                    /* Cast to (uint64_t[2]) via memcpy-equivalent:
                     * two anonymous-struct types with identical
                     * fields cannot be assigned to one another in
                     * strict C, so we explicitly initialise each
                     * field. This sidesteps the type mismatch while
                     * preserving the algorithm. */
                    if (ncount < 16) {
                        next[ncount].s = frags[j].s;
                        next[ncount].e = frags[j].e;
                        ncount++;
                    }
                } else {
                    if (a < excl[k].s && ncount < 16) {
                        next[ncount].s = a; next[ncount].e = excl[k].s; ncount++;
                    }
                    if (b > excl[k].e && ncount < 16) {
                        next[ncount].s = excl[k].e; next[ncount].e = b; ncount++;
                    }
                }
            }
            fcount = ncount;
            for (size_t j = 0; j < fcount; j++) {
                frags[j].s = next[j].s;
                frags[j].e = next[j].e;
            }
        }
        /* Emit surviving fragments rounded to MEMORY_RANGE_GRANULE.
         * Preserve the E820-derived type so pmm_init's Step 2 (which
         * only walks MEMORY_TYPE_RAM ranges) and any future non-RAM
         * consumer (ACPI reclaim, NVS) see the right surface; do NOT
         * force everything to MEMORY_TYPE_RAM. */
        for (size_t j = 0; j < fcount; j++) {
            uint64_t rs = round_up(frags[j].s, MEMORY_RANGE_GRANULE);
            uint64_t re = round_down(frags[j].e, MEMORY_RANGE_GRANULE);
            if (re <= rs) continue;
            if (out_count >= MEMORY_RANGE_MAX) return 0;
            out[out_count].phys_start = rs;
            out[out_count].phys_end   = re;
            out[out_count].type       = t;
            out_count++;
        }
    }
    /* Sort by phys_start ascending. n is small (≤64); insertion sort. */
    for (size_t i = 1; i < out_count; i++) {
        struct MEMORY_RANGE tmp = out[i];
        size_t j = i;
        while (j > 0 && out[j-1].phys_start > tmp.phys_start) {
            out[j] = out[j-1]; j--;
        }
        out[j] = tmp;
    }
    /* Merge adjacent/overlapping ranges — but only neighbours with
     * the same type. Different types mark a hardware boundary (e.g.
     * RAM next to MMIO/reserved) that must not be collapsed. */
    size_t w = 0;
    for (size_t i = 0; i < out_count; i++) {
        if (w == 0 || out[i].phys_start > out[w-1].phys_end ||
            out[i].type != out[w-1].type) {
            out[w++] = out[i];
        } else {
            if (out[i].phys_end > out[w-1].phys_end)
                out[w-1].phys_end = out[i].phys_end;
        }
    }
    return w;
}

/* Strong override of the weak default in kernel/memory/pmm_arch.c. */
uint64_t pmm_arch_zone_split(void)
{
    return 0x100000000ULL;   /* 4 GiB threshold */
}