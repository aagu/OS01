/* hosttests/cases/test_aarch64_sync_fault.c — EL1h sync diagnostic helper RED (Task 1).
 *
 * Spec contract (design §4 / plan Task 1):
 *   - aarch64_sync_far_valid(esr) returns true for the following ECs when
 *     FnV (ISS bit 10) is clear: 0x20/0x21 instruction aborts, 0x22 PC
 *     alignment, 0x24/0x25 data aborts, 0x34/0x35 watchpoints.
 *   - For 0x20/0x21/0x24/0x25/0x34/0x35 with bit 10 set, returns false.
 *   - PC alignment (0x22) is special: bit 10 does NOT encode FnV — it must
 *     return true even when bit 10 is set.
 *   - BRK (0x3c) returns false (no FAR semantic defined).
 *   - Unrelated EC (e.g. 0x3c, 0x11, 0x00) returns false.
 *
 * This test exercises the REAL header (kernel/include/arch/aarch64/sync_fault.h)
 * by including it directly.  RED before the header exists; GREEN once the
 * helper lands with the spec-defined truth table.
 */
#include <test_framework.h>
#include <stdint.h>
#include <stdbool.h>

#include <arch/aarch64/sync_fault.h>

/* Helpers to construct a synthetic ESR with a given EC and FnV bit. */
static inline uint64_t mk_esr(unsigned ec, bool fnv)
{
    return ((uint64_t)ec << 26) | (fnv ? (1UL << 10) : 0);
}

static void suite_instruction_aborts_clear(void)
{
    TEST_SUITE("instruction aborts (FnV clear) → FAR valid");
    assert_true(aarch64_sync_far_valid(mk_esr(0x20, false)));
    assert_true(aarch64_sync_far_valid(mk_esr(0x21, false)));
}

static void suite_pc_alignment(void)
{
    TEST_SUITE("PC alignment (0x22) ignores bit 10");
    assert_true(aarch64_sync_far_valid(mk_esr(0x22, false)));
    /* PC alignment has no FnV bit; bit 10 must NOT suppress FAR validity. */
    assert_true(aarch64_sync_far_valid(mk_esr(0x22, true)));
}

static void suite_data_aborts_clear(void)
{
    TEST_SUITE("data aborts (FnV clear) → FAR valid");
    assert_true(aarch64_sync_far_valid(mk_esr(0x24, false)));
    assert_true(aarch64_sync_far_valid(mk_esr(0x25, false)));
}

static void suite_watchpoints_clear(void)
{
    TEST_SUITE("watchpoints (FnV clear) → FAR valid");
    assert_true(aarch64_sync_far_valid(mk_esr(0x34, false)));
    assert_true(aarch64_sync_far_valid(mk_esr(0x35, false)));
}

static void suite_fnv_set(void)
{
    TEST_SUITE("FnV (bit 10) set → FAR invalid for aborts/watchpoints");
    assert_false(aarch64_sync_far_valid(mk_esr(0x20, true)));
    assert_false(aarch64_sync_far_valid(mk_esr(0x21, true)));
    assert_false(aarch64_sync_far_valid(mk_esr(0x24, true)));
    assert_false(aarch64_sync_far_valid(mk_esr(0x25, true)));
    assert_false(aarch64_sync_far_valid(mk_esr(0x34, true)));
    assert_false(aarch64_sync_far_valid(mk_esr(0x35, true)));
}

static void suite_unrelated_ec(void)
{
    TEST_SUITE("unrelated ECs → FAR not valid");
    assert_false(aarch64_sync_far_valid(mk_esr(0x3c, false)));  /* BRK */
    assert_false(aarch64_sync_far_valid(mk_esr(0x3c, true)));
    assert_false(aarch64_sync_far_valid(mk_esr(0x11, false)));  /* SVC */
    assert_false(aarch64_sync_far_valid(mk_esr(0x00, false)));  /* unknown */
    assert_false(aarch64_sync_far_valid(mk_esr(0x07, false)));  /* FP/SIMD */
}

int main(void)
{
    suite_instruction_aborts_clear();
    suite_pc_alignment();
    suite_data_aborts_clear();
    suite_watchpoints_clear();
    suite_fnv_set();
    suite_unrelated_ec();
    printf("\n%s: %d total, %d passed, %d failed\n", "test_aarch64_sync_fault",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
