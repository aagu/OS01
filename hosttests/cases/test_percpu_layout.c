/*
 * hosttests/cases/test_percpu_layout.c — pin percpu_t byte layout
 * (aarch64 M3 plan Task 9; fields landed in Task 7).
 *
 * Why: aarch64 head.S strides percpu_data[] with the numeric literal
 * PERCPU_DATA_SIZE (assembly cannot run sizeof()), and
 * kernel/percpu/percpu.c's percpu_init() deliberately stops its
 * memset 8 bytes short of sizeof(percpu_t) so the ipi_ready/tlb_ack_gen
 * tail survives re-init (one-shot publication semantics). Both facts
 * are pinned at compile time by _Static_asserts in
 * kernel/include/percpu/percpu.h and percpu.c; this hosttest repeats
 * them at runtime so the layout contract is exercised by the host test
 * suite too, not just by whichever TU happens to include the header.
 *
 * RED history: before Task 7 landed the fields, this TU would not even
 * compile (__builtin_offsetof(percpu_t, ipi_ready) → no member). Task 7
 * landed the fields + the 152 bump; M3 Task 12 later removed the legacy
 * tlb_wanted/tlb_ack pair, shrinking the struct back to 144 — the
 * `>= 152` assertion became stale and now pins == PERCPU_DATA_SIZE.
 */
#include <test_framework.h>
#include <stddef.h>
#include <percpu/percpu.h>

/* Header-only TU: percpu.h pulls sched/task.h whose static inline
 * wake/idle helpers reference idle_resume(); at -O0 clang emits the
 * out-of-line copy, so provide a never-called stub to keep the link
 * self-contained. */
void idle_resume(void) {}

TEST_FUNC(test_percpu_t_size_bumped) {
    /* M3 Task 12 removed the legacy tlb_wanted/tlb_ack pair, so the
     * struct shrank back to 144 bytes (ipi_ready + tlb_ack_gen tail).
     * Any future field addition must bump PERCPU_DATA_SIZE (and
     * re-check asm stride sites). */
    assert_true(sizeof(percpu_t) == PERCPU_DATA_SIZE);
}

TEST_FUNC(test_percpu_t_field_offsets_aligned) {
    /* Both M3 tail fields are uint32_t-class: 4-byte aligned is the
     * architectural minimum for the LDR/LDAR accesses Task 16+ emits. */
    assert_true(__builtin_offsetof(percpu_t, ipi_ready) % 4 == 0);
    assert_true(__builtin_offsetof(percpu_t, tlb_ack_gen) % 4 == 0);
}

TEST_FUNC(test_percpu_t_tail_is_last_8_bytes) {
    /* percpu_init() memsets sizeof(percpu_t) - 8 bytes only; the M3
     * tail must therefore be exactly the last 8 bytes. Mirrors the
     * _Static_asserts in percpu.h/percpu.c. */
    assert_eq(sizeof(percpu_t) - 8, __builtin_offsetof(percpu_t, ipi_ready));
    assert_eq(sizeof(percpu_t) - 4, __builtin_offsetof(percpu_t, tlb_ack_gen));
}

TEST_FUNC(test_static_assert_holds) {
    /* The compile-time pin (same expression as the _Static_assert in
     * percpu.h); here we observe it at runtime so a mismatch is a
     * visible test failure rather than a build break in an unrelated TU. */
    assert_true(sizeof(percpu_t) == PERCPU_DATA_SIZE);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_percpu_t_size_bumped),
    TEST_ENTRY(test_percpu_t_field_offsets_aligned),
    TEST_ENTRY(test_percpu_t_tail_is_last_8_bytes),
    TEST_ENTRY(test_static_assert_holds),
TEST_LIST_END

int main(void) {
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
