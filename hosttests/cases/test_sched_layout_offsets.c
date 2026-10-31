/* test_sched_layout_offsets.c — scheduler layout drift audit
 * (aarch64 EL1 scheduler plan, Phase 5 Task 4 / spec §10.1 items 1-2).
 *
 * Host-compiles the REAL <arch/context.h> facade (which pulls the real
 * per-arch thread_t definition AND its compile-time _Static_asserts —
 * so this TU fails to BUILD on any kernel-side layout drift too) and
 * then checks the x86 thread_t offsets against their RECORDED values,
 * so a future drift is caught by a test rather than by an audit script
 * that might be skipped.
 *
 * The recorded values are the frozen pre-facade x86 layout (Phase 1
 * pinned them in the header; this test pins them independently of the
 * header's own asserts — editing the header asserts alone cannot make
 * the drift pass).
 */

#include <arch/context.h>
#include "test_framework.h"

/* The x86 static asserts live in the real header; mirror the RECORDED
 * numbers here so the two can be compared by a test, not by eyeball. */
#define X86_RECORDED_RSP0_OFF        0
#define X86_RECORDED_RIP_OFF         8
#define X86_RECORDED_RSP_OFF        16
#define X86_RECORDED_FS_OFF         24
#define X86_RECORDED_GS_OFF         32
#define X86_RECORDED_CR3_OFF        40
#define X86_RECORDED_CR2_OFF        48
#define X86_RECORDED_TRAP_NR_OFF    56
#define X86_RECORDED_ERROR_CODE_OFF 64
#define X86_RECORDED_SCHED_FLAGS_OFF 72
#define X86_RECORDED_THREAD_SIZE    80

static void suite_x86_thread_offsets(void)
{
    TEST_SUITE("x86 thread_t recorded offsets have not drifted");
    assert_eq(X86_RECORDED_RSP0_OFF,        (int)__builtin_offsetof(thread_t, rsp0));
    assert_eq(X86_RECORDED_RIP_OFF,         (int)__builtin_offsetof(thread_t, rip));
    assert_eq(X86_RECORDED_RSP_OFF,         (int)__builtin_offsetof(thread_t, rsp));
    assert_eq(X86_RECORDED_FS_OFF,          (int)__builtin_offsetof(thread_t, fs));
    assert_eq(X86_RECORDED_GS_OFF,          (int)__builtin_offsetof(thread_t, gs));
    assert_eq(X86_RECORDED_CR3_OFF,         (int)__builtin_offsetof(thread_t, cr3));
    assert_eq(X86_RECORDED_CR2_OFF,         (int)__builtin_offsetof(thread_t, cr2));
    assert_eq(X86_RECORDED_TRAP_NR_OFF,     (int)__builtin_offsetof(thread_t, trap_nr));
    assert_eq(X86_RECORDED_ERROR_CODE_OFF,  (int)__builtin_offsetof(thread_t, error_code));
    assert_eq(X86_RECORDED_SCHED_FLAGS_OFF, (int)__builtin_offsetof(thread_t, sched_flags));
    assert_eq(X86_RECORDED_THREAD_SIZE,     (int)sizeof(thread_t));
}

static void suite_stack_invariants(void)
{
    TEST_SUITE("task_union stack invariants (both architectures)");
    assert_eq(32 * 1024, (int)STACK_SIZE);
    assert_eq(0, (int)(STACK_SIZE & (STACK_SIZE - 1)));  /* power of two */
    /* The task metadata + diagnostic reserve must fit below the usable
     * stack with the 256-byte margin the constructors rely on (the
     * full task_t/stack margin assert is in kernel/sched/kthread.c;
     * thread_t is the context share of that union). */
    assert_true(sizeof(thread_t) + 256 < STACK_SIZE);
}

int main(void)
{
    suite_x86_thread_offsets();
    suite_stack_invariants();
    printf("\n%s: %d total, %d passed, %d failed\n", "test_sched_layout_offsets",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
