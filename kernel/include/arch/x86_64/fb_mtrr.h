#ifndef _ARCH_X86_64_FB_MTRR_H
#define _ARCH_X86_64_FB_MTRR_H

/*
 * arch/x86_64/fb_mtrr.h -- MTRR state readout and range decoding.
 *
 * SCOPE (read this before quoting anything this module produces):
 *
 *   This module separates THREE different things and never mixes them:
 *
 *     1. the RAW values the guest read out of the MTRR/PAT MSRs
 *        (fb_mtrr_read_raw) -- a verbatim copy of the hardware state
 *        the guest can see;
 *     2. the GUEST-SIDE decode of the guest's OWN MTRR configuration
 *        (fb_mtrr_decode_range) -- what the guest's own MTRRs say
 *        about a physical range.  This is a well defined function of
 *        the raw values and is computed with the architectural rules;
 *     3. the EFFECTIVE memory type of an access -- what the processor
 *        really used.  This module does NOT compute it and cannot:
 *        under nested paging (AMD NPT / Intel EPT) the effective type
 *        is decided by the host's page tables and the host's PAT, and
 *        the guest cannot observe either.  Anything that claims
 *        otherwise is wrong.
 *
 *   The guest-side decode (2) is therefore a statement about the
 *   GUEST's MTRR configuration ONLY.  It is NOT evidence that the
 *   processor used that type for the framebuffer.
 */

#include <stdint.h>
#include <stdbool.h>

/* ── IA32 MSR numbers (64-bit view) ─────────────────────────────────
 *
 * There is NO "32-bit legacy alias" rule for these registers: the
 * same MSR number is used from 32-bit and 64-bit code.  The only one
 * of these registers with a separate 32-bit view is IA32_MTRR_CAP
 * (0x0FE in 64-bit mode, 0x1FE in 32-bit mode).  IA32_MTRR_DEF_TYPE
 * is 0x2FF in both modes -- 0x1FF is NOT an alias for it.
 */
#define FB_IA32_PAT               0x277u
#define FB_IA32_MTRR_CAP          0x0FEu
#define FB_IA32_MTRR_DEF_TYPE     0x2FFu
#define FB_IA32_MTRR_BASE(n)      (0x200u + 2u * (n))
#define FB_IA32_MTRR_MASK(n)      (0x201u + 2u * (n))
#define FB_IA32_MTRR_FIX64R_0     0x250u
#define FB_IA32_MTRR_FIX4R_0      0x260u

/* IA32_MTRR_DEF_TYPE bits */
#define FB_MTRR_DEF_TYPE_E        (1ULL << 11)   /* MTRRs enabled          */
#define FB_MTRR_DEF_TYPE_FE       (1ULL << 10)   /* fixed ranges enabled   */

/* IA32_MTRR_MASK bits */
#define FB_MTRR_MASK_V            (1ULL << 11)   /* range valid            */

/* MTRR memory-type encodings (Intel SDM Vol 3A Table 11-5; identical
 * to Linux <asm/mtrr.h>):
 *
 *     0 = UC   1 = WC   4 = WT   5 = WP   6 = WB
 *
 * NOTE: 1 is WC, NOT WB.  WB is 6. */
#define FB_MTRR_TYPE_UC           0x00u
#define FB_MTRR_TYPE_WC           0x01u
#define FB_MTRR_TYPE_WT           0x04u
#define FB_MTRR_TYPE_WP           0x05u
#define FB_MTRR_TYPE_WB           0x06u

#define FB_MTRR_MAX_RANGES        16u

/* ── Raw state, exactly as read from the MSRs ─────────────────────── */
struct fb_mtrr_raw {
    uint64_t pat;
    uint64_t cap;
    uint64_t def_type;
    uint64_t fixed64r0;
    uint64_t fixed4r0;

    uint64_t base[FB_MTRR_MAX_RANGES];
    uint64_t mask[FB_MTRR_MAX_RANGES];

    bool pat_ok;
    bool cap_ok;
    bool def_ok;
    bool fixed64r0_ok;
    bool fixed4r0_ok;
    bool range_ok[FB_MTRR_MAX_RANGES];

    unsigned range_count;          /* variable ranges CAP advertises  */
    unsigned max_phys_addr_bits;   /* CPUID.0x80000008[7:0]           */
    bool     mtrr_supported;       /* CPUID.1:EDX[12]                 */
};

/* MSR read callback: 0 on success, non-zero when the read failed
 * (e.g. the (virtual) CPU does not implement the MSR). */
typedef int (*fb_mtrr_msr_read_fn)(uint32_t msr, uint64_t *value, void *ctx);

/*
 * Read the raw MTRR/PAT state.  A failing read is recorded as a
 * failure (xxx_ok = false) and never as a zero value.
 */
void fb_mtrr_read_raw(fb_mtrr_msr_read_fn read_msr, void *ctx,
                      unsigned max_phys_addr_bits, bool mtrr_supported,
                      struct fb_mtrr_raw *out);

/* ── Decode result ────────────────────────────────────────────────── */
typedef enum {
    FB_MTRR_DECODE_UNKNOWN = 0,   /* cannot be decided (see reason)     */
    FB_MTRR_DECODE_UNIFORM,       /* one type covers the whole range    */
    FB_MTRR_DECODE_MIXED,         /* different types in different parts */
} fb_mtrr_decode_status_t;

struct fb_mtrr_decode {
    fb_mtrr_decode_status_t status;
    uint64_t type;                 /* valid only when status == UNIFORM */
    bool     from_default;         /* the type came from DEF_TYPE       */
    unsigned covering;             /* enabled variable ranges that hit  */
    uint8_t  covering_idx[FB_MTRR_MAX_RANGES];
    uint64_t lo;                   /* queried range bounds              */
    uint64_t hi;
    const char *reason;            /* why UNKNOWN, or a short note      */
};

/*
 * Decode the guest's OWN MTRR configuration for the physical range
 * [pa, pa+size).
 *
 * Architectural rules implemented here:
 *   * DEF_TYPE.E == 0  -> MTRRs are disabled and the UC type is
 *     applied to all physical memory (Intel SDM Vol 3A 11.11.2.1);
 *   * a variable range with MASK.V == 0 is disabled and is ignored;
 *   * the default type applies only where NO enabled variable range
 *     applies;
 *   * overlapping types combine per Intel SDM Vol 3A Table 11-6;
 *   * more than three overlapping ranges -> UNKNOWN (the SDM does not
 *     define the outcome);
 *   * a range whose variable part is not a contiguous run of bits, or
 *     whose base has bits above the physical address width, cannot be
 *     decoded -> UNKNOWN;
 *   * any MSR read failure -> UNKNOWN.
 */
void fb_mtrr_decode_range(const struct fb_mtrr_raw *raw, uint64_t pa, uint64_t size,
                          struct fb_mtrr_decode *out);

/* "UC" / "WC" / "WT" / "WP" / "WB" / "?" for the type encodings above. */
const char *fb_mtrr_type_name(uint64_t type);

/* True when the encoding is one of the five architectural types. */
bool fb_mtrr_type_valid(uint64_t type);

#endif /* _ARCH_X86_64_FB_MTRR_H */
