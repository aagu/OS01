/*
 * test/cases/test_libc_fread_fwrite_validate.c — fread/fwrite stream
 * validation contract (spec AAGU-4 §3.5 + cleanup plan Task 1).
 *
 * Strategy: open a temp file via the real open() (GLIBC) and populate it
 * via pwrite() (also GLIBC, not overridden by stdio_test_shims). Build a
 * `mini_file_t fake` whose `.fd` is this file BUT do NOT register it via
 * fopen/fdopen.
 *
 * Pre-fix fread dereferences fake->fd and reads the byte (returns 1);
 * pre-fix fwrite dereferences fake->fd and the shim's write returns count
 * (so fwrite returns 1). Post-fix both reject unregistered streams and
 * return 0.
 *
 * We use pwrite + a regular file rather than pipe() + write() because:
 *   - stdio_test_shims.c overrides write() and only forwards to the real
 *     syscall for fd 1/2. For pipe fds the shim just records metadata
 *     and returns count without writing — leaving the pipe empty, so
 *     fread would EOF-return 0 even pre-fix (false GREEN).
 *   - pwrite is NOT overridden by the shim, so the file actually gets
 *     populated, giving us a real RED pre-fix.
 */
#define _GNU_SOURCE
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>

/* OS01's <unistd.h> doesn't declare pwrite; pull it from host. */
extern ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);
extern off_t   lseek(int fd, off_t offset, int whence);

TEST_FUNC(test_fread_unregistered_stream_returns_zero) {
    /* Pre-fix: fread dereferences fake->fd, reads 1 byte from file, returns 1.
     * Post-fix: fread checks is_open_file(fake), not registered, returns 0. */
    char tmpl[] = "/tmp/test_fread_validate_XXXXXX";
    int fd = mkstemp(tmpl);
    assert_true(fd >= 0);

    /* Populate the file via pwrite (bypasses the host shim's write override). */
    ssize_t w = pwrite(fd, "x", 1, 0);
    assert_eq(w, 1);
    off_t off = lseek(fd, 0, 0);  /* 0 == SEEK_SET */
    assert_eq(off, 0);

    mini_file_t fake = { .fd = fd, .mode = 0 };
    char buf[4] = {0};
    size_t n = fread(buf, 1, sizeof buf, &fake);
    /* Post-fix expectation: unregistered stream → 0 bytes read. */
    assert_eq(n, 0);

    close(fd);
    unlink(tmpl);
}

TEST_FUNC(test_fwrite_unregistered_stream_returns_zero) {
    /* Pre-fix: fwrite calls write(mf->fd, ...). For an unregistered stream
     * the shim still returns count → fwrite returns 1.
     * Post-fix: fwrite checks is_open_file, returns 0.
     * The shim never forwards non-1/2 fds to the kernel, so no SIGPIPE
     * risk; the read end is irrelevant since this is a regular file. */
    char tmpl[] = "/tmp/test_fwrite_validate_XXXXXX";
    int fd = mkstemp(tmpl);
    assert_true(fd >= 0);

    mini_file_t fake = { .fd = fd, .mode = 1 };
    size_t n = fwrite("x", 1, 1, &fake);
    assert_eq(n, 0);    /* post-fix: not registered */

    close(fd);
    unlink(tmpl);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_fread_unregistered_stream_returns_zero),
    TEST_ENTRY(test_fwrite_unregistered_stream_returns_zero),
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
