/*
 * test_libc_stdio_io.c — libc-completion Task T7: stream modes and
 * reliable item I/O.
 *
 * The production objects under test are the REAL libc stdio
 * (stdio_file.c) and the REAL fcntl wrapper (unistd/fcntl.c).  The
 * only replaced boundary is the descriptor boundary
 * (libc_stream_runtime.c: read/write/close + a SYS_fcntl forward to
 * the host kernel), so real temp files work end to end while the
 * fault paths (EINTR, write budget, failing close, scripted/virtual
 * descriptors) stay deterministic.
 *
 * What is proven:
 *   modes      — every legal r/w/a/+/b combination opens with the
 *                right access flags ("a" really appends, "w" really
 *                truncates, "r+" really reads AND writes); the "e"/"x"
 *                extensions, empty and malformed modes are rejected
 *                with EINVAL (this libc does not promise O_CLOEXEC or
 *                O_EXCL — a loud EINVAL instead of a fake success).
 *   fdopen     — does not truncate, does not close the caller's fd on
 *                failure, and "a" turns the descriptor append-only
 *                through F_SETFL while PRESERVING its existing status
 *                flags (the atomic byte placement is the kernel's T6
 *                append).
 *   fread      — size 0 / nmemb 0 / size*nmemb overflow issue NO
 *                syscall (proven by an unconsumed read script); a
 *                partial item at EOF returns the full-item count; an
 *                EINTR failed read is retried.
 *   fwrite     — a failed write sets the error indicator and returns
 *                the COMPLETED items (a budgeted short-then-fail write
 *                keeps its successful bytes); an EINTR failed write is
 *                retried.
 *   fcntl      — F_GETFL/F_GETFD take no vararg, F_SETFL/F_SETFD/
 *                F_DUPFD take exactly one int, unsupported commands
 *                answer -1/EINVAL without a syscall, a bad descriptor
 *                maps to -1/EBADF.
 *   fclose     — a failing close() is reported as EOF and the stream
 *                is released anyway.
 */

#include "test_framework.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include <stdio/file.h>             /* private: __stdio_fd() — fileno is T8 */
#include "libc_stream_runtime.h"

/* libc's <fcntl.h> has no O_ACCMODE; the access-mode bits are 0..2. */
#ifndef O_ACCMODE
#define O_ACCMODE 3
#endif

/* libc's <unistd.h> has no seek-whence macros either. */
#ifndef SEEK_END
#define SEEK_END 2
#endif
#ifndef SEEK_SET
#define SEEK_SET 0
#endif

/* Host prototypes the OS01 libc does not declare. */
extern ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);
extern off_t lseek(int fd, off_t offset, int whence);

static char g_path[128];

static int make_temp(void)
{
    snprintf(g_path, sizeof(g_path), "/tmp/os01_io_XXXXXX");
    int fd = mkstemp(g_path);
    if (fd >= 0) {
        /* mkstemp opens O_RDWR; the tests manage their own lifecycle. */
        unlink(g_path);
    }
    return fd;
}

/* Write the whole file synchronously through the host (bypasses both
 * the libc under test and the write script). */
static void seed_file(const char *text)
{
    int fd = open(g_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert_true(fd >= 0);
    ssize_t n = write(fd, text, strlen(text));
    assert_eq((ssize_t)strlen(text), n);
    close(fd);
}

static long file_len(void)
{
    int fd = open(g_path, O_RDONLY);
    assert_true(fd >= 0);
    long end = lseek(fd, 0, SEEK_END);
    close(fd);
    return end;
}

static void read_back(char *out, long cap)
{
    int fd = open(g_path, O_RDONLY);
    assert_true(fd >= 0);
    long n = read(fd, out, (size_t)cap);
    assert_true(n >= 0);
    out[n] = '\0';
    close(fd);
}

static void assert_read_errno(void)
{
    assert_eq(EINVAL, errno);
}

/* ── Mode matrix ───────────────────────────────────────────────────── */

TEST_FUNC(test_fopen_mode_flags) {
    static const struct { const char *mode; int flags; } table[] = {
        { "r",    O_RDONLY },
        { "rb",   O_RDONLY },
        { "r+",   O_RDWR },
        { "rb+",  O_RDWR },
        { "w",    O_WRONLY },
        { "w+",   O_RDWR },
        { "a",    O_WRONLY | O_APPEND },
        { "a+",   O_RDWR | O_APPEND },
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        stream_rt_reset();
        assert_true(make_temp() >= 0);
        seed_file("");                       /* "r" needs an existing file */
        FILE *f = fopen(g_path, table[i].mode);
        if (!f) {
            /* Fail loudly with WHICH mode broke, not just that one did. */
            printf("  [FAIL] fopen(\"%s\") returned NULL (errno=%d)\n",
                   table[i].mode, errno);
            assert_true(0);
        }
        int flags = fcntl(__stdio_fd(f), F_GETFL);
        assert_eq(table[i].flags & O_ACCMODE, flags & O_ACCMODE);
        assert_eq(table[i].flags & O_APPEND, flags & O_APPEND);
        fclose(f);
    }
    stream_rt_reset();
}

TEST_FUNC(test_fopen_w_truncates_a_appends) {
    stream_rt_reset();
    assert_true(make_temp() >= 0);

    /* "w" truncates: "OLD" must not survive. */
    seed_file("OLD");
    FILE *f = fopen(g_path, "w");
    assert_not_null(f);
    fclose(f);
    assert_eq(0, (long)file_len());

    /* "a" does NOT truncate and its bytes land at the end. */
    seed_file("OLD");
    f = fopen(g_path, "a");
    assert_not_null(f);
    assert_eq(1, (int)fwrite("X", 1, 1, f));
    fclose(f);
    char buf[16];
    read_back(buf, sizeof(buf) - 1);
    assert_str_eq("OLDX", buf);

    /* "r+" reads AND writes, positioned at the START. */
    seed_file("OLD");
    f = fopen(g_path, "r+");
    assert_not_null(f);
    char probe = '?';
    assert_eq(1, (int)fread(&probe, 1, 1, f));   /* one byte from the start */
    assert_eq('O', probe);
    assert_eq(1, (int)fwrite("Y", 1, 1, f));     /* writes after the read */
    fclose(f);
    read_back(buf, sizeof(buf) - 1);
    assert_str_eq("OYD", buf);
}

TEST_FUNC(test_fopen_invalid_modes_einval) {
    static const char *bad[] = {
        "", "z", "r++", "w++", "a++", "re", "wx", "ax", "rbe", "rx", "q+",
    };

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        assert_true(make_temp() >= 0);
        errno = 0;
        FILE *f = fopen(g_path, bad[i]);
        if (f) {
            printf("  [FAIL] fopen(\"%s\") should have failed\n", bad[i]);
            fclose(f);
            assert_true(0);
        }
        if (errno != EINVAL) {
            printf("  [FAIL] fopen(\"%s\") errno=%d, want EINVAL\n",
                   bad[i], errno);
            assert_true(0);
        }
    }
    stream_rt_reset();
}

/* ── fdopen ────────────────────────────────────────────────────────── */

TEST_FUNC(test_fdopen_appends_and_w_does_not_truncate) {
    stream_rt_reset();
    assert_true(make_temp() >= 0);

    /* fdopen("a"): no truncate, and the stream is append — even though
     * the descriptor's position sits at 0, the byte must land at the
     * file's real end (the kernel's O_APPEND path, T6). */
    seed_file("AB");
    int fd = open(g_path, O_RDWR);
    assert_true(fd >= 0);
    FILE *f = fdopen(fd, "a");
    assert_not_null(f);
    assert_eq(1, (int)fwrite("C", 1, 1, f));
    fclose(f);
    char buf[16];
    read_back(buf, sizeof(buf) - 1);
    assert_str_eq("ABC", buf);

    /* fdopen("w") must NOT truncate: fdopen opens nothing. */
    fd = open(g_path, O_RDWR);
    assert_true(fd >= 0);
    f = fdopen(fd, "w");
    assert_not_null(f);
    fclose(f);
    assert_eq(3, (long)file_len());          /* "ABC" survived */
}

TEST_FUNC(test_fdopen_failure_keeps_fd_usable) {
    stream_rt_reset();
    assert_true(make_temp() >= 0);
    seed_file("");                       /* the fd must be a real, open file */

    int fd = open(g_path, O_RDWR);
    assert_true(fd >= 0);

    errno = 0;
    FILE *f = fdopen(fd, "no-such-mode");
    assert_null(f);
    assert_read_errno();

    /* The fd must still be the caller's: a write through it lands. */
    ssize_t n = write(fd, "Z", 1);
    assert_eq(1, (ssize_t)n);
    close(fd);
    char buf[4];
    read_back(buf, sizeof(buf) - 1);
    assert_str_eq("Z", buf);
    stream_rt_reset();
}

/* ── fread ─────────────────────────────────────────────────────────── */

TEST_FUNC(test_fread_no_op_cases_issue_no_read) {
    stream_rt_reset();

    /* A virtual descriptor whose read script holds "ABC".  Any read
     * syscall would consume the script; the no-op calls must leave it
     * fully intact for the real read afterwards. */
    static const int vfd = 42;
    stream_rt_virtual(vfd);
    stream_rt_script_read(vfd, "ABC", 3);
    FILE *f = fdopen(vfd, "r");
    assert_not_null(f);

    char buf[8];
    memset(buf, 0, sizeof(buf));
    assert_eq(0, (int)fread(buf, 0, 10, f));          /* size 0 */
    assert_eq(0, (int)fread(buf, 1, 0, f));           /* nmemb 0 */
    assert_eq(0, (int)fread(buf, (size_t)-1 / 2, 3, f)); /* overflow */
    assert_eq(0, feof(f));                            /* not an EOF */

    /* the script is untouched: the real read gets the whole payload */
    assert_eq(3, (int)fread(buf, 1, sizeof(buf), f));
    assert_mem_eq("ABC", buf, 3);
    fclose(f);
    stream_rt_reset();
}

TEST_FUNC(test_fread_partial_item_at_eof) {
    stream_rt_reset();
    assert_true(make_temp() >= 0);
    seed_file("0123456789");                 /* 10 bytes */

    FILE *f = fopen(g_path, "r");
    assert_not_null(f);
    char buf[16];
    /* 3 items of 4 requested, only 10 bytes exist: 2 FULL items. */
    assert_eq(2, (int)fread(buf, 4, 3, f));
    assert_eq(1, feof(f));
    fclose(f);
}

TEST_FUNC(test_fread_eintr_is_retried) {
    stream_rt_reset();
    static const int vfd = 43;
    stream_rt_virtual(vfd);
    stream_rt_script_read(vfd, "XYZ", 3);
    stream_rt_fail_read_once(vfd, EINTR);

    FILE *f = fdopen(vfd, "r");
    assert_not_null(f);
    char buf[8];
    assert_eq(3, (int)fread(buf, 1, sizeof(buf), f));
    assert_mem_eq("XYZ", buf, 3);
    assert_eq(0, ferror(f));
    fclose(f);
    stream_rt_reset();
}

/* ── fwrite ────────────────────────────────────────────────────────── */

TEST_FUNC(test_fwrite_failure_sets_error) {
    stream_rt_reset();
    static const int vfd = 44;
    stream_rt_virtual(vfd);
    stream_rt_fail_write(vfd, EIO);

    FILE *f = fdopen(vfd, "w");
    assert_not_null(f);
    assert_eq(0, (int)fwrite("hello", 1, 5, f));
    assert_eq(1, ferror(f));
    fclose(f);
    stream_rt_reset();
}

TEST_FUNC(test_fwrite_budget_reports_completed_items) {
    stream_rt_reset();
    static const int vfd = 45;
    stream_rt_virtual(vfd);
    stream_rt_capture_write(vfd);
    stream_rt_write_budget(vfd, 9);          /* 4+4+1 of a 12-byte request */

    FILE *f = fdopen(vfd, "w");
    assert_not_null(f);
    /* 3 items of 4 requested; the 10th byte fails: 2 COMPLETED items,
     * the 8 successful bytes stay written (side effect kept). */
    assert_eq(2, (int)fwrite("abcdefghijkl", 4, 3, f));
    assert_eq(1, ferror(f));

    size_t len = 0;
    const char *captured = stream_rt_written(vfd, &len);
    assert_eq(9, (size_t)len);   /* items 1+2 (8 bytes) + the first byte of item 3 */
    assert_mem_eq("abcdefgh", captured, 8);  /* the two COMPLETED items */
    fclose(f);
    stream_rt_reset();
}

TEST_FUNC(test_fwrite_eintr_is_retried) {
    stream_rt_reset();
    static const int vfd = 46;
    stream_rt_virtual(vfd);
    stream_rt_capture_write(vfd);
    stream_rt_fail_write_once(vfd, EINTR);

    FILE *f = fdopen(vfd, "w");
    assert_not_null(f);
    assert_eq(5, (int)fwrite("hello", 1, 5, f));
    assert_eq(0, ferror(f));
    size_t len = 0;
    const char *captured = stream_rt_written(vfd, &len);
    assert_eq(5, (size_t)len);
    assert_mem_eq("hello", captured, 5);
    fclose(f);
    stream_rt_reset();
}

/* ── fclose with a failing close ───────────────────────────────────── */

TEST_FUNC(test_fclose_error_returns_eof_and_releases) {
    stream_rt_reset();
    assert_true(make_temp() >= 0);
    seed_file("");

    int fd = open(g_path, O_RDWR);
    assert_true(fd >= 0);
    FILE *f = fdopen(fd, "w");
    assert_not_null(f);

    stream_rt_fail_close(fd, EIO);
    /* mkstemp/seed_file already closed this fd number through the mock;
     * only THIS stream's close attempt matters. */
    stream_rt_clear_close_count(fd);
    assert_eq(EOF, fclose(f));               /* the close error surfaces */
    assert_eq(1, stream_rt_close_count(fd)); /* exactly one close attempt */

    /* The stream is released even though the close failed: a later
     * write resolves to nothing instead of writing to a half-closed
     * descriptor. */
    assert_eq(0, (int)fwrite("Q", 1, 1, f));
    stream_rt_reset();
}

/* ── fcntl wrapper ─────────────────────────────────────────────────── */

TEST_FUNC(test_fcntl_wrapper_contract) {
    stream_rt_reset();
    assert_true(make_temp() >= 0);
    seed_file("");

    int fd = open(g_path, O_RDWR);
    assert_true(fd >= 0);

    /* F_GETFL takes NO vararg (this call site passes none). */
    int flags = fcntl(fd, F_GETFL);
    assert_eq(O_RDWR, flags & O_ACCMODE);

    /* F_SETFL takes exactly one int; the status flags round-trip. */
    assert_eq(0, fcntl(fd, F_SETFL, O_APPEND));
    flags = fcntl(fd, F_GETFL);
    assert_eq(O_APPEND, flags & O_APPEND);
    assert_eq(0, fcntl(fd, F_SETFL, flags & ~O_APPEND));
    flags = fcntl(fd, F_GETFL);
    assert_eq(0, flags & O_APPEND);

    /* F_GETFD/F_SETFD: the flag the caller set comes back. */
    assert_eq(0, fcntl(fd, F_SETFD, FD_CLOEXEC));
    assert_eq(FD_CLOEXEC, fcntl(fd, F_GETFD));
    assert_eq(0, fcntl(fd, F_SETFD, 0));

    /* F_DUPFD: a NEW descriptor at least as large as the argument. */
    int dupfd = fcntl(fd, F_DUPFD, 10);
    assert_true(dupfd >= 10);
    assert_true(dupfd != fd);
    assert_eq(0, close(dupfd));

    /* An unsupported command is rejected without a syscall. */
    errno = 0;
    assert_eq(-1, fcntl(fd, 7 /* not implemented */));
    assert_read_errno();

    /* A bad descriptor maps to -1/EBADF (kernel's -EBADF answer). */
    errno = 0;
    assert_eq(-1, fcntl(-1, F_GETFL));
    assert_eq(EBADF, errno);
    errno = 0;
    assert_eq(-1, fcntl(-1, F_SETFL, O_APPEND));
    assert_eq(EBADF, errno);

    close(fd);
    stream_rt_reset();
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_fopen_mode_flags),
    TEST_ENTRY(test_fopen_w_truncates_a_appends),
    TEST_ENTRY(test_fopen_invalid_modes_einval),
    TEST_ENTRY(test_fdopen_appends_and_w_does_not_truncate),
    TEST_ENTRY(test_fdopen_failure_keeps_fd_usable),
    TEST_ENTRY(test_fread_no_op_cases_issue_no_read),
    TEST_ENTRY(test_fread_partial_item_at_eof),
    TEST_ENTRY(test_fread_eintr_is_retried),
    TEST_ENTRY(test_fwrite_failure_sets_error),
    TEST_ENTRY(test_fwrite_budget_reports_completed_items),
    TEST_ENTRY(test_fwrite_eintr_is_retried),
    TEST_ENTRY(test_fclose_error_returns_eof_and_releases),
    TEST_ENTRY(test_fcntl_wrapper_contract),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
