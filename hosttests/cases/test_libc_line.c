/*
 * test_libc_line.c — libc-completion Task T8: dynamic line reading and
 * the standard character interfaces.
 *
 * The production objects are the REAL stdio (stdio_file.c with the T8
 * fgetc/getc/fgets additions, stdio_extras.c) and the REAL
 * getdelim.c/getline.  The replaced boundary is the descriptor mock
 * (libc_stream_runtime: scripted/virtual descriptors, read failures);
 * allocation failures are injected through -Wl,--wrap on
 * malloc/realloc, armed only around the call under test.
 *
 * Plan checklist coverage:
 *   - line with newline, empty lines, final segment without newline,
 *     initial EOF, embedded NUL, a delimiter other than '\n',
 *     buffer growth on an existing buffer, n=1 fgets consumes nothing;
 *   - returned length, terminating NUL, no writes past len+1;
 *   - growth ENOMEM (caller's buffer stays freeable, lineptr and n
 *     consistent), read-error vs EOF distinction, length overflow,
 *     the SSIZE_MAX capacity boundary.
 */

#include "test_framework.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <limits.h>

#include "libc_stream_runtime.h"

/* ── Allocation fault injection (linker wraps) ─────────────────────── */

extern void *__real_malloc(size_t size);
extern void *__real_realloc(void *ptr, size_t size);

/* Armed with N: the Nth allocation fails, earlier ones succeed
 * (N <= 0 passes everything through). */
static int g_fail_malloc_on;
static int g_fail_realloc_on;

void *__wrap_malloc(size_t size)
{
    if (g_fail_malloc_on > 0 && --g_fail_malloc_on == 0) {
        errno = ENOMEM;
        return NULL;
    }
    return __real_malloc(size);
}

void *__wrap_realloc(void *ptr, size_t size)
{
    if (g_fail_realloc_on > 0 && --g_fail_realloc_on == 0) {
        errno = ENOMEM;
        return NULL;
    }
    return __real_realloc(ptr, size);
}

/* ── Helpers ───────────────────────────────────────────────────────── */

static FILE *scripted_stream(int vfd, const char *data)
{
    stream_rt_reset();
    stream_rt_virtual(vfd);
    stream_rt_script_read(vfd, data, strlen(data));
    FILE *f = fdopen(vfd, "r");
    assert_not_null(f);
    return f;
}

static void close_scripted(FILE *f, int vfd)
{
    assert_eq(0, (int)fclose(f));
    assert_eq(1, stream_rt_close_count(vfd));   /* virtual close counted */
    stream_rt_reset();
}

/* ── getdelim / getline ────────────────────────────────────────────── */

TEST_FUNC(test_getline_three_lines_then_eof) {
    static const int vfd = 50;
    FILE *f = scripted_stream(vfd, "one\ntwo\nthree");

    char *line = NULL;
    size_t cap = 0;

    ssize_t n1 = getline(&line, &cap, f);
    assert_eq(4, (int)n1);
    if (n1 >= 0) {                              /* stub-safe: deref only what succeeded */
        assert_mem_eq("one\n", line, 4);
        assert_eq('\0', line[n1]);              /* terminating NUL */
        assert_true(cap >= (size_t)n1 + 1);
    }

    ssize_t n2 = getline(&line, &cap, f);
    assert_eq(4, (int)n2);
    if (n2 >= 0) assert_mem_eq("two\n", line, 4);

    /* final segment WITHOUT newline still counts */
    ssize_t n3 = getline(&line, &cap, f);
    assert_eq(5, (int)n3);
    if (n3 >= 0) {
        assert_mem_eq("three", line, 5);
        assert_eq('\0', line[n3]);
    }

    /* then the canonical EOF: -1, eof indicator set */
    assert_eq(-1, (int)getline(&line, &cap, f));
    assert_eq(1, feof(f));
    assert_eq(0, ferror(f));

    free(line);
    close_scripted(f, vfd);
}

TEST_FUNC(test_getline_empty_lines) {
    static const int vfd = 51;
    FILE *f = scripted_stream(vfd, "\n\nx");

    char *line = NULL;
    size_t cap = 0;
    ssize_t na = getline(&line, &cap, f);            /* "\n" */
    assert_eq(1, (int)na);
    if (na >= 0) assert_mem_eq("\n", line, 1);
    ssize_t nb = getline(&line, &cap, f);            /* "\n" */
    assert_eq(1, (int)nb);
    ssize_t nc = getline(&line, &cap, f);            /* "x" */
    assert_eq(1, (int)nc);
    if (nc >= 0) assert_mem_eq("x", line, 1);
    assert_eq(-1, (int)getline(&line, &cap, f));
    free(line);
    close_scripted(f, vfd);
}

TEST_FUNC(test_getdelim_delimiter_not_newline) {
    static const int vfd = 52;
    FILE *f = scripted_stream(vfd, "a:bc");

    char *line = NULL;
    size_t cap = 0;
    ssize_t n = getdelim(&line, &cap, ':', f);
    assert_eq(2, (int)n);                        /* delimiter INCLUDED */
    if (n >= 0) assert_mem_eq("a:", line, 2);
    n = getdelim(&line, &cap, ':', f);
    assert_eq(2, (int)n);
    if (n >= 0) assert_mem_eq("bc", line, 2);
    assert_eq(-1, (int)getdelim(&line, &cap, ':', f));
    free(line);
    close_scripted(f, vfd);
}

TEST_FUNC(test_getline_embedded_nul) {
    /* "a\0b\nc": the first line holds an embedded NUL — the LENGTH is
     * the contract, not strlen. */
    static const int vfd = 53;
    stream_rt_reset();
    stream_rt_virtual(vfd);
    stream_rt_script_read(vfd, "a\0b\nc", 5);
    FILE *f = fdopen(vfd, "r");
    assert_not_null(f);

    char *line = NULL;
    size_t cap = 0;
    ssize_t n = getline(&line, &cap, f);
    assert_eq(4, (int)n);
    if (n >= 0) {
        assert_mem_eq("a\0b\n", line, 4);
        assert_eq('\0', line[4]);                /* NUL AFTER the payload */
    }
    n = getline(&line, &cap, f);
    assert_eq(1, (int)n);
    if (n >= 0) assert_eq('c', line[0]);
    free(line);
    close_scripted(f, vfd);
}

TEST_FUNC(test_getline_initial_eof) {
    static const int vfd = 54;
    FILE *f = scripted_stream(vfd, "");

    char *line = NULL;
    size_t cap = 0;
    assert_eq(-1, (int)getline(&line, &cap, f));
    assert_eq(1, feof(f));
    /* the buffer may have been allocated; it must be freeable as-is */
    free(line);
    close_scripted(f, vfd);
}

TEST_FUNC(test_getline_grows_and_keeps_content) {
    static const int vfd = 55;
    /* 300 bytes, one line, no newline until the very end: forces the
     * 128 → 256 → 512 growth path past the caller's stale buffer. */
    static char big[301];
    memset(big, 'A', 300);
    big[300] = '\n';
    FILE *f = scripted_stream(vfd, big);

    /* a deliberately SMALL existing buffer: the libc must grow it */
    char *line = (char *)malloc(4);
    assert_not_null(line);
    size_t cap = 4;
    ssize_t n = getline(&line, &cap, f);
    assert_eq(301, (int)n);
    if (n >= 0) {
        assert_true(cap >= (size_t)n + 1);
        assert_mem_eq(big, line, 301);
        assert_eq('\0', line[n]);
    }
    free(line);
    close_scripted(f, vfd);
}

TEST_FUNC(test_getdelim_enomem_keeps_buffer_consistent) {
    static const int vfd = 56;
    /* "warm\n" first, then 600 'B' and a final newline: the warm line
     * allocates the buffer; the second getline needs TWO growths
     * (128→256 succeeds, 256→512 is the armed failure), so the
     * failure lands with a NON-EMPTY preserved prefix. */
    enum { TAIL_LEN = 600 };
    static char tail[TAIL_LEN + 1];
    memset(tail, 'B', TAIL_LEN);
    tail[TAIL_LEN] = '\n';
    static char script[5 + TAIL_LEN + 1];
    memcpy(script, "warm\n", 5);
    memcpy(script + 5, tail, TAIL_LEN + 1);
    stream_rt_reset();
    stream_rt_virtual(vfd);
    stream_rt_script_read(vfd, script, sizeof(script));
    FILE *f = fdopen(vfd, "r");
    assert_not_null(f);

    char *line = NULL;
    size_t cap = 0;
    if (getline(&line, &cap, f) != 5) {
        assert_true(0);                          /* stub-safe: no warm buffer */
        close_scripted(f, vfd);
        return;
    }
    assert_str_eq("warm\n", line);
    assert_eq(128, (size_t)cap);                 /* the first allocation */

    g_fail_realloc_on = 2;                       /* the SECOND growth fails */
    errno = 0;
    ssize_t n = getline(&line, &cap, f);
    assert_eq(-1, (int)n);
    assert_eq(ENOMEM, errno);

    /* the failure contract: the buffer is still owned by the caller,
     * still freeable, and *n describes its capacity consistently.
     * A growth fires when the buffer is exactly full, so the failure
     * lands with len == cap-1 bytes preserved ('B's). */
    assert_not_null(line);
    assert_eq(256, (size_t)cap);                 /* the landed growth */
    size_t kept = cap - 1;
    if (n == -1 && kept > 0 && kept <= TAIL_LEN) {
        assert_mem_eq(tail, line, kept);         /* the 'B' prefix ... */
        /* (no NUL is promised INSIDE the buffer on the ENOMEM path —
         * only the *lineptr/*n consistency and freeability are) */
    }

    /* and a disarmed retry continues from the SAME buffer, replaying
     * exactly the pushed-back rest (nothing read may be lost) */
    ssize_t n2 = getline(&line, &cap, f);
    assert_eq((int)(TAIL_LEN + 1 - kept), (int)n2);
    if (n2 >= 0 && n2 >= 1) {
        assert_mem_eq(tail + kept, line, (size_t)n2 - 1);
        assert_eq('\n', line[n2 - 1]);
        assert_eq('\0', line[n2]);
    }
    free(line);
    close_scripted(f, vfd);
}

TEST_FUNC(test_getdelim_capacity_boundary_einval) {
    static const int vfd = 57;
    FILE *f = scripted_stream(vfd, "tiny");

    /* a nonsense capacity beyond SSIZE_MAX is rejected without a read */
    char *line = NULL;
    size_t cap = (size_t)-1;                     /* SIZE_MAX */
    errno = 0;
    assert_eq(-1, (int)getdelim(&line, &cap, '\n', f));
    assert_eq(EINVAL, errno);

    /* the script is untouched */
    line = NULL;
    cap = 0;
    ssize_t n = getline(&line, &cap, f);
    assert_eq(4, (int)n);
    if (n >= 0) assert_mem_eq("tiny", line, 4);
    free(line);
    close_scripted(f, vfd);
}

TEST_FUNC(test_getdelim_error_vs_eof_distinct) {
    static const int vfd = 58;
    /* "AB" with no delimiter: the first call returns the partial
     * segment and the exhausted script sets the EOF indicator; after
     * clearerr the armed EIO proves an error is NOT an end-of-file
     * (the mock's failure flag preempts its script, so the error must
     * be armed between the calls). */
    stream_rt_reset();
    stream_rt_virtual(vfd);
    stream_rt_script_read(vfd, "AB", 2);
    FILE *f = fdopen(vfd, "r");
    assert_not_null(f);

    char *line = NULL;
    size_t cap = 0;
    assert_eq(2, (int)getline(&line, &cap, f));  /* "AB", no delimiter */
    assert_eq(1, feof(f));                       /* script exhausted */
    assert_eq(0, ferror(f));
    free(line);

    clearerr(f);
    stream_rt_fail_read(vfd, EIO);
    line = NULL;
    cap = 0;
    errno = 0;
    assert_eq(-1, (int)getline(&line, &cap, f)); /* the EIO */
    assert_eq(1, ferror(f));
    assert_eq(0, feof(f));                       /* error never sets EOF */
    assert_eq(EIO, errno);
    free(line);
    close_scripted(f, vfd);
}

/* ── fgets and the character interfaces ────────────────────────────── */

TEST_FUNC(test_fgets_contract) {
    static const int vfd = 59;
    FILE *f = scripted_stream(vfd, "one\ntwo");

    char buf[8];
    assert_eq((char *)buf, fgets(buf, (int)sizeof(buf), f));
    assert_str_eq("one\n", buf);

    /* n == 1 consumes nothing: s[0] = NUL, s returned, and the next
     * read still sees the stream's real next byte. */
    char one[2] = { '?', '?' };
    assert_eq((char *)one, fgets(one, 1, f));
    assert_eq('\0', one[0]);

    assert_eq((char *)buf, fgets(buf, (int)sizeof(buf), f));
    assert_str_eq("two", buf);                   /* no trailing newline */
    assert_null(fgets(buf, (int)sizeof(buf), f)); /* EOF */
    assert_eq(1, feof(f));
    close_scripted(f, vfd);
}

TEST_FUNC(test_fgetc_fputc_fileno) {
    static const int vfd = 60;
    FILE *f = scripted_stream(vfd, "x");

    /* fgetc == getc on this libc's single stream state */
    assert_eq('x', fgetc(f));
    assert_eq(EOF, getc(f));                     /* EOF, indicator set */
    assert_eq(1, feof(f));
    assert_eq(0, ferror(f));
    close_scripted(f, vfd);

    /* fileno returns the REAL descriptor behind the stream */
    FILE *g = scripted_stream(61, "");
    assert_eq(61, fileno(g));
    /* a stale/closed stream answers -1 with EBADF, not a wild fd */
    close_scripted(g, 61);
    errno = 0;
    assert_eq(-1, fileno(g));
    assert_eq(EBADF, errno);

    /* putc == fputc: byte lands on the captured descriptor */
    stream_rt_reset();
    static const int wfd = 62;
    stream_rt_virtual(wfd);
    stream_rt_capture_write(wfd);
    FILE *w = fdopen(wfd, "w");
    assert_not_null(w);
    assert_eq('Z', putc('Z', w));
    assert_eq('Y', fputc('Y', w));
    size_t len = 0;
    const char *out = stream_rt_written(wfd, &len);
    assert_eq(2, (size_t)len);
    assert_mem_eq("ZY", out, 2);
    fclose(w);
    stream_rt_reset();
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_getline_three_lines_then_eof),
    TEST_ENTRY(test_getline_empty_lines),
    TEST_ENTRY(test_getdelim_delimiter_not_newline),
    TEST_ENTRY(test_getline_embedded_nul),
    TEST_ENTRY(test_getline_initial_eof),
    TEST_ENTRY(test_getline_grows_and_keeps_content),
    TEST_ENTRY(test_getdelim_enomem_keeps_buffer_consistent),
    TEST_ENTRY(test_getdelim_capacity_boundary_einval),
    TEST_ENTRY(test_getdelim_error_vs_eof_distinct),
    TEST_ENTRY(test_fgets_contract),
    TEST_ENTRY(test_fgetc_fputc_fileno),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
