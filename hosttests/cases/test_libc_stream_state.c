/*
 * hosttests/cases/test_libc_stream_state.c — the stream-state contract
 * of the OS01 libc (plan task T5).
 *
 * What is under test
 * ------------------
 * The REAL production objects: libc/stdio/stdio_file.c (the stream core),
 * stdio_extras.c, printf.c, putchar.c, puts.c and getchar_unlocked.c.
 * They are linked into the test binary as they are — no renamed copies,
 * no re-implementation.
 *
 * Only the true boundaries are replaced (hosttests/mock/libc_stream_runtime.c):
 * the descriptor boundary (read/write/close) and the raw syscall entry.
 * Everything else — the resolver, the stream state, the EOF / error
 * indicators, the pushback byte, the registry — is production code.
 *
 * The four properties the task names
 * ----------------------------------
 *   eof_is_sticky          a read that reaches end-of-file sets the EOF
 *                          indicator and it STAYS set until clearerr()
 *                          (or a successful ungetc()) clears it.
 *   read_error_is_not_eof  an I/O failure sets the ERROR indicator and
 *                          leaves the EOF indicator alone: an error is
 *                          not an end-of-file.
 *   ungetc_one_byte        exactly ONE byte may be pushed back, EOF is
 *                          not a valid pushback character, and a
 *                          successful ungetc clears the EOF indicator.
 *   clearerr_both_flags    clearerr() drops the EOF indicator AND the
 *                          error indicator.
 *
 * Each of them is checked on a REGISTERED stream and on a STANDARD
 * stream, because the standard streams have to behave exactly like any
 * other stream.
 *
 * The destructive checks
 * ----------------------
 * Closing a standard stream really closes its descriptor, and a
 * registered stream whose descriptor happens to be 0, 1 or 2 must not be
 * mistaken for the corresponding sentinel.  Those checks take the
 * process' own standard output down with them, so they run LAST and
 * report through a diagnostic buffer that is published afterwards
 * (stream_rt_emit) — the report must not depend on the stream that was
 * just closed.
 */
#define _GNU_SOURCE
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "libc_stream_runtime.h"

/* Descriptors used by the registered-stream checks.  They are virtual:
 * the mock answers for them, so the test never depends on a host
 * descriptor existing. */
#define FD_SCRIPT_R 40   /* scripted read                             */
#define FD_FAIL_R   41   /* reads fail with EIO                       */
#define FD_FAIL_W   42   /* writes fail with EIO                      */
#define FD_CAPTURE  43   /* writes are captured                       */
#define FD_REG_BASE 44   /* 32 descriptors for the registry cap test  */

/* ── Diagnostics of the destructive phase ────────────────────────────
 *
 * The destructive checks cannot use the assertion macros: they close
 * the standard output, so a failing assertion could not print its
 * message any more.  They count their failures and describe them in a
 * buffer that is published after the fact. */
static char g_diag[4096];
static int  g_diag_failures;

static void diag_fail(const char *what)
{
    g_diag_failures++;
    size_t used = strlen(g_diag);
    if (used + 96 < sizeof(g_diag))
        snprintf(g_diag + used, sizeof(g_diag) - used, "  [FAIL] %s\n", what);
}

static int g_checks;

#define DC(cond, what) do {                         \
    g_checks++;                                     \
    if (!(cond)) diag_fail(what);                   \
} while (0)

/* ── EOF is sticky ─────────────────────────────────────────────────── */
TEST_FUNC(test_eof_is_sticky_on_a_registered_stream) {
    stream_rt_reset();
    stream_rt_virtual(FD_SCRIPT_R);
    stream_rt_script_read(FD_SCRIPT_R, "abc", 3);

    FILE *st = fdopen(FD_SCRIPT_R, "r");
    assert_not_null(st);

    assert_eq(getc_unlocked(st), 'a');
    assert_eq(getc_unlocked(st), 'b');
    assert_eq(getc_unlocked(st), 'c');
    /* The read reached the end of the file. */
    assert_eq(getc_unlocked(st), EOF);
    assert_eq(feof(st), 1);

    /* Sticky: a further read still reports EOF and the indicator
     * survives it. */
    assert_eq(getc_unlocked(st), EOF);
    assert_eq(feof(st), 1);
    assert_eq(ferror(st), 0);

    /* Only clearerr() drops the indicator. */
    clearerr(st);
    assert_eq(feof(st), 0);
    assert_eq(ferror(st), 0);

    assert_eq(fclose(st), 0);
}

TEST_FUNC(test_eof_is_sticky_on_the_standard_input) {
    stream_rt_reset();
    /* stdin reports end-of-file straight away. */
    stream_rt_script_read(0, "", 0);

    assert_eq(getchar(), EOF);
    assert_eq(feof(stdin), 1);

    /* Sticky on the standard stream as well. */
    assert_eq(getchar(), EOF);
    assert_eq(feof(stdin), 1);
    assert_eq(ferror(stdin), 0);

    clearerr(stdin);
    assert_eq(feof(stdin), 0);

    assert_eq(feof(stdout), 0);
    assert_eq(feof(stderr), 0);
}

/* ── An I/O error is not an end-of-file ────────────────────────────── */
TEST_FUNC(test_read_error_is_not_eof_on_a_registered_stream) {
    stream_rt_reset();
    stream_rt_virtual(FD_FAIL_R);
    stream_rt_fail_read(FD_FAIL_R, EIO);

    FILE *st = fdopen(FD_FAIL_R, "r");
    assert_not_null(st);

    assert_eq(getc_unlocked(st), EOF);
    /* The error indicator is set ... */
    assert_eq(ferror(st), 1);
    /* ... and the EOF indicator is NOT. */
    assert_eq(feof(st), 0);

    clearerr(st);
    assert_eq(ferror(st), 0);
    assert_eq(feof(st), 0);

    assert_eq(fclose(st), 0);
}

TEST_FUNC(test_read_error_is_not_eof_on_the_standard_input) {
    stream_rt_reset();
    stream_rt_fail_read(0, EIO);

    assert_eq(getchar(), EOF);
    assert_eq(ferror(stdin), 1);
    assert_eq(feof(stdin), 0);

    clearerr(stdin);
    assert_eq(ferror(stdin), 0);
}

/* ── ungetc holds exactly one byte ─────────────────────────────────── */
TEST_FUNC(test_ungetc_holds_exactly_one_byte) {
    stream_rt_reset();
    stream_rt_virtual(FD_SCRIPT_R);
    stream_rt_script_read(FD_SCRIPT_R, "xy", 2);

    FILE *st = fdopen(FD_SCRIPT_R, "r");
    assert_not_null(st);

    /* EOF is not a valid pushback character. */
    assert_eq(ungetc(EOF, st), EOF);

    /* The first byte is pushed back ... */
    assert_eq(ungetc('Z', st), 'Z');
    /* ... and only ONE byte may be held at a time. */
    assert_eq(ungetc('Y', st), EOF);

    /* The pushed byte comes back before the descriptor is read. */
    assert_eq(getc_unlocked(st), 'Z');
    /* Once it was consumed a new byte may be pushed. */
    assert_eq(ungetc('W', st), 'W');
    assert_eq(getc_unlocked(st), 'W');

    assert_eq(getc_unlocked(st), 'x');
    assert_eq(getc_unlocked(st), 'y');
    assert_eq(getc_unlocked(st), EOF);
    assert_eq(feof(st), 1);

    /* A successful ungetc clears the EOF indicator. */
    assert_eq(ungetc('R', st), 'R');
    assert_eq(feof(st), 0);
    assert_eq(getc_unlocked(st), 'R');
    assert_eq(getc_unlocked(st), EOF);
    assert_eq(feof(st), 1);

    assert_eq(fclose(st), 0);
}

TEST_FUNC(test_ungetc_on_the_standard_input) {
    stream_rt_reset();
    stream_rt_script_read(0, "hi", 2);

    assert_eq(ungetc(EOF, stdin), EOF);
    assert_eq(ungetc('K', stdin), 'K');
    assert_eq(getchar(), 'K');
    assert_eq(getchar(), 'h');
    assert_eq(getchar(), 'i');
    assert_eq(getchar(), EOF);
    assert_eq(feof(stdin), 1);

    /* A successful ungetc clears the EOF indicator of the standard
     * stream too. */
    assert_eq(ungetc('L', stdin), 'L');
    assert_eq(feof(stdin), 0);
    assert_eq(getchar(), 'L');
}

/* ── clearerr drops both indicators ────────────────────────────────── */
TEST_FUNC(test_clearerr_clears_both_indicators) {
    stream_rt_reset();
    stream_rt_virtual(FD_SCRIPT_R);
    /* The first read fails; the second one reaches the end of file. */
    stream_rt_script_read(FD_SCRIPT_R, "", 0);
    stream_rt_fail_read_once(FD_SCRIPT_R, EIO);

    FILE *st = fdopen(FD_SCRIPT_R, "r");
    assert_not_null(st);

    assert_eq(getc_unlocked(st), EOF);
    assert_eq(ferror(st), 1);
    assert_eq(feof(st), 0);

    assert_eq(getc_unlocked(st), EOF);
    /* Now BOTH indicators are set. */
    assert_eq(ferror(st), 1);
    assert_eq(feof(st), 1);

    clearerr(st);
    assert_eq(ferror(st), 0);
    assert_eq(feof(st), 0);

    assert_eq(fclose(st), 0);
}

TEST_FUNC(test_clearerr_on_the_standard_input) {
    stream_rt_reset();
    stream_rt_script_read(0, "", 0);
    stream_rt_fail_read_once(0, EIO);

    assert_eq(getchar(), EOF);
    assert_eq(ferror(stdin), 1);
    assert_eq(feof(stdin), 0);

    assert_eq(getchar(), EOF);
    assert_eq(ferror(stdin), 1);
    assert_eq(feof(stdin), 1);

    clearerr(stdin);
    assert_eq(ferror(stdin), 0);
    assert_eq(feof(stdin), 0);
}

/* ── The standard streams have their own capabilities ──────────────── */
TEST_FUNC(test_standard_streams_have_their_own_capabilities) {
    stream_rt_reset();

    /* stdin is not writable. */
    assert_eq(fwrite("x", 1, 1, stdin), 0);
    assert_eq(ferror(stdin), 1);
    clearerr(stdin);

    /* stdout and stderr are not readable. */
    char buf[4];
    assert_eq(fread(buf, 1, sizeof buf, stdout), 0);
    assert_eq(ferror(stdout), 1);
    clearerr(stdout);

    assert_eq(fread(buf, 1, sizeof buf, stderr), 0);
    assert_eq(ferror(stderr), 1);
    clearerr(stderr);

    /* The three standard streams are three separate streams: a failure
     * on one of them does not show up on the others. */
    assert_eq(ferror(stdin), 0);
    assert_eq(ferror(stdout), 0);
    assert_eq(ferror(stderr), 0);
    assert_eq(feof(stdin), 0);
    assert_eq(feof(stdout), 0);
    assert_eq(feof(stderr), 0);
}

/* ── Unknown and stale handles are rejected ────────────────────────── */
TEST_FUNC(test_invalid_and_closed_handles_are_rejected) {
    stream_rt_reset();
    char buf[4];
    FILE *bogus = (FILE *)(void *)(unsigned long)0xdeadbeef;

    assert_eq(fread(buf, 1, sizeof buf, bogus), 0);
    assert_eq(fwrite("x", 1, 1, bogus), 0);
    assert_eq(feof(bogus), 0);
    assert_eq(ferror(bogus), 0);
    assert_eq(fclose(bogus), EOF);
    assert_eq(fflush(bogus), EOF);
    assert_eq(ungetc('x', bogus), EOF);

    /* A closed stream is stale: every operation on it fails instead of
     * dereferencing freed memory. */
    stream_rt_virtual(FD_SCRIPT_R);
    stream_rt_script_read(FD_SCRIPT_R, "a", 1);
    FILE *st = fdopen(FD_SCRIPT_R, "r");
    assert_not_null(st);
    assert_eq(fclose(st), 0);
    assert_eq(fread(buf, 1, sizeof buf, st), 0);
    assert_eq(feof(st), 0);
    assert_eq(ferror(st), 0);
    assert_eq(fclose(st), EOF);
    assert_eq(fflush(st), EOF);
}

/* ── A failed write sets the error indicator ───────────────────────── */
TEST_FUNC(test_write_failure_sets_the_error_indicator) {
    stream_rt_reset();
    stream_rt_virtual(FD_FAIL_W);
    stream_rt_fail_write(FD_FAIL_W, EIO);

    FILE *st = fdopen(FD_FAIL_W, "w");
    assert_not_null(st);

    assert_eq(fwrite("x", 1, 1, st), 0);
    assert_eq(ferror(st), 1);
    assert_eq(feof(st), 0);
    assert_eq(fputs("y", st), EOF);

    clearerr(st);
    assert_eq(ferror(st), 0);
    assert_eq(fclose(st), 0);
}

/* ── The registry cap and the slot reuse ───────────────────────────── */
TEST_FUNC(test_registry_cap_and_slot_reuse) {
    stream_rt_reset();
    FILE *fps[32];

    for (int i = 0; i < 32; i++) {
        stream_rt_virtual(FD_REG_BASE + i);
        fps[i] = fdopen(FD_REG_BASE + i, "r");
        assert_not_null(fps[i]);
    }

    /* One over the cap must fail with ENOMEM. */
    errno = 0;
    FILE *over = fdopen(FD_REG_BASE + 32, "r");
    assert_null(over);
    assert_eq(errno, ENOMEM);

    for (int i = 0; i < 32; i++) {
        assert_eq(fclose(fps[i]), 0);
        /* The handle is stale now. */
        assert_eq(fflush(fps[i]), EOF);
    }

    /* The slots were released: the same 32 streams fit again. */
    for (int i = 0; i < 32; i++) {
        fps[i] = fdopen(FD_REG_BASE + i, "r");
        assert_not_null(fps[i]);
    }
    for (int i = 0; i < 32; i++)
        assert_eq(fclose(fps[i]), 0);
}

/* ── The formatted output goes through the stream state ────────────── */
TEST_FUNC(test_formatted_output_goes_through_the_stream_state) {
    stream_rt_reset();
    stream_rt_virtual(FD_CAPTURE);
    stream_rt_capture_write(FD_CAPTURE);

    FILE *st = fdopen(FD_CAPTURE, "w");
    assert_not_null(st);

    int written = fprintf(st, "%s-%d", "x", 7);
    assert_eq(written, 3);
    size_t len = 0;
    const char *out = stream_rt_written(FD_CAPTURE, &len);
    assert_not_null(out);
    assert_eq(len, 3);
    assert_mem_eq("x-7", out, 3);
    assert_eq(ferror(st), 0);
    assert_eq(feof(st), 0);

    assert_eq(fclose(st), 0);
}

/* ── The destructive phase ───────────────────────────────────────────
 *
 * Everything below closes a standard descriptor on purpose, so it runs
 * after the ordinary cases and reports through the diagnostic buffer. */
static int run_destructive_checks(void)
{
    /* Keep a copy of the three standard descriptors: the checks close
     * them on purpose and the report has to be published afterwards. */
    int saved0 = dup(0);
    int saved1 = dup(1);
    int saved2 = dup(2);

    /* 1. The positive path: the bytes really reach the descriptor.  It
     *    runs first, because the later phases close the standard
     *    output for good. */
    stream_rt_reset();
    stream_rt_capture_write(1);
    DC(printf("hi") == 2, "printf must report the number of characters written");
    {
        size_t len = 0;
        const char *out = stream_rt_written(1, &len);
        DC(out != NULL && len == 2 && out[0] == 'h' && out[1] == 'i',
           "printf must write through the standard output stream");
    }
    DC(putchar('x') == 'x', "putchar must report the character it wrote");
    DC(puts("ab") == 3, "puts must report the characters it wrote");

    /* 2. printf / putchar / puts write through the standard output
     *    STREAM: a failed write is visible as the error indicator and
     *    as a failed return value, never as a fake success. */
    stream_rt_reset();
    stream_rt_fail_write(1, EIO);
    DC(printf("x") == -1, "printf must fail when the write fails");
    DC(ferror(stdout) == 1, "a failed printf must set stdout's error indicator");
    DC(feof(stdout) == 0, "a failed write must not set the EOF indicator");
    DC(putchar('y') == EOF, "putchar must fail when the write fails");
    DC(puts("z") == EOF, "puts must fail when the write fails");
    clearerr(stdout);
    DC(ferror(stdout) == 0, "clearerr must clear stdout's error indicator");

    /* 2. A registered stream whose descriptor is 1 is a stream of its
     *    own — it is NOT the stdout sentinel. */
    stream_rt_reset();
    stream_rt_capture_write(1);
    FILE *reg1 = fdopen(1, "w");
    DC(reg1 != NULL, "fdopen(1) must succeed");
    DC(reg1 != stdout, "a registered stream must not be the stdout sentinel");
    DC(fileno_unlocked(reg1) == 1, "a registered stream must report its own fd");
    DC(fwrite("A", 1, 1, reg1) == 1, "a registered stream with fd 1 must be writable");
    {
        size_t len = 0;
        const char *out = stream_rt_written(1, &len);
        DC(out != NULL && len == 1 && out[0] == 'A',
           "a registered stream must own its own write state");
    }
    DC(feof(reg1) == 0 && ferror(reg1) == 0,
       "a registered stream must have its own indicators");
    DC(fclose(reg1) == 0, "fclose must close a registered stream");
    DC(stream_rt_close_count(1) == 1, "fclose must really close the underlying fd");

    /* Put a live descriptor back on fd 1 for the next phase. */
    dup2(saved1, 1);

    /* 3. The standard streams close for real. */
    stream_rt_reset();
    DC(fclose(stdout) == 0, "fclose(stdout) must close the standard stream");
    DC(stream_rt_close_count(1) == 1, "fclose(stdout) must really close fd 1");
    DC(printf("x") == -1, "printf must fail after stdout was closed");
    DC(fileno_unlocked(stdout) == -1, "a closed standard stream has no fd");
    DC(fwrite("x", 1, 1, stdout) == 0, "writing to a closed standard stream must fail");
    DC(fclose(stdout) == EOF, "closing an already closed stream must fail");

    DC(fclose(stderr) == 0, "fclose(stderr) must close the standard stream");
    DC(stream_rt_close_count(2) == 1, "fclose(stderr) must really close fd 2");
    DC(fprintf(stderr, "x") == -1, "fprintf must fail after stderr was closed");

    stream_rt_reset();
    stream_rt_script_read(0, "", 0);
    DC(fclose(stdin) == 0, "fclose(stdin) must close the standard stream");
    DC(stream_rt_close_count(0) == 1, "fclose(stdin) must really close fd 0");
    DC(getchar() == EOF, "reading from a closed standard stream must fail");

    /* 4. It is the STREAM STATE that blocks the writes, not the
     *    descriptor: a live descriptor on fd 1 does not revive stdout. */
    dup2(saved1, 1);
    DC(printf("x") == -1, "a closed stdout must stay closed");
    DC(fileno_unlocked(stdout) == -1, "a closed stdout must have no fd");

    /* 5. A registered stream whose descriptor is 0 or 2 is a stream of
     *    its own as well. */
    dup2(saved0, 0);
    dup2(saved2, 2);

    stream_rt_reset();
    stream_rt_script_read(0, "hi", 2);
    FILE *reg0 = fdopen(0, "r");
    DC(reg0 != NULL, "fdopen(0) must succeed");
    DC(reg0 != stdin, "a registered stream must not be the stdin sentinel");
    DC(fileno_unlocked(reg0) == 0, "a registered stream must report its own fd");
    {
        char buf[4] = {0};
        DC(fread(buf, 1, 2, reg0) == 2, "a registered stream with fd 0 must be readable");
        DC(buf[0] == 'h' && buf[1] == 'i', "the registered stream must read its own data");
    }
    DC(fclose(reg0) == 0, "fclose must close a registered stream");
    DC(stream_rt_close_count(0) == 1, "fclose must really close the underlying fd");

    stream_rt_reset();
    stream_rt_capture_write(2);
    FILE *reg2 = fdopen(2, "w");
    DC(reg2 != NULL, "fdopen(2) must succeed");
    DC(reg2 != stderr, "a registered stream must not be the stderr sentinel");
    DC(fileno_unlocked(reg2) == 2, "a registered stream must report its own fd");
    DC(fwrite("B", 1, 1, reg2) == 1, "a registered stream with fd 2 must be writable");
    DC(fclose(reg2) == 0, "fclose must close a registered stream");
    DC(stream_rt_close_count(2) == 1, "fclose must really close the underlying fd");

    /* Leave the descriptors usable for the report. */
    dup2(saved0, 0);
    dup2(saved1, 1);
    dup2(saved2, 2);

    return g_checks;
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_eof_is_sticky_on_a_registered_stream),
    TEST_ENTRY(test_eof_is_sticky_on_the_standard_input),
    TEST_ENTRY(test_read_error_is_not_eof_on_a_registered_stream),
    TEST_ENTRY(test_read_error_is_not_eof_on_the_standard_input),
    TEST_ENTRY(test_ungetc_holds_exactly_one_byte),
    TEST_ENTRY(test_ungetc_on_the_standard_input),
    TEST_ENTRY(test_clearerr_clears_both_indicators),
    TEST_ENTRY(test_clearerr_on_the_standard_input),
    TEST_ENTRY(test_standard_streams_have_their_own_capabilities),
    TEST_ENTRY(test_invalid_and_closed_handles_are_rejected),
    TEST_ENTRY(test_write_failure_sets_the_error_indicator),
    TEST_ENTRY(test_registry_cap_and_slot_reuse),
    TEST_ENTRY(test_formatted_output_goes_through_the_stream_state),
TEST_LIST_END

int main(void) {
    printf("=== Test Runner ===\n");
    (void)__test_table_size;
    int size = sizeof(__test_table) / sizeof(__test_table[0]);
    for (int i = 0; i < size; i++) {
        printf("\n--- %s ---\n", __test_table[i].name);
        __test_table[i].fn();
    }

    /* The destructive checks close the standard descriptors, so they run
     * after every ordinary case. */
    int destructive = run_destructive_checks();

    int total  = __test_stats.total + destructive;
    int failed = __test_stats.failed + g_diag_failures;
    int passed = total - failed;

    char report[512];
    int n = snprintf(report, sizeof report,
                     "\n  ---\n"
                     "  Total: %d | Passed: %d | Failed: %d\n"
                     "%s",
                     total, passed, failed,
                     failed > 0 ? "  >>> SOME TESTS FAILED <<<\n"
                                : "  >>> ALL TESTS PASSED <<<\n");
    /* stdout may be closed by now, so the report is published through
     * the mock's direct path instead of the libc under test. */
    stream_rt_emit(g_diag, strlen(g_diag));
    if (n > 0)
        stream_rt_emit(report, (size_t)n);
    return failed > 0 ? 1 : 0;
}
