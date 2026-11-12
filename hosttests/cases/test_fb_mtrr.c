/*
 * hosttests/cases/test_fb_mtrr.c
 *
 * MTRR raw readout and guest-side range decoding (framebuffer cache
 * policy work, round 3).
 *
 * The decoder under test is the SAME production object the kernel
 * links (kernel/arch/x86_64/memory/fb_mtrr.c); the test only replaces
 * the MSR read primitive with a controlled one.  The test therefore
 * cannot drift away from what the kernel prints.
 *
 * The first five cases are the negative cases the round-3 review
 * listed as "5/5 wrong" against the previous derivation:
 *
 *   1. MTRRs enabled, default WB, one DISABLED variable range
 *      -> the disabled range must be ignored: WB (not UC);
 *   2. MTRRs disabled (DEF_TYPE.E == 0), default field WB
 *      -> UC (the SDM applies UC to all physical memory);
 *   3. default UC plus one ENABLED variable range WB that covers the
 *      range -> WB (the variable range overrides the default);
 *   4. default WB, no variable ranges -> WB (and the type NAME of
 *      encoding 6 is "WB");
 *   5. default WC, no variable ranges -> WC (encoding 1 is WC; the
 *      previous code printed "WB" for it).
 *
 * The remaining cases cover the other findings: the 32-bit truncation
 * of the mask arithmetic, the "read failed -> unknown" rule, the
 * uniformity check over the queried size, and the real machine state
 * the review decoded independently.
 */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <test_framework.h>

#include <arch/x86_64/fb_mtrr.h>

static int g_failed = 0;

#define CHECK_TRUE(cond) do { \
    if (!(cond)) { \
        g_failed++; \
        printf("  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } else { \
        __test_stats.passed++; \
        __test_stats.total++; \
    } \
} while (0)

#define CHECK_EQ(exp, act) do { \
    long _e = (long)(exp); \
    long _a = (long)(act); \
    if (_e != _a) { \
        g_failed++; \
        printf("  [FAIL] %s:%d: %s (expected %ld, got %ld)\n", __FILE__, __LINE__, #act, _e, _a); \
    } else { \
        __test_stats.passed++; \
        __test_stats.total++; \
    } \
} while (0)

#define CHECK_STR(exp, act) do { \
    const char *_e = (exp); \
    const char *_a = (act); \
    if ((!_e && _a) || (_e && !_a) || (_e && _a && strcmp(_e, _a) != 0)) { \
        g_failed++; \
        printf("  [FAIL] %s:%d: %s (expected \"%s\", got \"%s\")\n", \
               __FILE__, __LINE__, #act, _e ? _e : "(null)", _a ? _a : "(null)"); \
    } else { \
        __test_stats.passed++; \
        __test_stats.total++; \
    } \
} while (0)

/* ── Controlled MSR source ────────────────────────────────────────── */

#define MOCK_MSR_SLOTS 0x300

struct mock_msr {
    uint64_t regs[MOCK_MSR_SLOTS];
    bool     fail[MOCK_MSR_SLOTS];
    unsigned max_phys;
    bool     supported;
};

static int mock_read_msr(uint32_t msr, uint64_t *value, void *ctx)
{
    struct mock_msr *m = (struct mock_msr *)ctx;
    if (msr >= MOCK_MSR_SLOTS || m->fail[msr]) {
        return -1;
    }
    *value = m->regs[msr];
    return 0;
}

static void mock_init(struct mock_msr *m)
{
    memset(m, 0, sizeof(*m));
    m->max_phys = 40;              /* the host CPU reports 40 bits */
    m->supported = true;
    m->regs[FB_IA32_MTRR_CAP] = 0x500u;   /* FIX + WC, VCNT = 0 */
}

/* Set the variable-range count the CAP register advertises. */
static void mock_set_vcnt(struct mock_msr *m, unsigned vcnt)
{
    m->regs[FB_IA32_MTRR_CAP] = (m->regs[FB_IA32_MTRR_CAP] & ~0xFFu) | (uint64_t)vcnt;
}

/*
 * An MTRR mask that leaves the address bits below `var_bits` variable
 * (the variable part always starts at bit 12) and fixes everything
 * above them.  Building it numerically keeps the test independent of
 * hand-computed hex constants.
 */
static uint64_t mock_mask(const struct mock_msr *m, unsigned var_bits)
{
    uint64_t phys = (UINT64_C(1) << m->max_phys) - UINT64_C(1);
    uint64_t addr_mask = phys & ~((UINT64_C(1) << var_bits) - UINT64_C(1));
    return addr_mask | FB_MTRR_MASK_V;
}

/* The base of a range that covers [pa, pa+size) with the given type. */
static uint64_t mock_base(const struct mock_msr *m, uint64_t pa, unsigned var_bits,
                           uint64_t type)
{
    uint64_t phys = (UINT64_C(1) << m->max_phys) - UINT64_C(1);
    uint64_t addr_mask = phys & ~((UINT64_C(1) << var_bits) - UINT64_C(1));
    return (pa & addr_mask) | (type & 0xFFu);
}

static void mock_decode(const struct mock_msr *m, uint64_t pa, uint64_t size,
                         struct fb_mtrr_decode *out)
{
    struct fb_mtrr_raw raw;
    fb_mtrr_read_raw(mock_read_msr, (void *)m, m->max_phys, m->supported, &raw);
    fb_mtrr_decode_range(&raw, pa, size, out);
}

/* The framebuffer BAR0 of this class of machine. */
#define MOCK_BAR0_PA   UINT64_C(0x80000000)
#define MOCK_BAR0_SIZE UINT64_C(0x1000000)     /* 16 MiB */

/* ── The five review negative cases ───────────────────────────────── */

static void test_case_disabled_variable_range_is_ignored(void)
{
    printf("\n--- test_case_disabled_variable_range_is_ignored ---\n");
    struct mock_msr m;
    mock_init(&m);

    /* MTRRs enabled, default type WB. */
    m.regs[FB_IA32_MTRR_DEF_TYPE] = FB_MTRR_DEF_TYPE_E | FB_MTRR_TYPE_WB;
    mock_set_vcnt(&m, 1);

    /* One variable range with type UC but V == 0 -> DISABLED. */
    m.regs[FB_IA32_MTRR_BASE(0)] = FB_MTRR_TYPE_UC;
    m.regs[FB_IA32_MTRR_MASK(0)] = 0;

    struct fb_mtrr_decode dec;
    mock_decode(&m, MOCK_BAR0_PA, MOCK_BAR0_SIZE, &dec);

    CHECK_EQ(FB_MTRR_DECODE_UNIFORM, dec.status);
    CHECK_EQ(FB_MTRR_TYPE_WB, dec.type);
    CHECK_TRUE(dec.from_default);
    CHECK_EQ(0, dec.covering);
}

static void test_case_mtrrs_disabled_means_uc(void)
{
    printf("\n--- test_case_mtrrs_disabled_means_uc ---\n");
    struct mock_msr m;
    mock_init(&m);

    /* E == 0: the default type field is ignored entirely. */
    m.regs[FB_IA32_MTRR_DEF_TYPE] = FB_MTRR_TYPE_WB;

    struct fb_mtrr_decode dec;
    mock_decode(&m, MOCK_BAR0_PA, MOCK_BAR0_SIZE, &dec);

    CHECK_EQ(FB_MTRR_DECODE_UNIFORM, dec.status);
    CHECK_EQ(FB_MTRR_TYPE_UC, dec.type);
}

static void test_case_variable_range_overrides_default(void)
{
    printf("\n--- test_case_variable_range_overrides_default ---\n");
    struct mock_msr m;
    mock_init(&m);

    /* Default type UC ... */
    m.regs[FB_IA32_MTRR_DEF_TYPE] = FB_MTRR_DEF_TYPE_E | FB_MTRR_TYPE_UC;
    mock_set_vcnt(&m, 1);

    /* ... and one ENABLED variable range of type WB covering BAR0. */
    unsigned var_bits = 24;          /* 16 MiB variable part */
    m.regs[FB_IA32_MTRR_BASE(0)] = mock_base(&m, MOCK_BAR0_PA, var_bits,
                                              FB_MTRR_TYPE_WB);
    m.regs[FB_IA32_MTRR_MASK(0)] = mock_mask(&m, var_bits);

    struct fb_mtrr_decode dec;
    mock_decode(&m, MOCK_BAR0_PA, MOCK_BAR0_SIZE, &dec);

    CHECK_EQ(FB_MTRR_DECODE_UNIFORM, dec.status);
    CHECK_EQ(FB_MTRR_TYPE_WB, dec.type);
    CHECK_TRUE(!dec.from_default);
    CHECK_EQ(1, dec.covering);
}

static void test_case_default_wb_without_variables(void)
{
    printf("\n--- test_case_default_wb_without_variables ---\n");
    struct mock_msr m;
    mock_init(&m);

    m.regs[FB_IA32_MTRR_DEF_TYPE] = FB_MTRR_DEF_TYPE_E | FB_MTRR_TYPE_WB;

    struct fb_mtrr_decode dec;
    mock_decode(&m, MOCK_BAR0_PA, MOCK_BAR0_SIZE, &dec);

    CHECK_EQ(FB_MTRR_DECODE_UNIFORM, dec.status);
    CHECK_EQ(FB_MTRR_TYPE_WB, dec.type);
    CHECK_TRUE(dec.from_default);
    CHECK_STR("WB", fb_mtrr_type_name(dec.type));
}

static void test_case_default_wc_without_variables(void)
{
    printf("\n--- test_case_default_wc_without_variables ---\n");
    struct mock_msr m;
    mock_init(&m);

    m.regs[FB_IA32_MTRR_DEF_TYPE] = FB_MTRR_DEF_TYPE_E | FB_MTRR_TYPE_WC;

    struct fb_mtrr_decode dec;
    mock_decode(&m, MOCK_BAR0_PA, MOCK_BAR0_SIZE, &dec);

    CHECK_EQ(FB_MTRR_DECODE_UNIFORM, dec.status);
    CHECK_EQ(FB_MTRR_TYPE_WC, dec.type);
    CHECK_TRUE(dec.from_default);
    CHECK_STR("WC", fb_mtrr_type_name(dec.type));
}

/* ── The type-encoding table ──────────────────────────────────────── */

static void test_type_encoding_table(void)
{
    printf("\n--- test_type_encoding_table ---\n");
    /* Intel SDM Vol 3A Table 11-5 / Linux <asm/mtrr.h>:
     *   0 = UC, 1 = WC, 4 = WT, 5 = WP, 6 = WB. */
    CHECK_STR("UC", fb_mtrr_type_name(0x00));
    CHECK_STR("WC", fb_mtrr_type_name(0x01));
    CHECK_STR("WT", fb_mtrr_type_name(0x04));
    CHECK_STR("WP", fb_mtrr_type_name(0x05));
    CHECK_STR("WB", fb_mtrr_type_name(0x06));
    CHECK_STR("?",  fb_mtrr_type_name(0x07));
    CHECK_STR("?",  fb_mtrr_type_name(0xFF));

    CHECK_TRUE(fb_mtrr_type_valid(0x00));
    CHECK_TRUE(fb_mtrr_type_valid(0x01));
    CHECK_TRUE(fb_mtrr_type_valid(0x04));
    CHECK_TRUE(fb_mtrr_type_valid(0x05));
    CHECK_TRUE(fb_mtrr_type_valid(0x06));
    CHECK_TRUE(!fb_mtrr_type_valid(0x02));
    CHECK_TRUE(!fb_mtrr_type_valid(0x07));
}

/* ── The 32-bit truncation of the mask arithmetic ─────────────────── */

static void test_high_address_bits_are_not_truncated(void)
{
    printf("\n--- test_high_address_bits_are_not_truncated ---\n");
    struct mock_msr m;
    mock_init(&m);

    m.regs[FB_IA32_MTRR_DEF_TYPE] = FB_MTRR_DEF_TYPE_E | FB_MTRR_TYPE_UC;
    mock_set_vcnt(&m, 1);

    /* A range far above the 32-bit boundary (bit 39). */
    uint64_t high_pa = UINT64_C(0x8000000000);
    unsigned var_bits = 12;
    m.regs[FB_IA32_MTRR_BASE(0)] = mock_base(&m, high_pa, var_bits, FB_MTRR_TYPE_WB);
    m.regs[FB_IA32_MTRR_MASK(0)] = mock_mask(&m, var_bits);

    struct fb_mtrr_decode dec;

    /* Inside that range: WB. */
    mock_decode(&m, high_pa, 0x1000, &dec);
    CHECK_EQ(FB_MTRR_DECODE_UNIFORM, dec.status);
    CHECK_EQ(FB_MTRR_TYPE_WB, dec.type);
    CHECK_EQ(1, dec.covering);

    /* Outside that range: the default (UC) applies.  The previous
     * implementation truncated the mask to 32 bits, which made EVERY
     * range look like it covered the whole address space. */
    mock_decode(&m, MOCK_BAR0_PA, MOCK_BAR0_SIZE, &dec);
    CHECK_EQ(FB_MTRR_DECODE_UNIFORM, dec.status);
    CHECK_EQ(FB_MTRR_TYPE_UC, dec.type);
    CHECK_EQ(0, dec.covering);
}

/* ── A failed read is unknown, never a guess ──────────────────────── */

static void test_failed_reads_are_unknown(void)
{
    printf("\n--- test_failed_reads_are_unknown ---\n");
    struct mock_msr m;
    struct fb_mtrr_decode dec;

    /* DEF_TYPE unreadable. */
    mock_init(&m);
    m.regs[FB_IA32_MTRR_DEF_TYPE] = FB_MTRR_DEF_TYPE_E | FB_MTRR_TYPE_WB;
    m.fail[FB_IA32_MTRR_DEF_TYPE] = true;
    mock_decode(&m, MOCK_BAR0_PA, MOCK_BAR0_SIZE, &dec);
    CHECK_EQ(FB_MTRR_DECODE_UNKNOWN, dec.status);

    /* CAP unreadable: we do not know how many ranges exist. */
    mock_init(&m);
    m.regs[FB_IA32_MTRR_DEF_TYPE] = FB_MTRR_DEF_TYPE_E | FB_MTRR_TYPE_WB;
    m.fail[FB_IA32_MTRR_CAP] = true;
    mock_decode(&m, MOCK_BAR0_PA, MOCK_BAR0_SIZE, &dec);
    CHECK_EQ(FB_MTRR_DECODE_UNKNOWN, dec.status);

    /* One variable range unreadable. */
    mock_init(&m);
    m.regs[FB_IA32_MTRR_DEF_TYPE] = FB_MTRR_DEF_TYPE_E | FB_MTRR_TYPE_WB;
    mock_set_vcnt(&m, 1);
    m.fail[FB_IA32_MTRR_MASK(0)] = true;
    mock_decode(&m, MOCK_BAR0_PA, MOCK_BAR0_SIZE, &dec);
    CHECK_EQ(FB_MTRR_DECODE_UNKNOWN, dec.status);

    /* The CPU does not implement MTRRs at all. */
    mock_init(&m);
    m.supported = false;
    m.regs[FB_IA32_MTRR_DEF_TYPE] = FB_MTRR_DEF_TYPE_E | FB_MTRR_TYPE_WB;
    mock_decode(&m, MOCK_BAR0_PA, MOCK_BAR0_SIZE, &dec);
    CHECK_EQ(FB_MTRR_DECODE_UNKNOWN, dec.status);

    /* An empty range cannot be decoded. */
    mock_init(&m);
    m.regs[FB_IA32_MTRR_DEF_TYPE] = FB_MTRR_DEF_TYPE_E | FB_MTRR_TYPE_WB;
    mock_decode(&m, MOCK_BAR0_PA, 0, &dec);
    CHECK_EQ(FB_MTRR_DECODE_UNKNOWN, dec.status);
}

/* ── The queried size must be covered uniformly ───────────────────── */

static void test_range_must_be_uniform(void)
{
    printf("\n--- test_range_must_be_uniform ---\n");
    struct mock_msr m;
    mock_init(&m);

    /* Default WB ... */
    m.regs[FB_IA32_MTRR_DEF_TYPE] = FB_MTRR_DEF_TYPE_E | FB_MTRR_TYPE_WB;
    mock_set_vcnt(&m, 1);

    /* ... and a UC range that covers only the first 4 MiB of BAR0. */
    unsigned var_bits = 22;                  /* 4 MiB variable part */
    m.regs[FB_IA32_MTRR_BASE(0)] = mock_base(&m, MOCK_BAR0_PA, var_bits,
                                              FB_MTRR_TYPE_UC);
    m.regs[FB_IA32_MTRR_MASK(0)] = mock_mask(&m, var_bits);

    struct fb_mtrr_decode dec;

    /* The whole BAR0 is NOT uniform: 4 MiB are UC, the rest is WB. */
    mock_decode(&m, MOCK_BAR0_PA, MOCK_BAR0_SIZE, &dec);
    CHECK_EQ(FB_MTRR_DECODE_MIXED, dec.status);

    /* The first 4 MiB alone are uniform UC. */
    mock_decode(&m, MOCK_BAR0_PA, 4u * 1024u * 1024u, &dec);
    CHECK_EQ(FB_MTRR_DECODE_UNIFORM, dec.status);
    CHECK_EQ(FB_MTRR_TYPE_UC, dec.type);

    /* The last 4 MiB alone are uniform WB (the default). */
    mock_decode(&m, MOCK_BAR0_PA + 12u * 1024u * 1024u, 4u * 1024u * 1024u, &dec);
    CHECK_EQ(FB_MTRR_DECODE_UNIFORM, dec.status);
    CHECK_EQ(FB_MTRR_TYPE_WB, dec.type);
}

/* ── The real machine state, decoded independently ────────────────── */

static void test_real_machine_state(void)
{
    printf("\n--- test_real_machine_state ---\n");
    struct mock_msr m;
    mock_init(&m);

    /* The values the round-3 review read out of the running guest
     * (KVM and TCG agreed):
     *
     *   PAT          = 0x0007040600070406
     *   MTRR_CAP     = 0x508      (VCNT = 8, FIX, WC)
     *   MTRR_DEF_TYPE= 0xc06      (E = 1, FE = 1, default type WB)
     *   range0: base 0x80000000    mask 0xff80000800   V = 1  -> UC
     *   range1: base 0x8000000000  mask 0xc000000800    V = 1  -> ?
     *   range2..7: base 0          mask 0              V = 0  -> ignore
     */
    m.regs[FB_IA32_PAT] = UINT64_C(0x0007040600070406);
    m.regs[FB_IA32_MTRR_CAP] = UINT64_C(0x508);
    m.regs[FB_IA32_MTRR_DEF_TYPE] = UINT64_C(0xc06);
    mock_set_vcnt(&m, 8);

    m.regs[FB_IA32_MTRR_BASE(0)] = UINT64_C(0x80000000);
    m.regs[FB_IA32_MTRR_MASK(0)] = UINT64_C(0xff80000800);

    m.regs[FB_IA32_MTRR_BASE(1)] = UINT64_C(0x8000000000);
    m.regs[FB_IA32_MTRR_MASK(1)] = UINT64_C(0xc000000800);

    for (unsigned i = 2; i < 8; i++) {
        m.regs[FB_IA32_MTRR_BASE(i)] = 0;
        m.regs[FB_IA32_MTRR_MASK(i)] = 0;      /* V = 0 -> disabled */
    }

    struct fb_mtrr_decode dec;
    mock_decode(&m, MOCK_BAR0_PA, MOCK_BAR0_SIZE, &dec);

    /* range0 covers [0x80000000, 0xffffffff] with UC, so the guest's
     * own MTRR configuration says UC for the whole BAR0.  The disabled
     * ranges 2..7 must not contribute. */
    CHECK_EQ(FB_MTRR_DECODE_UNIFORM, dec.status);
    CHECK_EQ(FB_MTRR_TYPE_UC, dec.type);
    CHECK_EQ(1, dec.covering);
    CHECK_EQ(0, dec.covering_idx[0]);
}

/* ── The raw readout never invents a value ────────────────────────── */

static void test_three_overlapping_ranges_are_decoded(void)
{
    printf("\n--- test_three_overlapping_ranges_are_decoded ---\n");
    struct mock_msr m;
    mock_init(&m);

    m.regs[FB_IA32_MTRR_DEF_TYPE] = FB_MTRR_DEF_TYPE_E | FB_MTRR_TYPE_UC;
    mock_set_vcnt(&m, 3);

    /* Three ENABLED ranges over BAR0: the SDM defines the combined
     * type for up to three overlapping variable ranges (Table 11-6),
     * so the decoder must still produce a real answer here. */
    unsigned var_bits = 24;
    for (unsigned i = 0; i < 3; i++) {
        m.regs[FB_IA32_MTRR_BASE(i)] = mock_base(&m, MOCK_BAR0_PA, var_bits,
                                                  FB_MTRR_TYPE_WB);
        m.regs[FB_IA32_MTRR_MASK(i)] = mock_mask(&m, var_bits);
    }

    struct fb_mtrr_decode dec;
    mock_decode(&m, MOCK_BAR0_PA, MOCK_BAR0_SIZE, &dec);

    CHECK_EQ(FB_MTRR_DECODE_UNIFORM, dec.status);
    CHECK_EQ(FB_MTRR_TYPE_WB, dec.type);
    CHECK_EQ(3, dec.covering);
}

static void test_more_than_three_overlapping_ranges_is_unknown(void)
{
    printf("\n--- test_more_than_three_overlapping_ranges_is_unknown ---\n");
    struct mock_msr m;
    mock_init(&m);

    m.regs[FB_IA32_MTRR_DEF_TYPE] = FB_MTRR_DEF_TYPE_E | FB_MTRR_TYPE_WB;
    mock_set_vcnt(&m, 4);

    /* A fourth overlapping range is outside the SDM table and must be
     * rejected as UNKNOWN instead of overrunning the decoder's
     * collected-range slots. */
    unsigned var_bits = 24;
    for (unsigned i = 0; i < 4; i++) {
        m.regs[FB_IA32_MTRR_BASE(i)] = mock_base(&m, MOCK_BAR0_PA, var_bits,
                                                  FB_MTRR_TYPE_WB);
        m.regs[FB_IA32_MTRR_MASK(i)] = mock_mask(&m, var_bits);
    }

    struct fb_mtrr_decode dec;
    mock_decode(&m, MOCK_BAR0_PA, MOCK_BAR0_SIZE, &dec);

    CHECK_EQ(FB_MTRR_DECODE_UNKNOWN, dec.status);
    CHECK_TRUE(dec.reason != NULL && strstr(dec.reason, "more than three") != NULL);
}

static void test_raw_readout_records_failures(void)
{
    printf("\n--- test_raw_readout_records_failures ---\n");
    struct mock_msr m;
    mock_init(&m);
    m.regs[FB_IA32_PAT] = UINT64_C(0x0007040600070406);
    m.regs[FB_IA32_MTRR_CAP] = UINT64_C(0x508);
    m.regs[FB_IA32_MTRR_DEF_TYPE] = UINT64_C(0xc06);
    mock_set_vcnt(&m, 8);
    m.fail[FB_IA32_MTRR_FIX64R_0] = true;
    m.fail[FB_IA32_MTRR_BASE(3)] = true;

    struct fb_mtrr_raw raw;
    fb_mtrr_read_raw(mock_read_msr, (void *)&m, m.max_phys, m.supported, &raw);

    CHECK_TRUE(raw.pat_ok);
    CHECK_TRUE(raw.cap_ok);
    CHECK_TRUE(raw.def_ok);
    CHECK_TRUE(!raw.fixed64r0_ok);
    CHECK_TRUE(raw.fixed4r0_ok);
    CHECK_EQ(8, raw.range_count);
    CHECK_TRUE(raw.range_ok[0]);
    CHECK_TRUE(!raw.range_ok[3]);
    CHECK_EQ(UINT64_C(0x0007040600070406), raw.pat);
    CHECK_EQ(UINT64_C(0x508), raw.cap);
    CHECK_EQ(UINT64_C(0xc06), raw.def_type);
    CHECK_EQ(40, raw.max_phys_addr_bits);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_case_disabled_variable_range_is_ignored),
    TEST_ENTRY(test_case_mtrrs_disabled_means_uc),
    TEST_ENTRY(test_case_variable_range_overrides_default),
    TEST_ENTRY(test_case_default_wb_without_variables),
    TEST_ENTRY(test_case_default_wc_without_variables),
    TEST_ENTRY(test_type_encoding_table),
    TEST_ENTRY(test_high_address_bits_are_not_truncated),
    TEST_ENTRY(test_failed_reads_are_unknown),
    TEST_ENTRY(test_range_must_be_uniform),
    TEST_ENTRY(test_three_overlapping_ranges_are_decoded),
    TEST_ENTRY(test_more_than_three_overlapping_ranges_is_unknown),
    TEST_ENTRY(test_real_machine_state),
    TEST_ENTRY(test_raw_readout_records_failures),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    if (g_failed > 0) {
        printf("test_fb_mtrr: %d failures\n", g_failed);
        return 1;
    }
    return 0;
}
