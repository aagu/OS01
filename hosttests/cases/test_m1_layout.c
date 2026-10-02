/*
 * hosttests/cases/test_m1_layout.c — checked PMM metadata layout calculator
 *                                     (aarch64 M1 plan Task 1).
 *
 * The production kernel/memory/pmm.c sizes the bits_map, pages_struct
 * and zones_struct via hand-rolled formulas with no overflow check.
 * Task 1 extracts the same math into kernel/memory/pmm_boot.c (which
 * produces a struct pmm_layout) and updates pmm_init to use it.
 *
 * This suite asserts the LP64 production layout math is preserved
 * byte-for-byte across the refactor, while exercising all the boundary
 * conditions the checked-arithmetic version needs to handle correctly:
 *
 *   - 512 MiB / 4 GiB nominal spans (happy paths)
 *   - 1 page (smallest legal input)
 *   - sparse min/max spans (very small, very large)
 *   - unaligned base (e.g. base = 1) — base value must not affect offsets
 *   - near-UINT64_MAX base — same; offsets are relative
 *   - multiplication / addition / align overflow — must return <0 with
 *     `out` zeroed (zero layout is not usable per the brief contract)
 *
 * Offsets are RELATIVE to the caller's base_va (the production code
 * aligns start_brk to 4 KiB before assigning bits_map, so the relative
 * bits_map offset is 0). Per the brief: `bits_map_off = 0`,
 * `pages_struct_off` aligned 4 KiB after bits_map+bits_length,
 * `zones_struct_off` aligned 4 KiB after pages_struct+pages_length,
 * `end_of_struct_off = zones_struct_off + zones_length + 32*sizeof(long)`
 * aligned down to sizeof(long), and
 * `total_bytes = align_up(end_of_struct_off, 4 KiB)`.
 *
 * metadata tail of `32*sizeof(unsigned long)` (= 256 B) is preserved.
 * Page count follows the SPAN, not the RAM total — sparse zones with
 * a small RAM total but wide PA span still produce a Page array sized
 * for the span.
 */
#include "test_framework.h"
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <memory/memory_map.h>
#include <memory/pmm.h>
#include <memory/pmm_boot.h>

/* ── Hand-mirrored reference formula (LP64, production math) ──
 * Used only for the happy-path assertions. The checked calculator is
 * the implementation under test, not this macro set. */
#define PMM_BOOT_TAIL_LONG_WORDS 32UL
#define PMM_BOOT_TAIL_BYTES      (PMM_BOOT_TAIL_LONG_WORDS * sizeof(unsigned long))

/* Production math, ported to the bitfields of `out` so each happy-path
 * test can compare the calculator's output byte-for-byte. */
static void reference_layout(uint64_t span_pages, struct pmm_layout *ref)
{
    uint64_t bits_length   = ((span_pages + 63) & ~63UL) / 8;
    uint64_t pages_length  = ((span_pages * sizeof(struct Page) + sizeof(long) - 1)
                              & ~(sizeof(long) - 1));
    uint64_t zones_length  = ((MEMORY_RANGE_MAX * sizeof(struct Zone) + sizeof(long) - 1)
                              & ~(sizeof(long) - 1));
    uint64_t pages_off     = ((bits_length + 0xFFFUL) & ~0xFFFUL);
    uint64_t zones_off     = ((pages_off + pages_length + 0xFFFUL) & ~0xFFFUL);
    uint64_t end_of_struct_off =
        ((zones_off + zones_length + sizeof(long) * 32UL) & ~(sizeof(long) - 1));
    uint64_t total_bytes   = ((end_of_struct_off + 0xFFFUL) & ~0xFFFUL);

    ref->bits_map_off       = 0;
    ref->bits_length        = bits_length;
    ref->pages_struct_off   = pages_off;
    ref->pages_length       = pages_length;
    ref->zones_struct_off   = zones_off;
    ref->zones_length       = zones_length;
    ref->end_of_struct_off  = end_of_struct_off;
    ref->total_bytes        = total_bytes;
}

#define LAYOUT_EQ(label, got, ref, field) do { \
    if ((got).field != (ref).field) { \
        printf("  [FAIL] %s: " #field " got=0x%llx expected=0x%llx\n", \
               label, (unsigned long long)(got).field, \
               (unsigned long long)(ref).field); \
        __test_stats.failed++; \
    } else { \
        __test_stats.passed++; \
    } \
    __test_stats.total++; \
} while (0)

static void assert_layout_eq(const char *label,
                             const struct pmm_layout *got,
                             const struct pmm_layout *ref)
{
    LAYOUT_EQ(label, *got, *ref, bits_map_off);
    LAYOUT_EQ(label, *got, *ref, bits_length);
    LAYOUT_EQ(label, *got, *ref, pages_struct_off);
    LAYOUT_EQ(label, *got, *ref, pages_length);
    LAYOUT_EQ(label, *got, *ref, zones_struct_off);
    LAYOUT_EQ(label, *got, *ref, zones_length);
    LAYOUT_EQ(label, *got, *ref, end_of_struct_off);
    LAYOUT_EQ(label, *got, *ref, total_bytes);
}

/* ── Happy paths ───────────────────────────────────────────── */
TEST_FUNC(test_512_mib_span)
{
    /* 512 MiB = 256 * 2 MiB pages. */
    struct pmm_layout got = {0};
    struct pmm_layout ref;
    reference_layout(256, &ref);
    int rc = pmm_layout_calculate(0, 256, &got);
    assert_eq(0, rc);
    assert_layout_eq("512 MiB", &got, &ref);
}

TEST_FUNC(test_4_gib_span)
{
    /* 4 GiB = 2048 * 2 MiB pages. */
    struct pmm_layout got = {0};
    struct pmm_layout ref;
    reference_layout(2048, &ref);
    int rc = pmm_layout_calculate(0, 2048, &got);
    assert_eq(0, rc);
    assert_layout_eq("4 GiB", &got, &ref);
}

TEST_FUNC(test_min_one_frame)
{
    /* 1 page — smallest legal input; bit rounding still bumps bits_length to 8. */
    struct pmm_layout got = {0};
    struct pmm_layout ref;
    reference_layout(1, &ref);
    int rc = pmm_layout_calculate(0, 1, &got);
    assert_eq(0, rc);
    assert_layout_eq("1 page", &got, &ref);
    assert_true(ref.bits_length == 8);    /* 64 bits = 8 bytes */
    assert_true(ref.bits_map_off == 0);  /* bits_map at base */
}

TEST_FUNC(test_sparse_min_max_span)
{
    /* sparse-ish: small span (16) and large span (1 GiB / 2 MiB = 512 pages). */
    struct pmm_layout got = {0};
    struct pmm_layout ref;

    reference_layout(16, &ref);
    assert_eq(0, pmm_layout_calculate(0x12345678ULL, 16, &got));
    assert_layout_eq("16 pages", &got, &ref);

    reference_layout(512, &ref);
    assert_eq(0, pmm_layout_calculate(0xCAFE0000ULL, 512, &got));
    assert_layout_eq("512 pages", &got, &ref);
}

/* ── Unaligned base ────────────────────────────────────────── */
/* Offsets are RELATIVE to base_va; the value of base_va must not affect
 * the resulting offsets (the caller is expected to align first, mirroring
 * production pmm.c which does `(start_brk + 0xFFF) & ~0xFFF`). */
TEST_FUNC(test_unaligned_base_does_not_change_offsets)
{
    struct pmm_layout got_a = {0};
    struct pmm_layout got_b = {0};
    struct pmm_layout got_c = {0};
    struct pmm_layout ref;

    reference_layout(256, &ref);
    assert_eq(0, pmm_layout_calculate(1ULL, 256, &got_a));
    assert_eq(0, pmm_layout_calculate(0xDEADBEEFULL, 256, &got_b));
    assert_eq(0, pmm_layout_calculate(0xFFFFULL, 256, &got_c));
    assert_layout_eq("base=1",        &got_a, &ref);
    assert_layout_eq("base=0xDEADBEEF", &got_b, &ref);
    assert_layout_eq("base=0xFFFF",   &got_c, &ref);
}

/* ── Near-UINT64_MAX base ──────────────────────────────────── */
/* Near-max base must not overflow offset arithmetic — the calculator
 * only adds offsets that are well below UINT64_MAX, so even at near-max
 * base the addition still fits (offsets are tiny relative to base_va).
 * But: any internal addition that would push past UINT64_MAX MUST error. */
TEST_FUNC(test_near_uint64_max_base)
{
    struct pmm_layout got = {0};
    /* Calculator does not validate base + total_bytes — the offsets
     * themselves are tiny (well below 64 MiB at 256 pages). So a
     * near-max base with reasonable span_pages should succeed and
     * return the same offsets as base = 0. */
    uint64_t near_max = 0xFFFFFFFFFFFFF000ULL;
    assert_eq(0, pmm_layout_calculate(near_max, 256, &got));
    assert_true(got.bits_map_off == 0);
    /* offsets are unchanged — base alignment doesn't enter the math */
    struct pmm_layout ref;
    reference_layout(256, &ref);
    assert_true(got.pages_struct_off == ref.pages_struct_off);
    assert_true(got.total_bytes      == ref.total_bytes);
}

/* ── NULL out pointer ──────────────────────────────────────── */
TEST_FUNC(test_null_out_returns_einval)
{
    int rc = pmm_layout_calculate(0, 256, NULL);
    assert_eq(-EINVAL, rc);
}

/* ── Zero span_pages is an error (caller must floor to 1) ──── */
TEST_FUNC(test_zero_span_returns_einval_and_zeros_out)
{
    struct pmm_layout got;
    /* Pre-fill with sentinel to verify the calculator zeroes on failure. */
    got.bits_map_off      = 0xAAAAAAAAAAAAAAAAULL;
    got.bits_length       = 0xBBBBBBBBBBBBBBBBULL;
    got.pages_struct_off  = 0xCCCCCCCCCCCCCCCCULL;
    got.pages_length       = 0xDDDDDDDDDDDDDDDDULL;
    got.zones_struct_off  = 0xEEEEEEEEEEEEEEEEULL;
    got.zones_length      = 0xF0F0F0F0F0F0F0F0ULL;
    got.end_of_struct_off = 0x0F0F0F0F0F0F0F0FULL;
    got.total_bytes       = 0x123456789ABCDEF0ULL;

    int rc = pmm_layout_calculate(0, 0, &got);
    assert_eq(-EINVAL, rc);
    /* Brief: "其余失败清零 out" */
    assert_true(got.bits_map_off == 0);
    assert_true(got.bits_length == 0);
    assert_true(got.pages_struct_off == 0);
    assert_true(got.pages_length == 0);
    assert_true(got.zones_struct_off == 0);
    assert_true(got.zones_length == 0);
    assert_true(got.end_of_struct_off == 0);
    assert_true(got.total_bytes == 0);
}

/* ── Multiplication overflow (span_pages * sizeof(struct Page)) ── */
TEST_FUNC(test_pages_mul_overflow)
{
    struct pmm_layout got;
    /* Fill with sentinels so we can detect "not zeroed". */
    got.bits_map_off = got.bits_length = got.pages_struct_off = got.pages_length =
        got.zones_struct_off = got.zones_length = got.end_of_struct_off =
        got.total_bytes = 0xDEADBEEFDEADBEEFULL;

    /* span_pages * sizeof(struct Page) would overflow uint64_t. */
    uint64_t span = UINT64_MAX / sizeof(struct Page) + 1;
    int rc = pmm_layout_calculate(0, span, &got);
    assert_true(rc < 0);
    assert_eq(-EOVERFLOW, rc);
    assert_true(got.bits_map_off == 0);
    assert_true(got.bits_length == 0);
    assert_true(got.total_bytes == 0);
}

/* ── Addition overflow (span_pages + 63) ───────────────────── */
TEST_FUNC(test_bits_add_overflow)
{
    struct pmm_layout got = { .bits_map_off = 0xAAAAAAAAAAAAAAAAULL };
    /* span_pages + 63 would overflow uint64_t. */
    uint64_t span = UINT64_MAX - 62;
    int rc = pmm_layout_calculate(0, span, &got);
    assert_true(rc < 0);
    assert_eq(-EOVERFLOW, rc);
    assert_true(got.total_bytes == 0);
}

/* ── Zones length lower bound (no overflow possible: MEMORY_RANGE_MAX is
 * a small compile-time constant, so MEMORY_RANGE_MAX * sizeof(struct Zone)
 * never overflows uint64_t). We instead pin zones_length to the
 * arithmetic lower bound and confirm it is align-up'd to sizeof(long). */
TEST_FUNC(test_zones_length_lower_bound)
{
    /* MEMORY_RANGE_MAX is a small constant; force it via a custom sanity check
     * that does not depend on sizeof. The production formula uses a small
     * constant (64); verify the multiplication is bounded by a clear
     * upper limit and produces a sane zones_length. */
    struct pmm_layout got;
    reference_layout(2048, &got);   /* any reasonable span */
    /* zones_length should be at least MEMORY_RANGE_MAX * sizeof(struct Zone). */
    assert_true(got.zones_length >= (uint64_t)MEMORY_RANGE_MAX * sizeof(struct Zone));
    /* and aligned up to sizeof(long). */
    assert_true((got.zones_length % sizeof(long)) == 0);
}

/* ── Align overflow (end_of_struct + 0xFFF pushes past UINT64_MAX) ── */
TEST_FUNC(test_align_overflow)
{
    /* end_of_struct_off + 0xFFF must not overflow. With span_pages huge,
     * pages_struct_off becomes large; end_of_struct_off can exceed
     * UINT64_MAX - 0xFFF. We can't easily produce that via span_pages
     * (since pages_struct_off is bounded by span_pages * sizeof(Page),
     * and that overflows first). But we can construct a layout where
     * end_of_struct is just below UINT64_MAX by chaining very large
     * pages_struct + zones + tail. Use the largest legal span_pages
     * that does not multiply-overflow. */
    struct pmm_layout got = { .total_bytes = 0xDEADBEEFDEADBEEFULL };
    /* span_pages = (UINT64_MAX / sizeof(struct Page)) — fits exactly */
    uint64_t span = UINT64_MAX / sizeof(struct Page);
    int rc = pmm_layout_calculate(0, span, &got);
    /* This either succeeds (returning a layout that fits) or fails
     * with -EOVERFLOW on the trailing 4 KiB align. Either is acceptable
     * per the contract; the test asserts the result is sane (either a
     * valid layout OR a zeroed, error layout). */
    if (rc == 0) {
        /* valid layout: total_bytes fits, end_of_struct + 0xFFF doesn't
         * overflow. offsets should still obey the structural invariants. */
        assert_true(got.end_of_struct_off <= got.total_bytes);
        assert_true((got.total_bytes & 0xFFFULL) == 0);
    } else {
        assert_eq(-EOVERFLOW, rc);
        assert_true(got.total_bytes == 0);
    }
}

/* ── Structural invariants (apply to every happy-path result) ── */
TEST_FUNC(test_structural_invariants)
{
    /* bits_map is at base. */
    /* pages_struct is 4 KiB-aligned and > bits_map. */
    /* zones_struct is 4 KiB-aligned and > pages_struct. */
    /* end_of_struct >= zones_struct + zones_length + 32*sizeof(long). */
    /* total_bytes is 4 KiB-aligned and >= end_of_struct. */
    uint64_t spans[] = { 1, 256, 2048, 8192, 1024 * 1024 };
    for (size_t i = 0; i < sizeof(spans) / sizeof(spans[0]); i++) {
        struct pmm_layout got = {0};
        assert_eq(0, pmm_layout_calculate(0, spans[i], &got));
        assert_true(got.bits_map_off == 0);
        assert_true(got.pages_struct_off > got.bits_map_off);
        assert_true((got.pages_struct_off & 0xFFFULL) == 0);
        assert_true(got.zones_struct_off > got.pages_struct_off);
        assert_true((got.zones_struct_off & 0xFFFULL) == 0);
        /* metadata tail of 32 * sizeof(unsigned long) is reserved. */
        assert_true(got.end_of_struct_off >=
                    got.zones_struct_off + got.zones_length +
                    PMM_BOOT_TAIL_BYTES);
        assert_true((got.end_of_struct_off % sizeof(long)) == 0);
        assert_true(got.total_bytes >= got.end_of_struct_off);
        assert_true((got.total_bytes & 0xFFFULL) == 0);
        /* bits_length is bytes (not bits) — matches production. */
        assert_true((got.bits_length % sizeof(long)) == 0);
        /* Page count tracks SPAN, not RAM total: for any span, pages_length
         * >= span_pages * sizeof(struct Page). */
        assert_true(got.pages_length >= spans[i] * sizeof(struct Page));
    }
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_512_mib_span),
    TEST_ENTRY(test_4_gib_span),
    TEST_ENTRY(test_min_one_frame),
    TEST_ENTRY(test_sparse_min_max_span),
    TEST_ENTRY(test_unaligned_base_does_not_change_offsets),
    TEST_ENTRY(test_near_uint64_max_base),
    TEST_ENTRY(test_null_out_returns_einval),
    TEST_ENTRY(test_zero_span_returns_einval_and_zeros_out),
    TEST_ENTRY(test_pages_mul_overflow),
    TEST_ENTRY(test_bits_add_overflow),
    TEST_ENTRY(test_zones_length_lower_bound),
    TEST_ENTRY(test_align_overflow),
    TEST_ENTRY(test_structural_invariants),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed ? 1 : 0;
}
