/*
 * test/cases/test_libc_fflush.c — fflush() contract tests
 *
 * RED-test rationale (post-F2 analysis): the handoff's proposed RED test
 * ("printf("X") + fflush(stdout) + read pipe, assert X present") does NOT
 * go RED against current libc/stdio. Reason: libc/stdio is unbuffered —
 * every printf/fprintf/fwrite path emits data synchronously via write_all()
 * or direct write() syscalls. There is no libc-side buffer for fflush to
 * drain, so the data is already on fd 1 BEFORE fflush runs. That test
 * passes today regardless of fflush's implementation.
 *
 * The real "useful regardless" contract for fflush is the POSIX minimum:
 *
 *   - fflush(NULL)            → 0   (no streams to flush)
 *   - fflush(stdout/stderr)   → 0   (sentinels, libc owns them)
 *   - fflush(known FILE*)     → 0   (registered via fopen, valid)
 *   - fflush(unknown pointer) → EOF (-1)  (not a registered stream)
 *
 * That last rule is what makes fflush a *validator*, not just a no-op.
 * Today fflush returns 0 for ANY pointer — including garbage — which
 * means a typo'd stream silently succeeds. The test below fails on the
 * current implementation and passes once fflush tracks the fopen'd
 * FILE list (or otherwise validates the pointer).
 *
 * test_fflush_does_not_emit_extra_writes is a regression guard: it
 * captures the current "libc is unbuffered, fflush is a no-op" behavior
 * and will fire if any future fflush implementation regresses to
 * double-writing.
 */
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stdio_test_shims.h"

/* mini_file_t is defined in <stdio.h> but the host test framework pulls
 * in stdio.h via test_framework.h, so the layout { int fd; int mode; }
 * is visible. fopen() is the real one (linked from libc_stdio_file.o). */
extern void *fopen(const char *path, const char *mode);
extern int  fclose(void *f);

TEST_FUNC(test_fflush_null_returns_zero) {
    /* POSIX: fflush(NULL) flushes ALL streams. With no streams to flush
     * (and no per-stream buffer state in our libc), the contract is 0. */
    int rc = fflush(NULL);
    assert_eq(rc, 0);
}

TEST_FUNC(test_fflush_stdout_returns_zero) {
    /* stdout is a sentinel ((FILE*)2), not a mini_file_t. */
    int rc = fflush(stdout);
    assert_eq(rc, 0);
}

TEST_FUNC(test_fflush_stderr_returns_zero) {
    int rc = fflush(stderr);
    assert_eq(rc, 0);
}

TEST_FUNC(test_fflush_does_not_emit_extra_writes) {
    /* Regression guard: with libc unbuffered, fflush must not cause an
     * additional write(1, ...) call beyond what printf already issued.
     * Captures the current "fflush is a no-op beyond validation" contract. */
    shim_write_reset();
    int n = printf("X");
    assert_eq(n, 1);
    int writes_before = shim_write_count();
    int rc = fflush(stdout);
    int writes_after  = shim_write_count();
    assert_eq(rc, 0);
    assert_eq(writes_after, writes_before);
}

TEST_FUNC(test_fflush_unknown_pointer_returns_eof) {
    /* RED test: today fflush returns 0 for ANY void* — including
     * clearly invalid pointers. POSIX says fflush returns EOF on error
     * (including invalid stream argument). Once fflush tracks the
     * fopen'd FILE list, this should return -1 for unregistered
     * pointers. */
    void *bogus = (void *)0xdeadbeef;
    int rc = fflush(bogus);
    assert_eq(rc, -1);
}

TEST_FUNC(test_fflush_fopen_then_fflush_returns_zero) {
    /* Companion to test_fflush_unknown_pointer_returns_eof: a stream
     * that WAS registered via fopen() must be accepted by fflush(). */
    void *fp = fopen("/dev/null", "w");
    assert_not_null(fp);
    int rc = fflush(fp);
    assert_eq(rc, 0);
    fclose(fp);
}

TEST_FUNC(test_fflush_after_fclose_returns_eof) {
    /* Once fclose()'d, the FILE* slot must be unregistered. A stale
     * pointer (use-after-free) must be rejected by fflush(). */
    void *fp = fopen("/dev/null", "w");
    assert_not_null(fp);
    fclose(fp);
    int rc = fflush(fp);   /* stale pointer — must be rejected */
    assert_eq(rc, -1);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_fflush_null_returns_zero),
    TEST_ENTRY(test_fflush_stdout_returns_zero),
    TEST_ENTRY(test_fflush_stderr_returns_zero),
    TEST_ENTRY(test_fflush_does_not_emit_extra_writes),
    TEST_ENTRY(test_fflush_unknown_pointer_returns_eof),
    TEST_ENTRY(test_fflush_fopen_then_fflush_returns_zero),
    TEST_ENTRY(test_fflush_after_fclose_returns_eof),
TEST_LIST_END

int main(void) {
    printf("=== Test Runner ===\n");
    int size = sizeof(__test_table) / sizeof(__test_table[0]);
    for (int i = 0; i < size; i++) {
        printf("\n--- %s ---\n", __test_table[i].name);
        __test_table[i].fn();
    }
    int failed = __test_stats.failed;
    printf("\n  ---\n");
    printf("  Total: %d | Passed: %d | Failed: %d\n",
           __test_stats.total, __test_stats.passed, failed);
    if (failed > 0)
        printf("  >>> SOME TESTS FAILED <<<\n");
    else
        printf("  >>> ALL TESTS PASSED <<<\n");
    return failed > 0 ? 1 : 0;
}
