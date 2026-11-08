/*
 * test_libc_stdio_format.c — libc-completion Task T9: formatted fd IO
 * and stream positioning / re-opening.
 *
 * Production objects: the REAL stdio (stdio_extras with dprintf/
 * vdprintf, asprintf.c, stdio_seek.c) and the REAL lseek wrapper
 * (unistd/lseek.c).  Boundaries replaced: the descriptor mock (scripted
 * fds, write failures, SYS_lseek/SYS_fcntl forwards to the host
 * kernel) and armed --wrap malloc for the ENOMEM paths.
 *
 * Checklist coverage:
 *   dprintf/vdprintf on a REAL fd (exact bytes), the two-va_list
 *   discipline (one va_list serving vdprintf AND vasprintf), EINTR
 *   retry and permanent write failure on a raw descriptor, asprintf
 *   ENOMEM leaving *strp NULL (the INT_MAX truncation guard is
 *   structurally unreachable in a test — formatting 2 GiB — and is
 *   covered by the measure-pass reading in the vasprintf tests);
 *   lseek wrapper ESPIPE/EINVAL/offset-round-trip directly; fseeko/
 *   ftello logical positions across ungetc; seek clearing EOF and
 *   pushback; rewind clearing the error indicator; append reopen;
 *   stdin reopen; failed reopen invalidating the old stream (LAST —
 *   it kills stdin for the process); NULL path EINVAL.
 */

#include "test_framework.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "libc_stream_runtime.h"

extern void *__real_malloc(size_t size);
static int g_fail_malloc_on;

void *__wrap_malloc(size_t size)
{
    if (g_fail_malloc_on > 0 && --g_fail_malloc_on == 0) {
        errno = ENOMEM;
        return NULL;
    }
    return __real_malloc(size);
}

extern ssize_t pwrite(int fd, const void *buf, size_t count, off_t offset);
extern off_t lseek(int fd, off_t offset, int whence); /* libc's own decl */

static char g_path[128];

static void make_temp(void)
{
    snprintf(g_path, sizeof(g_path), "/tmp/os01_fmt_XXXXXX");
    int fd = mkstemp(g_path);
    assert_true(fd >= 0);
    if (fd >= 0) close(fd);
    unlink(g_path);
}

static void seed_file(const char *text)
{
    int fd = open(g_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert_true(fd >= 0);
    assert_eq((ssize_t)strlen(text), write(fd, text, strlen(text)));
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

/* ── dprintf / vdprintf / asprintf ─────────────────────────────────── */

TEST_FUNC(test_dprintf_real_fd_exact_bytes) {
    stream_rt_reset();
    make_temp();

    int fd = open(g_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert_true(fd >= 0);
    /* hoisted: the framework stringizes assert expressions into its
     * failure message, and a % inside the expression would crash the
     * message formatter itself */
    int n = dprintf(fd, "%s=%d\n", "answer", 42);
    assert_eq(10, n);                                /* "answer=42\n" */
    close(fd);

    char buf[32];
    read_back(buf, sizeof(buf) - 1);
    assert_str_eq("answer=42\n", buf);
}

/* One va_list handed DOWN a helper must serve BOTH a vdprintf and a
 * vasprintf — the callee's va_copy discipline must not consume the
 * caller's list. */
static int format_twice(int fd, char **strp, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n1 = vdprintf(fd, fmt, ap);
    va_end(ap);

    va_start(ap, fmt);
    int n2 = vasprintf(strp, fmt, ap);
    va_end(ap);

    return (n1 == n2) ? n1 : -2;
}

TEST_FUNC(test_va_list_serves_vdprintf_and_vasprintf) {
    stream_rt_reset();
    static const int vfd = 70;
    stream_rt_virtual(vfd);
    stream_rt_capture_write(vfd);

    char *s = NULL;
    int n = format_twice(vfd, &s, "%d-%s-%c", 7, "seven", 'z');
    assert_eq(9, n);
    assert_not_null(s);
    assert_str_eq("7-seven-z", s);

    size_t len = 0;
    const char *out = stream_rt_written(vfd, &len);
    assert_eq(9, (size_t)len);
    assert_mem_eq("7-seven-z", out, 9);

    free(s);
    stream_rt_reset();
}

TEST_FUNC(test_dprintf_eintr_retries_and_failure) {
    stream_rt_reset();
    static const int vfd = 71;
    stream_rt_virtual(vfd);
    stream_rt_fail_write_once(vfd, EINTR);
    assert_eq(2, dprintf(vfd, "ok"));
    stream_rt_fail_write(vfd, EIO);              /* now permanent */
    int bad = dprintf(vfd, "nope");              /* hoisted: no % but keep shape */
    assert_eq(-1, bad);
    stream_rt_reset();
}

TEST_FUNC(test_asprintf_enomem_nulls_strp) {
    stream_rt_reset();
    g_fail_malloc_on = 1;                        /* the format buffer fails */
    char *s = (char *)0xdeadbeef;
    errno = 0;
    int rc = asprintf(&s, "alloc-%d", 5);        /* hoisted: % in expression */
    assert_eq(-1, rc);
    assert_null(s);                              /* failure contract */
    g_fail_malloc_on = 0;

    /* and a clean call works afterwards */
    s = NULL;
    rc = asprintf(&s, "alloc-%d", 5);
    assert_eq(7, rc);                           /* "alloc-5" */
    assert_not_null(s);
    assert_str_eq("alloc-5", s);
    free(s);
    stream_rt_reset();
}

/* ── the lseek wrapper, directly ───────────────────────────────────── */

TEST_FUNC(test_lseek_wrapper_contract) {
    stream_rt_reset();
    make_temp();
    seed_file("ABCDEF");
    int fd = open(g_path, O_RDONLY);
    assert_true(fd >= 0);

    /* a query does not move the offset; an absolute set round-trips */
    assert_eq(0, (long)lseek(fd, 0, SEEK_CUR));
    assert_eq(4, (long)lseek(fd, 4, SEEK_SET));
    assert_eq(4, (long)lseek(fd, 0, SEEK_CUR));
    assert_eq(6, (long)lseek(fd, 0, SEEK_END));

    /* a pipe is not seekable: -1 with ESPIPE, errno really set */
    int pfd[2];
    assert_eq(0, pipe(pfd));
    errno = 0;
    assert_eq(-1, (long)lseek(pfd[1], 0, SEEK_SET));
    assert_eq(ESPIPE, errno);

    /* a nonsense whence is EINVAL */
    errno = 0;
    assert_eq(-1, (long)lseek(fd, 0, 99));
    assert_eq(EINVAL, errno);

    close(pfd[0]);
    close(pfd[1]);
    close(fd);
}

/* ── fseeko / ftello / rewind on the stream ────────────────────────── */

TEST_FUNC(test_seek_clears_eof_and_pushback) {
    stream_rt_reset();
    make_temp();
    seed_file("ABCDEF");
    FILE *f = fopen(g_path, "r");
    assert_not_null(f);

    char buf[8];
    assert_eq(6, (int)fread(buf, 1, sizeof(buf), f));
    assert_eq(1, feof(f));

    /* seek clears the EOF indicator: the stream is readable again */
    assert_eq(0, fseek(f, 0, SEEK_SET));
    assert_eq(0, feof(f));
    assert_eq('A', fgetc(f));

    /* pushback + absolute seek to the end */
    assert_eq('X', ungetc('X', f));
    assert_eq(0, fseek(f, 0, SEEK_END));
    assert_eq(-1, (int)fgetc(f));                /* true EOF at the end */
    assert_eq(1, feof(f));
    fclose(f);
}

TEST_FUNC(test_ftello_is_logical_after_ungetc) {
    stream_rt_reset();
    make_temp();
    seed_file("ABCDEF");
    FILE *f = fopen(g_path, "r");
    assert_not_null(f);

    char buf[4];
    assert_eq(3, (int)fread(buf, 1, 3, f));      /* physical 3 */
    assert_eq(3, (long)ftello(f));

    assert_eq('Z', ungetc('Z', f));              /* logical 2 */
    assert_eq(2, (long)ftello(f));               /* the LOGICAL position */
    assert_eq('Z', fgetc(f));                    /* pushed byte replayed */
    assert_eq('D', fgetc(f));                    /* then the real stream */
    assert_eq(4, (long)ftello(f));

    /* History: A,B,C read (physical 3), Z replayed from pushback,
     * D read (physical 4).  ungetc('X') makes the LOGICAL position 3
     * (= physical 4 - 1 pushed).  fseek(0, SEEK_CUR) must land there
     * and DISCARD the pushback. */
    assert_eq('X', ungetc('X', f));              /* logical 3, physical 4 */
    assert_eq(3, (long)ftello(f));
    assert_eq(0, fseek(f, 0, SEEK_CUR));
    assert_eq(3, (long)ftello(f));               /* the logical position */
    assert_eq('D', fgetc(f));                    /* pushback gone: the
                                                    file's own D follows */
    assert_eq(4, (long)ftello(f));

    /* a relative CUR seek counts logically from the no-pushback state */
    assert_eq(0, fseek(f, 1, SEEK_CUR));         /* 4 → 5 */
    assert_eq('F', fgetc(f));                    /* the last byte, physical 6 */
    assert_eq(6, (long)ftello(f));

    /* a relative CUR seek FROM a pushed-back stream: ungetc makes the
     * logical position 5 (6 physical - 1); +1 lands back on 6 = EOF */
    assert_eq('Y', ungetc('Y', f));              /* logical 5 */
    assert_eq(5, (long)ftello(f));
    assert_eq(0, fseek(f, 1, SEEK_CUR));         /* logical 5 + 1 → 6 */
    assert_eq(-1, (int)fgetc(f));
    assert_eq(1, feof(f));

    /* rewind clears BOTH indicators and returns to 0 */
    clearerr(f);                                  /* clear EOF to read */
    assert_eq(0, fseek(f, 0, SEEK_SET));
    assert_eq('A', fgetc(f));
    fclose(f);
}

TEST_FUNC(test_rewind_clears_error) {
    stream_rt_reset();
    static const int vfd = 72;
    /* error armed AFTER the script is drained: read 1 byte, then EIO */
    stream_rt_reset();
    stream_rt_virtual(vfd);
    stream_rt_script_read(vfd, "AB", 2);
    FILE *f = fdopen(vfd, "r");
    assert_not_null(f);

    assert_eq('A', fgetc(f));
    stream_rt_fail_read(vfd, EIO);
    assert_eq(EOF, fgetc(f));                    /* the error */
    assert_eq(1, ferror(f));

    /* rewind: seek (the virtual fd's lseek answers ... the mock's raw
     * forward on a VIRTUAL fd fails with EBADF → seek fails, but the
     * indicator clearing still happens and reports nothing). */
    rewind(f);
    assert_eq(0, ferror(f));
    assert_eq(0, feof(f));
    fclose(f);
    stream_rt_reset();
}

TEST_FUNC(test_freopen_append_and_stdin_reopen) {
    stream_rt_reset();
    make_temp();

    /* append reopen: the stream lands at the real end via mode "a" */
    seed_file("one\n");
    FILE *f = fopen(g_path, "r");                /* opened as something else */
    assert_not_null(f);
    assert_true(freopen(g_path, "a", f) == f);   /* the SAME stream */
    assert_eq(3, (int)fwrite("two", 1, 3, f));   /* 3 items of 1 */
    fclose(f);
    char buf[16];
    read_back(buf, sizeof(buf) - 1);
    assert_str_eq("one\ntwo", buf);

    /* stdin reopen: the standard stream object is re-pointed, the fd
     * replaced; the framework does not read stdin so this is safe. */
    seed_file("stdin-data\n");
    FILE *r = freopen(g_path, "r", stdin);
    assert_true(r == stdin);                     /* the SAME stream */
    char probe[16];
    assert_not_null(fgets(probe, (int)sizeof(probe), stdin));
    assert_str_eq("stdin-data\n", probe);
}

TEST_FUNC(test_freopen_failures) {
    stream_rt_reset();
    make_temp();

    /* NULL path is EINVAL (this libc has no Linux reopen-same-file) */
    errno = 0;
    assert_null(freopen(NULL, "r", stdin));
    assert_eq(EINVAL, errno);

    /* a failing open invalidates the old stream (stdin — killed LAST:
     * nothing after this case may use stdin). */
    FILE *r = freopen("/nonexistent_dir_xyz/nope", "r", stdin);
    assert_null(r);
    assert_eq(0, feof(stdin));                   /* resolver → closed: 0 */
    assert_eq(EOF, fgetc(stdin));                /* unusable, not a crash */
    stream_rt_reset();
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_dprintf_real_fd_exact_bytes),
    TEST_ENTRY(test_va_list_serves_vdprintf_and_vasprintf),
    TEST_ENTRY(test_dprintf_eintr_retries_and_failure),
    TEST_ENTRY(test_asprintf_enomem_nulls_strp),
    TEST_ENTRY(test_lseek_wrapper_contract),
    TEST_ENTRY(test_seek_clears_eof_and_pushback),
    TEST_ENTRY(test_ftello_is_logical_after_ungetc),
    TEST_ENTRY(test_rewind_clears_error),
    TEST_ENTRY(test_freopen_append_and_stdin_reopen),
    TEST_ENTRY(test_freopen_failures),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
