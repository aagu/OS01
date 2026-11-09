/*
 * test_libc_dir.c — libc-completion Task T15: the directory descriptor
 * and the sorted scan.
 *
 * What is under test: the PRODUCTION sources libc/unistd/dirent.c
 * (opendir/readdir/closedir/dirfd/fdopendir) and libc/stdlib/scandir.c
 * (scandir/alphasort).  What is replaced: the DIRECTORY — the mock
 * hosttests/mock/libc_dir_runtime.c answers open/fstat/getdents64/close
 * over a small fake tree (see that file for what it simulates and what
 * the real kernel does).
 *
 * The properties this group pins:
 *   - opendir() on a regular file answers ENOTDIR;
 *   - fdopendir() TAKES OVER the descriptor: closedir() closes it; on
 *     FAILURE the descriptor is NOT closed and stays the caller's;
 *   - readdir() sets errno when the kernel fails, and leaves errno
 *     UNCHANGED at the end of the directory;
 *   - every kernel record is validated: a length that lies, a length
 *     below the header, or a name that is not NUL-terminated inside the
 *     record answers EIO instead of walking off the buffer;
 *   - d_off is the KERNEL's cookie, copied from the record — not a
 *     running entry counter;
 *   - scandir() honours the filter, keeps the kernel order without a
 *     comparator, sorts with alphasort(), answers 0 for an empty
 *     directory, and hands back INDEPENDENT copies;
 *   - every allocation failure releases the copies, the array and the
 *     directory stream, and never writes a half-finished array into the
 *     caller's pointer.
 *
 * NOTE: assert_eq() stringizes its expression into a printf format, so
 * no call below may contain a '%' in its expression.
 */

#include "test_framework.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../mock/libc_dir_runtime.h"

/* ── the allocation interposer ─────────────────────────────────────────
 *
 * The libc's malloc/realloc/free are replaced through the linker's
 * --wrap so that an allocation failure can be injected at a chosen
 * point.  The interposer also COUNTS the live allocations, which is
 * what makes "the failure path released everything" observable: after a
 * failed scandir() the live count must be exactly what it was before
 * the call.
 */
extern void *__real_malloc(size_t size);
extern void *__real_realloc(void *ptr, size_t size);
extern void __real_free(void *ptr);

static long g_alloc_live;
static long g_alloc_seq;
static long g_alloc_fail_at;   /* 0 = never fail */

void *__wrap_malloc(size_t size)
{
    void *p;

    if (g_alloc_fail_at && ++g_alloc_seq == g_alloc_fail_at)
        return NULL;
    p = __real_malloc(size);
    if (p)
        g_alloc_live++;
    return p;
}

void *__wrap_realloc(void *ptr, size_t size)
{
    void *q;

    if (g_alloc_fail_at && ++g_alloc_seq == g_alloc_fail_at)
        return NULL;           /* the old block is NOT freed */
    q = __real_realloc(ptr, size);
    if (q && !ptr)
        g_alloc_live++;
    return q;
}

void __wrap_free(void *ptr)
{
    if (ptr)
        g_alloc_live--;
    __real_free(ptr);
}

static void alloc_arm(long fail_at)
{
    g_alloc_seq = 0;
    g_alloc_fail_at = fail_at;
}

static void alloc_disarm(void)
{
    g_alloc_fail_at = 0;
}

/* ── the fake tree ───────────────────────────────────────────────────── */

#define DIR_PATH   "/fdir"
#define FILE_PATH  "/ffile"
#define EMPTY_PATH "/fempty"

static void tree_reset(void)
{
    dir_rt_reset();
    dir_rt_add_path(DIR_PATH, 1);
    dir_rt_add_path(FILE_PATH, 0);
    dir_rt_add_path(EMPTY_PATH, 1);
    /* Deliberately NOT in sorted order, so a sorted scan is visible. */
    dir_rt_add_entry(DIR_PATH, "zeta", DT_REG, 100);
    dir_rt_add_entry(DIR_PATH, "alpha", DT_DIR, 200);
    dir_rt_add_entry(DIR_PATH, "mu", DT_LNK, 300);
}

/* ── the cases ───────────────────────────────────────────────────────── */

TEST_FUNC(test_opendir_regular_file_is_enotdir)
{
    DIR *dir;

    tree_reset();
    errno = 0;
    dir = opendir(FILE_PATH);
    assert_null(dir);
    assert_eq(ENOTDIR, errno);
}

TEST_FUNC(test_opendir_lists_entries)
{
    DIR *dir;
    struct dirent *e;

    tree_reset();
    dir = opendir(DIR_PATH);
    assert_not_null(dir);

    e = readdir(dir);
    assert_not_null(e);
    assert_str_eq("zeta", e->d_name);
    assert_eq(DT_REG, e->d_type);

    e = readdir(dir);
    assert_not_null(e);
    assert_str_eq("alpha", e->d_name);
    assert_eq(DT_DIR, e->d_type);

    e = readdir(dir);
    assert_not_null(e);
    assert_str_eq("mu", e->d_name);
    assert_eq(DT_LNK, e->d_type);

    assert_null(readdir(dir));
    assert_eq(0, closedir(dir));
}

TEST_FUNC(test_readdir_eof_keeps_errno)
{
    DIR *dir;

    tree_reset();
    dir = opendir(EMPTY_PATH);
    assert_not_null(dir);

    /* The end of the directory must NOT touch errno. */
    errno = ENOSYS;
    assert_null(readdir(dir));
    assert_eq(ENOSYS, errno);

    assert_eq(0, closedir(dir));
}

TEST_FUNC(test_readdir_error_sets_errno)
{
    DIR *dir;

    tree_reset();
    dir_rt_set_getdents_error(DIR_PATH, EIO);
    dir = opendir(DIR_PATH);
    assert_not_null(dir);

    errno = 0;
    assert_null(readdir(dir));
    assert_eq(EIO, errno);

    assert_eq(0, closedir(dir));
}

TEST_FUNC(test_readdir_bad_record_length_is_eio)
{
    DIR *dir;

    tree_reset();
    /* The record claims more bytes than the kernel wrote. */
    dir_rt_set_bad_record(DIR_PATH, 1);
    dir = opendir(DIR_PATH);
    assert_not_null(dir);

    errno = 0;
    assert_null(readdir(dir));
    assert_eq(EIO, errno);

    assert_eq(0, closedir(dir));
}

TEST_FUNC(test_readdir_short_record_is_eio)
{
    DIR *dir;

    tree_reset();
    /* The record is shorter than the record header. */
    dir_rt_set_bad_record(DIR_PATH, 3);
    dir = opendir(DIR_PATH);
    assert_not_null(dir);

    errno = 0;
    assert_null(readdir(dir));
    assert_eq(EIO, errno);

    assert_eq(0, closedir(dir));
}

TEST_FUNC(test_readdir_unterminated_name_is_eio)
{
    DIR *dir;

    tree_reset();
    /* The name is not NUL-terminated inside the record. */
    dir_rt_set_bad_record(DIR_PATH, 2);
    dir = opendir(DIR_PATH);
    assert_not_null(dir);

    errno = 0;
    assert_null(readdir(dir));
    assert_eq(EIO, errno);

    assert_eq(0, closedir(dir));
}

TEST_FUNC(test_readdir_copies_kernel_d_off)
{
    DIR *dir;
    struct dirent *e;

    tree_reset();
    dir = opendir(DIR_PATH);
    assert_not_null(dir);

    /* The cookie is the KERNEL's: 100, 200, 300 — not 1, 2, 3. */
    e = readdir(dir);
    assert_not_null(e);
    assert_eq(100, (long)e->d_off);
    e = readdir(dir);
    assert_not_null(e);
    assert_eq(200, (long)e->d_off);
    e = readdir(dir);
    assert_not_null(e);
    assert_eq(300, (long)e->d_off);

    assert_eq(0, closedir(dir));
}

TEST_FUNC(test_dirfd_returns_the_descriptor)
{
    DIR *dir;
    int fd;

    tree_reset();
    dir = opendir(DIR_PATH);
    assert_not_null(dir);

    fd = dirfd(dir);
    assert_true(fd >= 0);
    assert_eq(1, dir_rt_fd_is_ours(fd));

    assert_eq(0, closedir(dir));
}

TEST_FUNC(test_fdopendir_success_closes_the_descriptor)
{
    int fd;
    DIR *dir;

    tree_reset();
    fd = open(DIR_PATH, O_RDONLY);
    assert_true(fd >= 0);
    assert_eq(1, dir_rt_fd_is_ours(fd));

    dir = fdopendir(fd);
    assert_not_null(dir);
    assert_eq(fd, dirfd(dir));

    /* The stream owns the descriptor now: closedir() closes it, so the
     * descriptor is really gone afterwards. */
    assert_eq(0, closedir(dir));
    errno = 0;
    assert_eq(-1, close(fd));
    assert_eq(EBADF, errno);
}

TEST_FUNC(test_fdopendir_failure_keeps_the_descriptor)
{
    int fd;
    DIR *dir;

    tree_reset();
    fd = open(FILE_PATH, O_RDONLY);
    assert_true(fd >= 0);

    errno = 0;
    dir = fdopendir(fd);
    assert_null(dir);
    assert_eq(ENOTDIR, errno);

    /* The descriptor stays the CALLER's: it must NOT have been closed, so
     * the caller can still use it. */
    assert_eq(1, dir_rt_fd_is_ours(fd));
    assert_eq(0, close(fd));
}

/* ── scandir ─────────────────────────────────────────────────────────── */

static int filter_beta(const struct dirent *e)
{
    return e->d_name[0] == 'a';
}

TEST_FUNC(test_scandir_uses_the_filter)
{
    struct dirent **list = NULL;
    int n;

    tree_reset();
    n = scandir(DIR_PATH, &list, filter_beta, NULL);
    assert_eq(1, n);
    assert_not_null(list);
    assert_str_eq("alpha", list[0]->d_name);
    assert_null(list[1]);
    free(list[0]);
    free(list);
}

TEST_FUNC(test_scandir_null_comparator_keeps_the_kernel_order)
{
    struct dirent **list = NULL;
    int n;

    tree_reset();
    n = scandir(DIR_PATH, &list, NULL, NULL);
    assert_eq(3, n);
    assert_str_eq("zeta", list[0]->d_name);
    assert_str_eq("alpha", list[1]->d_name);
    assert_str_eq("mu", list[2]->d_name);
    assert_null(list[3]);
    free(list[0]);
    free(list[1]);
    free(list[2]);
    free(list);
}

TEST_FUNC(test_scandir_alphasort_orders_by_name)
{
    struct dirent **list = NULL;
    int n;

    tree_reset();
    n = scandir(DIR_PATH, &list, NULL, alphasort);
    assert_eq(3, n);
    assert_str_eq("alpha", list[0]->d_name);
    assert_str_eq("mu", list[1]->d_name);
    assert_str_eq("zeta", list[2]->d_name);
    assert_null(list[3]);
    free(list[0]);
    free(list[1]);
    free(list[2]);
    free(list);
}

TEST_FUNC(test_scandir_empty_directory)
{
    struct dirent **list = NULL;
    int n;

    tree_reset();
    n = scandir(EMPTY_PATH, &list, NULL, alphasort);
    assert_eq(0, n);
    assert_not_null(list);
    assert_null(list[0]);
    free(list);
}

TEST_FUNC(test_scandir_entries_are_independent_copies)
{
    struct dirent **list = NULL;
    struct dirent **again = NULL;
    int n;

    tree_reset();
    n = scandir(DIR_PATH, &list, NULL, alphasort);
    assert_eq(3, n);

    /* The entries are copies: changing one does not change the next. */
    list[0]->d_name[0] = 'X';
    assert_str_eq("mu", list[1]->d_name);

    /* ... and a second scan is unaffected by the first one's edits. */
    n = scandir(DIR_PATH, &again, NULL, alphasort);
    assert_eq(3, n);
    assert_str_eq("alpha", again[0]->d_name);

    free(list[0]);
    free(list[1]);
    free(list[2]);
    free(list);
    free(again[0]);
    free(again[1]);
    free(again[2]);
    free(again);
}

TEST_FUNC(test_scandir_failure_leaves_the_caller_pointer_untouched)
{
    struct dirent **list = NULL;
    int n;

    tree_reset();
    errno = 0;
    n = scandir(FILE_PATH, &list, NULL, NULL);
    assert_eq(-1, n);
    assert_eq(ENOTDIR, errno);
    /* Nothing was handed out. */
    assert_null(list);
}

TEST_FUNC(test_scandir_allocation_failure_frees_everything)
{
    long fail_at;

    /* Every allocation inside the scan can fail: the DIR, the array and
     * each copy.  The order is DIR, array, copy, array, copy, ... */
    for (fail_at = 1; fail_at <= 5; fail_at++) {
        struct dirent **list = NULL;
        long live_before;
        long live_after;
        int n;

        tree_reset();
        /* The framework has already printed (so stdout's buffer exists);
         * no allocation happens between the measurement and the end of
         * the call, so the live count is a stable witness. */
        fflush(stdout);
        live_before = g_alloc_live;

        alloc_arm(fail_at);
        errno = 0;
        n = scandir(DIR_PATH, &list, NULL, alphasort);
        alloc_disarm();

        assert_eq(-1, n);
        assert_eq(ENOMEM, errno);
        /* No half-finished array was written to the caller. */
        assert_null(list);
        live_after = g_alloc_live;
        assert_eq(live_before, live_after);
    }
}

TEST_LIST_BEGIN
TEST_ENTRY(test_opendir_regular_file_is_enotdir),
TEST_ENTRY(test_opendir_lists_entries),
TEST_ENTRY(test_readdir_eof_keeps_errno),
TEST_ENTRY(test_readdir_error_sets_errno),
TEST_ENTRY(test_readdir_bad_record_length_is_eio),
TEST_ENTRY(test_readdir_short_record_is_eio),
TEST_ENTRY(test_readdir_unterminated_name_is_eio),
TEST_ENTRY(test_readdir_copies_kernel_d_off),
TEST_ENTRY(test_dirfd_returns_the_descriptor),
TEST_ENTRY(test_fdopendir_success_closes_the_descriptor),
TEST_ENTRY(test_fdopendir_failure_keeps_the_descriptor),
TEST_ENTRY(test_scandir_uses_the_filter),
TEST_ENTRY(test_scandir_null_comparator_keeps_the_kernel_order),
TEST_ENTRY(test_scandir_alphasort_orders_by_name),
TEST_ENTRY(test_scandir_empty_directory),
TEST_ENTRY(test_scandir_entries_are_independent_copies),
TEST_ENTRY(test_scandir_failure_leaves_the_caller_pointer_untouched),
TEST_ENTRY(test_scandir_allocation_failure_frees_everything),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
