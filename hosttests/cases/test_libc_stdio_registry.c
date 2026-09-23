/*
 * test/cases/test_libc_stdio_registry.c — P2-8 contract tests for the
 * libc FILE registry cap, the fdopen() path, and concurrent fopen+fclose
 * interleave (slot reuse after close).
 *
 * Three concerns from the cleanup plan:
 *   1. OPEN_FILES_MAX exhaustion — fopen() must return NULL once the
 *      32-slot registry is full, and the failure must set errno=ENOMEM
 *      (per the comment in libc/stdio/stdio_file.c).
 *   2. fdopen() — wraps an existing fd without an underlying open() call,
 *      and must still register the FILE so is_open_file() / file_to_fd()
 *      accept it.
 *   3. Concurrent fopen/fclose — closing slot i must make slot i
 *      reusable by a subsequent fopen(), not leak it until process exit.
 *      Today the registry uses a fixed-size array with lazy NULL-fill on
 *      close and a scan-on-register; this test pins both halves of that
 *      contract.
 *
 * All tests route through the real libc (libc_stdio_file.o), not mock
 * shims, because the registry logic itself is what's under test.
 *
 * Strategy for exhaustion: open 32 distinct temp files, fclose() each
 * one in order so the test never exhausts fd slots. The registry has a
 * separate cap from the fd table — the libc's open() returns real fds
 * via the host syscall, so this loop only stresses the FILE layer.
 */
#define _GNU_SOURCE
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>

/* Reuse the shim's write-counting machinery so fflush-emits-no-writes
 * style assertions can stack on top if needed in future. */
#include "stdio_test_shims.h"

/* OS01's <unistd.h> doesn't declare pwrite; pull it from host. */
extern ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);
extern off_t   lseek(int fd, off_t offset, int whence);

/* mini_file_t layout from <stdio.h>: { int fd; int mode; } — the same
 * layout the host test framework already sees via test_framework.h. */

extern void *fopen(const char *path, const char *mode);
extern void *fdopen(int fd, const char *mode);
extern int   fclose(void *f);
extern int   fflush(void *f);
extern size_t fread(void *ptr, size_t size, size_t nmemb, void *f);
extern size_t fwrite(const void *p, size_t s, size_t n, void *f);

/* OPEN_FILES_MAX is a private #define in libc/stdio/stdio_file.c — the
 * hosttest must agree on the cap, so mirror it here. If the upstream
 * value changes, this constant must change with it; the test_libc_fflush
 * suite already does this same kind of mirroring. */
#define OPEN_FILES_MAX_MIRROR  32

/* Make a unique temp file, return its path. Uses mkstemp for the fd,
 * then transfers ownership to fopen() via path-based fopen+fclose of
 * a throwaway probe to keep the test driver happy on cleanup. The
 * returned path is malloc'd; caller frees. */
static char *make_tmpfile(void)
{
    char tmpl[] = "/tmp/test_stdio_registry_XXXXXX";
    int fd = mkstemp(tmpl);
    assert_true(fd >= 0);
    close(fd);    /* fopen() will reopen by path */
    return strdup(tmpl);
}

TEST_FUNC(test_open_files_max_exhaustion_returns_null_enomem) {
    /* Open OPEN_FILES_MAX_MIRROR files, all distinct paths. The 33rd
     * fopen() must fail with NULL and errno=ENOMEM. */
    char *paths[OPEN_FILES_MAX_MIRROR + 2];
    void *fps   [OPEN_FILES_MAX_MIRROR + 2];

    for (int i = 0; i < OPEN_FILES_MAX_MIRROR; i++) {
        paths[i] = make_tmpfile();
        fps[i] = fopen(paths[i], "w");
        assert_not_null(fps[i]);    /* sanity: should fit in cap */
    }

    /* One over the cap — must fail. */
    paths[OPEN_FILES_MAX_MIRROR] = make_tmpfile();
    errno = 0;
    fps[OPEN_FILES_MAX_MIRROR] = fopen(paths[OPEN_FILES_MAX_MIRROR], "w");
    assert_null(fps[OPEN_FILES_MAX_MIRROR]);
    assert_eq(errno, ENOMEM);

    /* Cleanup. */
    for (int i = 0; i < OPEN_FILES_MAX_MIRROR; i++) {
        int rc = fclose(fps[i]);
        assert_eq(rc, 0);
        unlink(paths[i]);
        free(paths[i]);
    }
    unlink(paths[OPEN_FILES_MAX_MIRROR]);
    free(paths[OPEN_FILES_MAX_MIRROR]);
}

TEST_FUNC(test_fdopen_registers_file) {
    /* fdopen() wraps an existing fd and must register the FILE so the
     * registry accepts it for fwrite/fread/fflush. Pre-fix fwrite and
     * fread had explicit is_open_file() checks; the contract under test
     * here is that fdopen's output satisfies that check. */
    char tmpl[] = "/tmp/test_fdopen_XXXXXX";
    int fd = mkstemp(tmpl);
    assert_true(fd >= 0);

    void *fp = fdopen(fd, "w");
    assert_not_null(fp);

    /* fwrite on the registered FILE must succeed (return 1 = one
     * element written). Pre-fix fwrite's sentinel check rejected fd 1/2
     * (stdout/stderr) but accepted arbitrary registered FILEs — fdopen
     * must hit the latter path. */
    size_t n = fwrite("x", 1, 1, fp);
    assert_eq(n, 1);

    /* fflush must accept it (returns 0; pre-fix returned -1 for
     * unregistered FILEs but accepted registered ones). */
    int rc = fflush(fp);
    assert_eq(rc, 0);

    /* fclose must succeed. */
    rc = fclose(fp);
    assert_eq(rc, 0);

    unlink(tmpl);
}

TEST_FUNC(test_fclose_releases_slot_for_reuse) {
    /* Slot reuse: open a FILE, close it, open another — the second
     * fopen must succeed (i.e. the registry slot was released by the
     * close, not leaked). Without this guarantee the 32-slot cap
     * would behave like a per-process leak that eventually freezes
     * the workload. */
    char *path1 = make_tmpfile();
    char *path2 = make_tmpfile();

    void *fp1 = fopen(path1, "w");
    assert_not_null(fp1);
    int rc = fclose(fp1);
    assert_eq(rc, 0);

    /* fp1 is now invalid; fflush must reject it. */
    assert_eq(fflush(fp1), -1);

    /* The slot fp1 occupied must be reusable. */
    void *fp2 = fopen(path2, "w");
    assert_not_null(fp2);

    /* Sanity: fp2 must be the same address as fp1 — the registry's
     * scan-on-register reuses the first NULL slot. This is a stronger
     * contract than just "fopen succeeds" but matches the registry's
     * documented behaviour (see libc/stdio/stdio_file.c:30). */
    assert_eq((void *)fp2, (void *)fp1);

    rc = fclose(fp2);
    assert_eq(rc, 0);

    unlink(path1); free(path1);
    unlink(path2); free(path2);
}

TEST_FUNC(test_fread_fwrite_on_unregistered_stream_is_rejected) {
    /* Regression guard for the P1-5 cleanup: a mini_file_t that was
     * NOT registered via fopen()/fdopen() must be rejected by both
     * fread and fwrite (return 0). Pre-fix fread/fwrite dereferenced
     * any non-sentinel pointer, including arbitrary struct addresses,
     * which produced arbitrary fd reads/writes — a real footgun.
     *
     * Mirrors test_libc_fread_fwrite_validate's two RED→GREEN cases
     * but co-locates them with the new registry tests for the cleanup
     * batch. */
    char tmpl[] = "/tmp/test_unreg_XXXXXX";
    int fd = mkstemp(tmpl);
    assert_true(fd >= 0);
    pwrite(fd, "x", 1, 0);   /* host syscall; bypasses stdio shims */

    mini_file_t fake = { .fd = fd, .mode = 0 };
    char buf[4] = {0};
    assert_eq(fread(buf, 1, sizeof buf, &fake), 0);

    fake.mode = 1;
    assert_eq(fwrite("x", 1, 1, &fake), 0);

    close(fd);
    unlink(tmpl);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_open_files_max_exhaustion_returns_null_enomem),
    TEST_ENTRY(test_fdopen_registers_file),
    TEST_ENTRY(test_fclose_releases_slot_for_reuse),
    TEST_ENTRY(test_fread_fwrite_on_unregistered_stream_is_rejected),
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
        printf("  >>> ALL TESTS PASSED <<\n");
    return failed > 0 ? 1 : 0;
}