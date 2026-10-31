/* test_switch_complete.c — common switch tail task_switch_complete()
 * (aarch64 EL1 scheduler plan Phase 1, Task 2).
 *
 * The REAL kernel/sched/kthread.c is host-compiled via
 * switch_complete_test_runtime.h (spy task_finish_switch / kfree and a
 * test-controlled current); everything asserted here runs against that
 * production object.  No test-local reimplementation of the tail exists.
 *
 * The bug this suite pins (spec §8.3 / plan Review Focus 1): reading
 * prev->thread / prev->fpu_save / prev->stack_alloc_base AFTER
 * task_finish_switch(prev) clears on_cpu is a read-after-free — a waiter
 * may free prev in that window.  The spy task_finish_switch poisons those
 * fields, so any late read frees a detectable sentinel pointer.
 *
 * Cases:
 *   1. self-reap: finish called once; exactly thread, fpu and raw stack
 *      freed, with the PRE-poison pointers (capture-before-finish proof)
 *      and in that order (raw stack last, spec §8.3), after finish;
 *   2. non-self-reap: finish called, nothing freed;
 *   3. actual_prev == current: neither finish nor free, even with
 *      PF_SELF_REAP set (never finish/free our own stack);
 *   4. self-reap with thread == NULL: only the non-NULL objects freed.
 */

#include "test_framework.h"
/* Included explicitly because the test TU does not preinclude it; the
 * production TU gets it via -include on the Makefile rule (pattern of
 * test_monitor_accounting.c). */
#include "../mock/switch_complete_test_runtime.h"

static thread_t thd_obj;
static char fpu_buf[64] __attribute__((aligned(16)));
static char stk_buf[4096];
static task_t prev_task;

/* Control + spy state, task_finish_switch / kmalloc / kfree and
 * switch_tail_reset() moved into ../mock/switch_complete_test_runtime.h
 * (Phase 2, Task 1): the REAL kernel/sched/kthread.c now also carries the
 * kthread_alloc_and_publish() constructor, so the mock must provide the
 * whole constructor surface in every test TU that links the production
 * object — this file keeps only its local fixtures.
 */

/* ── Case 1: self-reap captures before finish, frees after ── */
static void suite_self_reap_capture(void)
{
    TEST_SUITE("self-reap capture-before-finish");
    switch_tail_reset();
    prev_task.flags = PF_SELF_REAP;
    prev_task.thread = &thd_obj;
    prev_task.fpu_save = fpu_buf;
    prev_task.stack_alloc_base = stk_buf;

    task_switch_complete(&prev_task);

    /* finish ran exactly once, on prev */
    assert_eq(1, spy_finish_calls);
    assert_true(spy_finish_last == &prev_task);

    /* exactly the three objects freed */
    assert_eq(3, spy_kfree_calls);

    /* THE ordering proof: the freed pointers are the PRE-poison values.
     * A tail that read prev->thread/fpu_save/stack_alloc_base after
     * task_finish_switch would hand kfree the poison sentinels. */
    assert_true((void *)spy_kfreed[0] == (void *)&thd_obj);
    assert_true(spy_kfreed[1] == (void *)fpu_buf);
    assert_true(spy_kfreed[2] == (void *)stk_buf);
    assert_true(spy_kfreed[0] != (void *)SWITCH_TAIL_POISON_THREAD);
    assert_true(spy_kfreed[1] != SWITCH_TAIL_POISON_FPU);
    assert_true(spy_kfreed[2] != SWITCH_TAIL_POISON_STACK);

    /* freed in ownership order, raw stack LAST (spec §8.3) */
    assert_true((void *)spy_kfreed[0] == (void *)&thd_obj);
    assert_true((void *)spy_kfreed[2] == (void *)stk_buf);

    /* every free happened after the finish publish */
    assert_true(spy_seq_finish < spy_seq_kfree[0]);
    assert_true(spy_seq_finish < spy_seq_kfree[2]);
}

/* ── Case 2: non-self-reap finishes but frees nothing ── */
static void suite_non_self_reap(void)
{
    TEST_SUITE("non-self-reap frees nothing");
    switch_tail_reset();
    prev_task.flags = 0; /* e.g. a plain PF_KTHREAD without SELF_REAP */
    prev_task.thread = &thd_obj;
    prev_task.fpu_save = fpu_buf;
    prev_task.stack_alloc_base = stk_buf;

    task_switch_complete(&prev_task);

    assert_eq(1, spy_finish_calls);
    assert_true(spy_finish_last == &prev_task);
    assert_eq(0, spy_kfree_calls);
}

/* ── Case 3: self-switch is a no-op even with PF_SELF_REAP ── */
static void suite_self_switch_noop(void)
{
    TEST_SUITE("self-switch no-op");
    switch_tail_reset();
    prev_task.flags = PF_SELF_REAP;
    prev_task.thread = &thd_obj;
    prev_task.fpu_save = fpu_buf;
    prev_task.stack_alloc_base = stk_buf;
    switch_tail_current = &prev_task; /* actual_prev == current */

    task_switch_complete(&prev_task);

    assert_eq(0, spy_finish_calls);
    assert_eq(0, spy_kfree_calls);
}

/* ── Case 4: self-reap with a NULL member frees only what exists ── */
static void suite_self_reap_null_thread(void)
{
    TEST_SUITE("self-reap null member");
    switch_tail_reset();
    prev_task.flags = PF_SELF_REAP;
    prev_task.thread = NULL;
    prev_task.fpu_save = fpu_buf;
    prev_task.stack_alloc_base = stk_buf;

    task_switch_complete(&prev_task);

    assert_eq(1, spy_finish_calls);
    assert_eq(2, spy_kfree_calls);
    assert_true(spy_kfreed[0] == (void *)fpu_buf);
    assert_true(spy_kfreed[1] == (void *)stk_buf);
}

static void suite_kthread_yield_irq_restore(void)
{
    TEST_SUITE("kthread_yield restores caller IRQ state");
    mock_irq_state = 0;
    mock_schedule_calls = 0;
    mock_irq_restore_calls = 0;
    mock_restore_after_schedule = 0;
    kthread_yield();
    assert_eq(1, mock_schedule_calls);
    assert_eq(1, mock_irq_restore_calls);
    assert_eq(1, mock_restore_after_schedule);
    assert_eq(0, mock_irq_state);

    mock_irq_state = 1;
    mock_schedule_calls = 0;
    mock_irq_restore_calls = 0;
    mock_restore_after_schedule = 0;
    kthread_yield();
    assert_eq(1, mock_schedule_calls);
    assert_eq(1, mock_irq_restore_calls);
    assert_eq(1, mock_restore_after_schedule);
    assert_eq(1, mock_irq_state);
}

int main(void)
{
    suite_self_reap_capture();
    suite_non_self_reap();
    suite_self_switch_noop();
    suite_self_reap_null_thread();
    suite_kthread_yield_irq_restore();
    printf("\n%s: %d total, %d passed, %d failed\n", "test_switch_complete",
           __test_stats.total, __test_stats.passed, __test_stats.failed);
    return __test_stats.failed ? 1 : 0;
}
