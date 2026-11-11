/*
 * kernel/arch/x86_64/memory/fb_mtrr.c
 *
 * MTRR state readout and guest-side range decoding.
 *
 * This file deliberately contains NO architecture header dependency so
 * that the exact same code can be compiled into the host regression
 * (hosttests/cases/test_fb_mtrr.c) and driven with controlled MSR
 * values.  See <arch/x86_64/fb_mtrr.h> for the scope contract.
 */

#include <arch/x86_64/fb_mtrr.h>

#include <stddef.h>
#include <string.h>

bool fb_mtrr_type_valid(uint64_t type)
{
    switch (type & 0xFFu) {
    case FB_MTRR_TYPE_UC:
    case FB_MTRR_TYPE_WC:
    case FB_MTRR_TYPE_WT:
    case FB_MTRR_TYPE_WP:
    case FB_MTRR_TYPE_WB:
        return true;
    default:
        return false;
    }
}

const char *fb_mtrr_type_name(uint64_t type)
{
    switch (type & 0xFFu) {
    case FB_MTRR_TYPE_UC: return "UC";
    case FB_MTRR_TYPE_WC: return "WC";
    case FB_MTRR_TYPE_WT: return "WT";
    case FB_MTRR_TYPE_WP: return "WP";
    case FB_MTRR_TYPE_WB: return "WB";
    default:              return "?";
    }
}

void fb_mtrr_read_raw(fb_mtrr_msr_read_fn read_msr, void *ctx,
                      unsigned max_phys_addr_bits, bool mtrr_supported,
                      struct fb_mtrr_raw *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    out->max_phys_addr_bits = max_phys_addr_bits;
    out->mtrr_supported = mtrr_supported;

    if (!read_msr) {
        return;
    }

    out->pat_ok = (read_msr(FB_IA32_PAT, &out->pat, ctx) == 0);
    out->cap_ok = (read_msr(FB_IA32_MTRR_CAP, &out->cap, ctx) == 0);
    out->def_ok = (read_msr(FB_IA32_MTRR_DEF_TYPE, &out->def_type, ctx) == 0);
    out->fixed64r0_ok = (read_msr(FB_IA32_MTRR_FIX64R_0, &out->fixed64r0, ctx) == 0);
    out->fixed4r0_ok = (read_msr(FB_IA32_MTRR_FIX4R_0, &out->fixed4r0, ctx) == 0);

    unsigned vcnt = out->cap_ok ? (unsigned)(out->cap & 0xFFu) : 0u;
    if (vcnt > FB_MTRR_MAX_RANGES) {
        vcnt = FB_MTRR_MAX_RANGES;
    }
    out->range_count = vcnt;

    for (unsigned i = 0; i < vcnt; i++) {
        uint64_t base = 0, mask = 0;
        int rb = read_msr(FB_IA32_MTRR_BASE(i), &base, ctx);
        int rm = read_msr(FB_IA32_MTRR_MASK(i), &mask, ctx);
        out->base[i] = base;
        out->mask[i] = mask;
        out->range_ok[i] = (rb == 0 && rm == 0);
    }
}

/* Intel SDM Vol 3A Table 11-6: combining two overlapping MTRR types.
 *
 *   same type            -> that type
 *   UC with anything     -> UC
 *   WC with WT/WP/WB     -> WC
 *   WT with WP/WB        -> WT
 *   WP with WB           -> WP
 *
 * (The table is symmetric and associative over the five legal types,
 * so combining pairwise over three ranges reproduces the SDM's
 * three-column entries.) */
static bool fb_mtrr_combine(uint64_t a, uint64_t b, uint64_t *out)
{
    if (!fb_mtrr_type_valid(a) || !fb_mtrr_type_valid(b)) {
        return false;
    }
    if (a == b) {
        *out = a;
        return true;
    }
    if (a == FB_MTRR_TYPE_UC || b == FB_MTRR_TYPE_UC) {
        *out = FB_MTRR_TYPE_UC;
        return true;
    }
    if (a == FB_MTRR_TYPE_WC || b == FB_MTRR_TYPE_WC) {
        *out = FB_MTRR_TYPE_WC;
        return true;
    }
    if (a == FB_MTRR_TYPE_WT || b == FB_MTRR_TYPE_WT) {
        *out = FB_MTRR_TYPE_WT;
        return true;
    }
    if (a == FB_MTRR_TYPE_WP || b == FB_MTRR_TYPE_WP) {
        *out = FB_MTRR_TYPE_WP;
        return true;
    }
    /* WB + WB was handled by the same-type case above. */
    return false;
}

/* The physical address mask for the MTRR register arithmetic: the
 * MTRR mask register only describes bits 51:12, and the bits above
 * the implemented physical address width are always zero. */
static uint64_t fb_mtrr_phys_mask(const struct fb_mtrr_raw *raw)
{
    unsigned bits = raw->max_phys_addr_bits;
    if (bits == 0u) {
        bits = 36u;          /* architectural minimum on x86-64 */
    }
    if (bits < 32u) {
        bits = 32u;
    }
    if (bits > 52u) {
        bits = 52u;         /* MTRR mask register tops out at bit 51 */
    }
    return (UINT64_C(1) << bits) - UINT64_C(1);
}

static void fb_mtrr_sort_u64(uint64_t *a, unsigned n)
{
    for (unsigned i = 1; i < n; i++) {
        uint64_t key = a[i];
        unsigned j = i;
        while (j > 0 && a[j - 1] > key) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = key;
    }
}

void fb_mtrr_decode_range(const struct fb_mtrr_raw *raw, uint64_t pa, uint64_t size,
                          struct fb_mtrr_decode *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->status = FB_MTRR_DECODE_UNKNOWN;
    out->reason = "not evaluated";

    if (!raw) {
        out->reason = "no raw state";
        return;
    }
    if (size == 0u) {
        out->reason = "empty range";
        return;
    }
    if (pa + size < pa) {
        out->reason = "range overflow";
        return;
    }
    out->lo = pa;
    out->hi = pa + size - 1u;

    if (!raw->mtrr_supported) {
        out->reason = "CPU does not implement MTRRs";
        return;
    }
    if (!raw->def_ok) {
        out->reason = "IA32_MTRR_DEF_TYPE unreadable";
        return;
    }

    /* MTRRs disabled: the SDM applies UC to all physical memory and
     * the default type field is ignored. */
    if (!(raw->def_type & FB_MTRR_DEF_TYPE_E)) {
        out->status = FB_MTRR_DECODE_UNIFORM;
        out->type = FB_MTRR_TYPE_UC;
        out->reason = "MTRRs disabled (DEF_TYPE.E=0): UC for all physical memory";
        return;
    }

    uint64_t def_type = raw->def_type & 0xFFu;
    if (!fb_mtrr_type_valid(def_type)) {
        out->reason = "invalid default type in DEF_TYPE";
        return;
    }
    if (!raw->cap_ok) {
        /* Without CAP we do not know how many variable ranges exist, so
         * we cannot claim that the default type applies everywhere. */
        out->reason = "IA32_MTRR_CAP unreadable";
        return;
    }

    uint64_t phys_mask = fb_mtrr_phys_mask(raw);

    /* ── Collect the enabled variable ranges that touch the query ── */
    struct {
        uint64_t lo;
        uint64_t hi;
        uint64_t type;
        uint8_t  idx;
    } rel[FB_MTRR_MAX_RANGES];
    unsigned nrel = 0;

    uint64_t cuts[2 * FB_MTRR_MAX_RANGES + 2];
    unsigned ncuts = 0;

    for (unsigned i = 0; i < raw->range_count; i++) {
        if (!raw->range_ok[i]) {
            out->reason = "variable MTRR range unreadable";
            return;
        }

        uint64_t base = raw->base[i];
        uint64_t mask = raw->mask[i];

        /* V == 0: the range is disabled and must be ignored. */
        if (!(mask & FB_MTRR_MASK_V)) {
            continue;
        }

        uint64_t type = base & 0xFFu;
        if (!fb_mtrr_type_valid(type)) {
            out->reason = "invalid type in variable MTRR range";
            return;
        }

        /* The low 12 bits of base and mask are reserved and must be
         * cleared before the address arithmetic. */
        uint64_t base_addr = base & ~UINT64_C(0xFFF);
        uint64_t mask_addr = mask & ~UINT64_C(0xFFF);

        /* Bits above the implemented physical address width are always
         * zero in a real address: a base with such a bit set can never
         * match, so the range covers nothing. */
        if (base_addr & ~phys_mask) {
            continue;
        }

        /* The set of covered addresses is
         *   { p : p & mask_addr == base_addr & mask_addr }
         * which is the interval [fixed, fixed + dontcare] when the
         * variable bits form one contiguous run (the architectural
         * requirement for MTRR masks). */
        uint64_t fixed = base_addr & mask_addr;
        uint64_t dontcare = (~mask_addr) & phys_mask;

        uint64_t var = dontcare >> 12;
        if ((var & (var + UINT64_C(1))) != 0u) {
            /* The variable address bits (above the 4 KiB page offset,
             * which always varies) must form one contiguous run. */
            out->reason = "variable MTRR range is not a contiguous run of bits";
            return;
        }

        uint64_t lo = fixed;
        uint64_t hi = fixed + dontcare;
        if (hi < lo) {
            out->reason = "variable MTRR range overflow";
            return;
        }

        if (hi < pa || lo > out->hi) {
            continue;               /* does not touch the query */
        }

        rel[nrel].lo = lo;
        rel[nrel].hi = hi;
        rel[nrel].type = type;
        rel[nrel].idx = (uint8_t)i;
        nrel++;

        if (nrel > 3u) {
            /* The SDM defines the outcome for up to three overlapping
             * ranges (Table 11-6); for more the result is undefined. */
            out->reason = "more than three variable MTRR ranges overlap "
                          "(the SDM does not define the result)";
            return;
        }

        cuts[ncuts++] = lo;
        cuts[ncuts++] = hi + 1u;
    }

    for (unsigned i = 0; i < nrel; i++) {
        out->covering_idx[i] = rel[i].idx;
    }
    out->covering = nrel;

    if (nrel == 0u) {
        /* No variable range applies: the default type is the answer. */
        out->status = FB_MTRR_DECODE_UNIFORM;
        out->type = def_type;
        out->from_default = true;
        out->reason = "no enabled variable MTRR range covers the range";
        return;
    }

    /* ── Split the query range at every range boundary and evaluate
     *    each piece, so a range that covers only part of the query is
     *    visible as a mixed result instead of being ignored. ── */
    cuts[ncuts++] = pa;
    cuts[ncuts++] = out->hi + 1u;
    fb_mtrr_sort_u64(cuts, ncuts);

    bool have_type = false;
    uint64_t type = 0;

    for (unsigned c = 0; c + 1u < ncuts; c++) {
        uint64_t s = cuts[c];
        uint64_t e = cuts[c + 1u];
        if (e <= s) {
            continue;
        }
        uint64_t piece_hi = e - 1u;

        uint64_t piece_type = def_type;
        unsigned hits = 0;
        for (unsigned i = 0; i < nrel; i++) {
            if (rel[i].lo <= s && piece_hi <= rel[i].hi) {
                if (hits == 0u) {
                    piece_type = rel[i].type;
                } else {
                    uint64_t combined = 0;
                    if (!fb_mtrr_combine(piece_type, rel[i].type, &combined)) {
                        out->reason = "overlapping MTRR types cannot be combined";
                        return;
                    }
                    piece_type = combined;
                }
                hits++;
            }
        }

        if (!have_type) {
            type = piece_type;
            have_type = true;
        } else if (type != piece_type) {
            out->status = FB_MTRR_DECODE_MIXED;
            out->reason = "different MTRR types cover different parts of the range";
            return;
        }
    }

    if (!have_type) {
        out->reason = "empty evaluation";
        return;
    }

    out->status = FB_MTRR_DECODE_UNIFORM;
    out->type = type;
    out->from_default = false;
    out->reason = "one type covers the whole range";
}
