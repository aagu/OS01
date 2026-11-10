#include "systest.h"

// ── 0: putchar ─────────────────────────────────────────────
void test_putchar(void)
{
    int64_t ret = syscall(SYS_putchar, (uint64_t)'X', 0, 0);
    CHECK3(ret >= 0, "putchar", "no crash");
}


// ── 1: write ───────────────────────────────────────────────
void test_write(void)
{
    const char *msg = "systest write\n";
    int64_t ret = write(1, msg, strlen(msg));
    CHECK3(ret == (int64_t)strlen(msg), "write", "fd=1 correct len");
    ret = write(999, msg, 5);
    CHECK3(ret == -1 && errno == EBADF, "write", "fd=999 EBADF");
}


// ── 35: uname ──────────────────────────────────────────────
void test_uname(void)
{
    struct utsname uts;
    CHECK3(uname(&uts) == 0 && uts.sysname[0], "uname", "sysname set");
}


/* ── Signal handler sync test ──────────────────────────── */
static volatile int sigusr1_got = 0;

static void sigusr1_handler(int sig) { (void)sig; sigusr1_got = 1; }

void test_signal_handler_sync(void)
{
    signal(SIGUSR1, sigusr1_handler);
    kill(getpid(), SIGUSR1);
    if (sigusr1_got)
        PASS("signal handler sync", "handler called by SIGUSR1");
    else
        FAIL("signal handler sync", "handler not called");
    signal(SIGUSR1, SIG_DFL);
}


// ── 66: getrandom(2) + /dev/urandom ─────────────────────────
void test_getrandom(void)
{
    uint8_t a[32], b[32];
    memset(a, 0, 32);
    memset(b, 0, 32);

    // 1. Basic: returns 32, non-zero, two calls differ.
    errno = 0;
    ssize_t ra = getrandom(a, 32, 0);
    CHECK3(ra == 32, "getrandom", "basic returns 32");
    int nz = 0;
    for (int i = 0; i < 32; i++) if (a[i]) nz++;
    CHECK3(nz > 0, "getrandom", "non-zero output");
    ssize_t rb = getrandom(b, 32, 0);
    CHECK3(rb == 32, "getrandom", "second call returns 32");
    CHECK3(memcmp(a, b, 32) != 0, "getrandom", "two calls differ");

    // 2. Flags: both GRND flags (and their OR) accepted; unknown → EINVAL.
    CHECK3(getrandom(a, 32, GRND_NONBLOCK) == 32, "getrandom", "GRND_NONBLOCK ok");
    CHECK3(getrandom(a, 32, GRND_RANDOM) == 32, "getrandom", "GRND_RANDOM ok");
    CHECK3(getrandom(a, 32, GRND_NONBLOCK | GRND_RANDOM) == 32, "getrandom", "both flags ok");
    errno = 0;
    ssize_t rbad = getrandom(a, 32, 0x100);
    CHECK3(rbad == -1 && errno == EINVAL, "getrandom", "bad flag EINVAL");

    // 3. Bad pointer: out-of-range → EFAULT.
    errno = 0;
    ssize_t r0 = getrandom((void *)0xFFFF800000000000ULL, 32, 0);
    CHECK3(r0 == -1 && errno == EFAULT, "getrandom", "out-of-range EFAULT");

    // 3b. Deterministic unmapped page: mmap one page, munmap it, use that VA.
    //     Must be in-range and unmapped → exercises the per-page PTE check.
    //     NOT a COW/MAP_PRIVATE file map (that's a documented -EFAULT deviation).
    void *p = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p != MAP_FAILED) {
        munmap(p, 4096);
        errno = 0;
        ssize_t r1 = getrandom(p, 4096, 0);
        CHECK3(r1 == -1 && errno == EFAULT, "getrandom", "unmapped VA EFAULT");
    } else {
        FAIL("getrandom: mmap for EFAULT test");
    }

    // 4. len=0 → 0 (buf may be NULL).
    errno = 0;
    CHECK3(getrandom(NULL, 0, 0) == 0, "getrandom", "len=0 returns 0");

    // 5. Large len (1 MiB) must not hang (exercises chunked lock release).
    //     Touch the buffer first: anon pages are demand-mapped on first
    //     access; user_write_range_begin requires PTEs to already exist.
    {
        size_t big = 1024 * 1024;
        void *buf = mmap(NULL, big, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK3(buf != MAP_FAILED, "getrandom", "1MiB mmap");
        if (buf != MAP_FAILED) {
            memset(buf, 0, big);   // fault in all pages
            errno = 0;
            ssize_t r = getrandom(buf, big, 0);
            CHECKF(r == (ssize_t)big, "getrandom", "1MiB = %ldB",
                   "1MiB = %ldB", (long)r);
            munmap(buf, big);
        }
    }

    // 6. /dev/urandom: read 16B, two reads differ; unmapped VA → read < 0.
    {
        int fd = open("/dev/urandom", O_RDONLY);
        CHECK3(fd >= 0, "getrandom", "/dev/urandom open");
        if (fd >= 0) {
            uint8_t ua[16], ub[16];
            ssize_t n1 = read(fd, ua, 16);
            ssize_t n2 = read(fd, ub, 16);
            CHECK3(n1 == 16 && n2 == 16, "getrandom", "/dev/urandom read 16");
            CHECK3(memcmp(ua, ub, 16) != 0, "getrandom", "/dev/urandom two reads differ");
            close(fd);
        }
        void *q = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (q != MAP_FAILED) {
            munmap(q, 4096);
            int f2 = open("/dev/urandom", O_RDONLY);
            if (f2 >= 0) {
                errno = 0;
                ssize_t r = read(f2, q, 4096);
                CHECK3(r < 0, "getrandom", "/dev/urandom unmapped VA <0");
                close(f2);
            }
        }
    }

    // 7. Concurrency: fork, child loops 1000×getrandom(32B), parent draws
    //    its own sample; both must not hang and must differ (no cross-process
    //    keystream repeat).  Note: same-mm munmap race is NOT expressible in
    //    current userland (clone→fork, no CLONE_VM); mm->lock correctness is
    //    covered by the mmap/mprotect/fork-mmap/COW suites.
    {
        int pipefd[2];
        if (pipe(pipefd) == 0) {
            int pid = fork();
            if (pid == 0) {
                close(pipefd[0]);
                uint8_t c[32];
                for (int i = 0; i < 1000; i++) getrandom(c, 32, 0);
                getrandom(c, 32, 0);   // fresh sample for comparison
                write(pipefd[1], c, 32);
                close(pipefd[1]);
                _exit(0);
            }
            close(pipefd[1]);
            uint8_t pa[32], ca[32];
            getrandom(pa, 32, 0);
            ssize_t got = read(pipefd[0], ca, 32);
            close(pipefd[0]);
            int st;
            waitpid(pid, &st, 0);
            CHECK3(WIFEXITED(st) && WEXITSTATUS(st) == 0, "getrandom", "fork child 1000 iters");
            CHECK3(got == 32, "getrandom", "pipe received child sample");
            CHECK3(memcmp(pa, ca, 32) != 0, "getrandom", "parent/child first differ");
        }
    }

    // 8. Monobit smoke test (32 KiB, no statistical power — catches only
    //    constant/all-zero/strongly-periodic disasters, not weak RNGs).
    //    Integer math only: OS01's printf has no %f and no %z modifier.
    {
        size_t big = 32768;
        uint8_t *buf = (uint8_t *)malloc(big);
        CHECK3(buf != NULL, "getrandom", "32KiB malloc");
        if (buf) {
            memset(buf, 0, big);   // fault in all heap pages (no demand paging)
            ssize_t r = getrandom(buf, big, 0);
            CHECKF(r == (ssize_t)big, "getrandom", "32KiB returns %ldB",
                   "32KiB got %ldB", (long)r);
            long ones = 0;
            for (size_t i = 0; i < big; i++) {
                uint8_t v = buf[i];
                for (int k = 0; k < 8; k++) ones += (v >> k) & 1;
            }
            long total = (long)big * 8;
            long pct = (ones * 100) / total;   // integer % of 1-bits
            CHECKF(pct > 45 && pct < 55, "getrandom",
                   "monobit 1-bit%% in (45,55)", "monobit 1-bit%% = %ld", pct);
            free(buf);
        }
    }
}


// ── 6b: libc printf float/getopt (printf hardening / getopt compat) ──
static int libc_expect(const char *got, const char *exp)
{
    return strcmp(got, exp) == 0;
}

void test_libc_printf_getopt(void)
{
    char b[128];

    // integer (incl. %llu)
    snprintf(b, sizeof(b), "%llu", 18446744073709551615ULL);
    CHECK3(libc_expect(b, "18446744073709551615"), "printf %llu", b);
    snprintf(b, sizeof(b), "%d-%s", 42, "x");
    CHECK3(libc_expect(b, "42-x"), "printf %d-%s", b);

    // float family
    snprintf(b, sizeof(b), "%.0f", 2.5);       CHECK3(libc_expect(b, "2"),           "printf %.0f", b);
    snprintf(b, sizeof(b), "%f", -0.0);        CHECK3(libc_expect(b, "-0.000000"),  "printf %f -0", b);
    snprintf(b, sizeof(b), "%e", 1.5);        CHECK3(libc_expect(b, "1.500000e+00"),"printf %e", b);
    snprintf(b, sizeof(b), "%g", 1e7);         CHECK3(libc_expect(b, "1e+07"),      "printf %g", b);
    snprintf(b, sizeof(b), "%+.1f", -0.0);    CHECK3(libc_expect(b, "-0.0"),       "printf %+.1f", b);
    snprintf(b, sizeof(b), "%f", __builtin_inf());    CHECK3(libc_expect(b, "inf"), "printf inf", b);
    snprintf(b, sizeof(b), "%e", __builtin_nan(""));  CHECK3(libc_expect(b, "nan"), "printf nan", b);

    // bounded snprintf: output truncated, NUL always present, returns full len
    int r = snprintf(b, 4, "%s", "abcdef");
    CHECK3(r == 6 && b[0] == 'a' && b[1] == 'b' && b[2] == 'c' && b[3] == '\0',
           "snprintf bounded", "ret=6 truncated+NUL");

    // %n assigned only in the render pass (count pass must not touch it)
    int n = -1;
    snprintf(b, sizeof(b), "abc%n", &n);
    CHECK3(n == 3, "printf %n", "n=3 after render");

    // getopt: optind==0 reset + separated argument
    {
        char *argv[] = { "prog", "-a", "-b", "file", NULL };
        int argc = 4;
        optind = 0; /* reset */
        int c, seen_a = 0, seen_b = 0;
        char *barg = NULL;
        while ((c = getopt(argc, argv, "ab:")) != -1) {
            if (c == 'a') seen_a = 1;
            else if (c == 'b') { seen_b = 1; barg = optarg; }
        }
        CHECK3(seen_a && seen_b && barg && strcmp(barg, "file") == 0,
               "getopt -a -b file", "a,b,arg=file");
    }
}


// ── T7: stdio modes on target temp files (libc-completion plan) ──
//
// The mode matrix, exercised through the REAL kernel on a tmpfs
// file: fopen("w") truncates, fopen("a") appends at the REAL end
// (the kernel's T6 atomic append, reached through the libc fopen
// layer), "r+" reads and writes, fdopen("a") turns a plain
// descriptor append-only through F_SETFL and the byte lands at EOF
// even though the descriptor position sits at 0, and a malformed
// mode ("wx" — the close-on-exec/exclusive extensions are not
// promised) is rejected with EINVAL.  EINTR behaviour is host-only
// (regular-file syscalls on this kernel never interrupt).
void test_stdio_modes(void)
{
    static const char *path = "/tmp/systest_stdio.dat";
    char buf[32];
    ssize_t n;
    unlink(path);

    /* fopen "w": create/truncate, write at 0 */
    FILE *f = fopen(path, "w");
    CHECK3(f != NULL, "fopen w", "opened");
    size_t w = fwrite("hello", 1, 5, f);
    CHECK3(w == 5, "fwrite w", "5 bytes");
    fclose(f);

    /* fopen "a": appends at the real end */
    f = fopen(path, "a");
    CHECK3(f != NULL, "fopen a", "opened");
    w = fwrite("WORLD", 1, 5, f);
    CHECK3(w == 5, "fwrite a", "5 bytes");
    fclose(f);

    /* fopen "r": exact read-back */
    memset(buf, 0, sizeof(buf));
    f = fopen(path, "r");
    CHECK3(f != NULL, "fopen r", "opened");
    size_t r = fread(buf, 1, sizeof(buf) - 1, f);
    CHECK3(r == 10 && memcmp(buf, "helloWORLD", 10) == 0,
           "fread r", "content matches");
    fclose(f);

    /* fdopen("a"): a plain descriptor at position 0, the byte must
     * land at the file's REAL end */
    int fd = open(path, O_RDWR);
    CHECK3(fd >= 0, "open fdopen", "descriptor");
    f = fdopen(fd, "a");
    CHECK3(f != NULL, "fdopen a", "opened");
    w = fwrite("!", 1, 1, f);
    CHECK3(w == 1, "fdopen a write", "1 byte at EOF");
    fclose(f);                      /* fdopen owns the fd */

    /* verify the complete file content */
    fd = open(path, O_RDONLY);
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    CHECK3(n == 11 && memcmp(buf, "helloWORLD!", 11) == 0,
           "fdopen a verify", "appended at real end");

    /* malformed mode: the "x" extension is not promised */
    errno = 0;
    f = fopen(path, "wx");
    CHECK3(f == NULL && errno == EINVAL, "fopen wx", "EINVAL");

    /* fopen "w" on an existing file truncates it */
    f = fopen(path, "w");
    CHECK3(f != NULL, "fopen w truncate", "opened");
    fclose(f);
    fd = open(path, O_RDONLY);
    n = read(fd, buf, 1);
    close(fd);
    CHECK3(n == 0, "fopen w truncate", "file empty");
    unlink(path);
}



// ── T8: BusyBox line applets through the libc getline (libc
//    completion plan) ──
//
// getdelim/getline now exist in libc.a and the BusyBox private build
// carries -DHAVE_GETLINE=1 (config/busybox.config.in).  NOTE: busybox
// 1.36.1's own getline call sites sit inside #if 0, so nothing in
// THIS binary references getline today — head/tail extract lines via
// bb_get_chunk_from_file, an fgetc loop.  That is exactly why this
// case still matters: it drives OUR fgetc path through real applets
// over a five-line tmpfs file with stdout redirected to a file, and
// checks the exact extracted lines.
static int busybox_lines_expect(const char **av, const char *out_path,
                                const char *expect, const char *label)
{
    unlink(out_path);
    pid_t pid = fork();
    if (pid == 0) {
        int out = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out < 0) _exit(120);
        if (dup2(out, 1) < 0) _exit(121);
        close(out);
        execve(av[0], (char *const *)av, NULL);
        _exit(127);
    }
    if (pid <= 0) { FAIL("%s: fork", label); return 0; }

    int st = -1;
    if (waitpid(pid, &st, 0) != pid) { FAIL("%s: waitpid", label); return 0; }
    if ((st & 0x7f) != 0 || ((st >> 8) & 0xff) != 0) {
        FAIL("%s: exit status=%d", label, st);
        return 0;
    }

    int fd = open(out_path, O_RDONLY);
    if (fd < 0) { FAIL("%s: no output", label); return 0; }
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 0) n = 0;
    buf[n] = '\0';
    int ok = (strcmp(buf, expect) == 0);
    CHECK3(ok, label, ok ? "exact output" : buf);
    return ok;
}

void test_busybox_lines(void)
{
    static const char *path = "/tmp/systest_bb_lines.txt";
    static const char *out = "/tmp/systest_bb_lines.out";
    unlink(path);

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK3(fd >= 0, "bb lines setup", "file created");
    if (fd < 0) return;
    const char *content = "alpha\nbeta\ngamma\ndelta\nepsilon\n";
    ssize_t wn = write(fd, content, strlen(content));
    close(fd);
    CHECK3(wn == (ssize_t)strlen(content), "bb lines setup", "5 lines seeded");

    char *av_head[5] = {(char *)"/bin/head", (char *)"-n",
                        (char *)"2", (char *)path, NULL};
    if (!busybox_lines_expect(av_head, out, "alpha\nbeta\n", "bb head -n 2")) {
        /* the CHECK3 inside already reported the failure detail */
    }

    char *av_tail[5] = {(char *)"/bin/tail", (char *)"-n",
                        (char *)"2", (char *)path, NULL};
    busybox_lines_expect(av_tail, out, "delta\nepsilon\n", "bb tail -n 2");

    char *av_cat[4] = {(char *)"/bin/cat", (char *)path, NULL};
    busybox_lines_expect(av_cat, out, content, "bb cat");

    unlink(path);
    unlink(out);
}


// ── T10: integer conversions on a uint64 token (libc-completion
//    plan) ──
//
// The strto* family must survive the full uint64 range without
// truncation: a token written to tmpfs, read back as text, and
// converted must reproduce the exact value (UINTMAX_MAX, INTMAX_MIN,
// a base-16 token with the high bits set), and an overflow must
// answer ERANGE with the saturated maximum.  The strtol family was
// previously able to truncate these (strtoull was a truncation cast
// of strtoul) — this case pins the fix on the real kernel.
void test_integer_conversions(void)
{
    static const char *path = "/tmp/systest_inttok.txt";
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    CHECK3(fd >= 0, "inttok setup", "file created");
    if (fd < 0) return;
    const char *content =
        "18446744073709551615\n"
        "-9223372036854775808\n"
        "0xdeadbeefcafebabe\n"
        "99999999999999999999999\n";
    ssize_t wn = write(fd, content, strlen(content));
    close(fd);
    CHECK3(wn == (ssize_t)strlen(content), "inttok setup", "tokens seeded");

    fd = open(path, O_RDONLY);
    if (fd < 0) { FAIL("inttok reopen"); return; }
    char buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) { FAIL("inttok read"); return; }
    buf[n] = '\0';

    errno = 0;
    char *e = NULL;
    uintmax_t u = strtoumax(buf, &e, 10);
    CHECK3(u == UINTMAX_MAX && errno == 0 &&
           (long)(e - buf) == 20, "strtoumax uint64 token",
           "full uint64 range, exact consumption");

    errno = 0;
    intmax_t m = strtoimax(e + 1, &e, 10);
    CHECK3(m == INTMAX_MIN && errno == 0,
           "strtoimax int64 minimum", "INTMAX_MIN exact");

    errno = 0;
    u = strtoumax(e + 1, &e, 16);
    CHECK3(u == 0xdeadbeefcafebabeULL && errno == 0 &&
           (long)(e - buf) == 60, "strtoumax base16 token",
           "high-bit token exact");

    errno = 0;
    u = strtoumax(e + 1, &e, 10);
    CHECK3(u == UINTMAX_MAX && errno == ERANGE, "strtoumax overflow",
           "ERANGE with saturated maximum");

    unlink(path);
}


// ── T13: the calendar core, the POSIX TZ and the formatting on the
//    real kernel (libc-completion plan) ──
//
// The host group proves the conversion against a glibc oracle; this
// case proves that the SAME libc works in the guest: the epoch is a
// Thursday, the broken-down fields are right, the leap year 2000 and
// the non-leap year 2100 differ, mktime normalizes and round-trips,
// timegm is the UTC twin, strftime formats, and the zone follows the
// environment.  The instants are CONSTANTS — nothing here reads a wall
// clock, except the last probe, which only checks that the clock the
// desktop shows is a moving one.
void test_time_conversion(void)
{
    struct tm tm;
    struct tm *p;
    time_t t;

    /* the epoch: 1970-01-01 was a Thursday */
    t = (time_t)0;
    memset(&tm, 0, sizeof tm);
    p = gmtime_r(&t, &tm);
    CHECK3(p != NULL && p->tm_year == 70 && p->tm_mon == 0 && p->tm_mday == 1 &&
           p->tm_hour == 0 && p->tm_min == 0 && p->tm_sec == 0 &&
           p->tm_wday == 4 && p->tm_yday == 0,
           "time epoch", "1970-01-01 is a Thursday");

    /* one second before the epoch */
    t = (time_t)-1;
    memset(&tm, 0, sizeof tm);
    p = gmtime_r(&t, &tm);
    CHECK3(p != NULL && p->tm_year == 69 && p->tm_mon == 11 && p->tm_mday == 31 &&
           p->tm_hour == 23 && p->tm_min == 59 && p->tm_sec == 59,
           "time before epoch", "1969-12-31 23:59:59");

    /* 2000 is a leap year, 2100 is not */
    t = (time_t)951782400;   /* 2000-02-29T00:00:00Z */
    memset(&tm, 0, sizeof tm);
    p = gmtime_r(&t, &tm);
    CHECK3(p != NULL && p->tm_mon == 1 && p->tm_mday == 29 && p->tm_yday == 59,
           "time leap 2000", "2000-02-29 is day 59");

    t = (time_t)4107542400;  /* 2100-03-01T00:00:00Z */
    memset(&tm, 0, sizeof tm);
    p = gmtime_r(&t, &tm);
    CHECK3(p != NULL && p->tm_year == 200 && p->tm_mon == 2 && p->tm_mday == 1 &&
           p->tm_yday == 59,
           "time non-leap 2100", "2100-03-01 is day 59");

    /* after the 32-bit boundary */
    t = (time_t)2147483648;  /* 2038-01-19T03:14:08Z */
    memset(&tm, 0, sizeof tm);
    p = gmtime_r(&t, &tm);
    CHECK3(p != NULL && p->tm_year == 138 && p->tm_mon == 0 && p->tm_mday == 19 &&
           p->tm_hour == 3 && p->tm_min == 14 && p->tm_sec == 8,
           "time after 2038", "2038-01-19T03:14:08Z");

    /* an instant that cannot be represented is EOVERFLOW, not a wrong
     * answer */
    t = (time_t)9223372036854775807LL;
    memset(&tm, 0, sizeof tm);
    errno = 0;
    p = gmtime_r(&t, &tm);
    CHECK3(p == NULL && errno == EOVERFLOW, "time overflow",
           "EOVERFLOW for an unrepresentable instant");

    /* mktime normalizes the broken-down fields.  The zone is pinned to
     * UTC first, so the expectation does not depend on the boot
     * environment. */
    unsetenv("TZ");
    tzset();
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 2024 - 1900;
    tm.tm_mon = 1;          /* February (29 days in 2024) */
    tm.tm_mday = 32;        /* the 3rd of March */
    tm.tm_hour = 12;
    t = mktime(&tm);
    CHECK3(t == (time_t)1709467200 && tm.tm_mon == 2 && tm.tm_mday == 3 &&
           tm.tm_hour == 12,
           "time mktime normalizes", "2024-02-32 is 2024-03-03");

    /* timegm is the UTC twin */
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 2024 - 1900;
    tm.tm_mon = 2;
    tm.tm_mday = 10;
    tm.tm_hour = 7;
    t = timegm(&tm);
    CHECK3(t == (time_t)1710054000, "time timegm",
           "2024-03-10T07:00:00Z");

    /* strftime formats the broken-down time */
    {
        char buf[64];
        size_t n;

        memset(&tm, 0, sizeof tm);
        tm.tm_year = 2021 - 1900;
        tm.tm_mon = 0;
        tm.tm_mday = 3;
        tm.tm_hour = 4;
        tm.tm_min = 5;
        tm.tm_sec = 6;
        tm.tm_wday = 0;
        tm.tm_yday = 2;
        n = strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
        CHECK3(n == 19 && strcmp(buf, "2021-01-03 04:05:06") == 0,
               "time strftime", "2021-01-03 04:05:06");
    }

    /* the zone follows the environment */
    setenv("TZ", "CST-8", 1);
    t = (time_t)1710050400;  /* 2024-03-10T06:00:00Z */
    memset(&tm, 0, sizeof tm);
    p = localtime_r(&t, &tm);
    CHECK3(p != NULL && p->tm_hour == 14 && p->tm_isdst == 0,
           "time zone", "CST-8 is UTC+8");

    /* a malformed zone is UTC, not a leftover */
    setenv("TZ", "not-a-zone", 1);
    memset(&tm, 0, sizeof tm);
    p = localtime_r(&t, &tm);
    CHECK3(p != NULL && p->tm_hour == 6, "time malformed zone",
           "a malformed TZ is UTC");
    unsetenv("TZ");

    /* the wall clock the desktop shows is a MOVING one */
    {
        struct timespec ts1;
        struct timespec ts2;

        if (clock_gettime(CLOCK_REALTIME, &ts1) == 0) {
            int64_t a = (int64_t)ts1.tv_sec * 1000000000LL + (int64_t)ts1.tv_nsec;
            int64_t b;
            int spins = 0;
            do {
                if (clock_gettime(CLOCK_REALTIME, &ts2) != 0) {
                    break;
                }
                b = (int64_t)ts2.tv_sec * 1000000000LL + (int64_t)ts2.tv_nsec;
            } while (b == a && ++spins < 1000);
            CHECK3(b > a, "time wall clock moves",
                   "CLOCK_REALTIME advances");
        } else {
            skip_case("no-clock-gettime");
        }
    }
}


// ── T11: long double precision across processes (libc-completion
//    plan) ──
//
// strtold must preserve MORE significant bits than strtod (the x87
// 80-bit format carries 64 mantissa bits vs 53), deterministically,
// in a fresh process after fork+exec through the scheduler.  The
// expected 80-bit pattern of 3.14159265358979323846264338327950288
// (little-endian: 8 mantissa bytes + the 2 exponent/sign bytes) was
// produced by the T11 host oracle and is hard-coded here — both the
// host glibc and the OS01 floatscan are correctly rounded, so the
// bits must match on the target exactly.  The old strtod could not
// even accept this check: it lost the tail digits to repeated
// multiply/divide, and strtold did not exist.
static const char *fp_ld_expected = "\x35\xc2\x68\x21\xa2\xda\x0f\xc9\x00\x40";

void test_float_precision(void)
{
    const char *s = "3.14159265358979323846264338327950288";

    /* the long double result must differ from the double one (extra
     * precision survived the parse) */
    long double ld = strtold(s, NULL);
    double d = strtod(s, NULL);
    long double dw = (long double)d;
    CHECK3(ld != dw, "float ld extra precision",
           "long double carries more bits than double");
    CHECK3((ld - dw) > 0 && (ld - dw) < 1e-15L, "float ld sane delta",
           "difference at the double-precision scale");

    /* the exact 80-bit pattern, in-process */
    CHECK3(memcmp(&ld, fp_ld_expected, 10) == 0, "float ld pattern",
           "x87 80-bit pattern exact");

    /* the same parse in a fresh process after fork (scheduler +
     * syscall path), verified against the parent's pattern */
    pid_t pid = fork();
    if (pid == 0) {
        long double c = strtold(s, NULL);
        _exit(memcmp(&c, fp_ld_expected, 10) == 0 ? 0 : 1);
    }
    int st = -1;
    int64_t w = waitpid(pid, &st, 0);
    CHECK3(w == pid && st == 0, "float ld fresh process",
           "child parse bit-identical");
}

void test_linux_abi_compat(void)
{
    static const uint8_t linux_probe_code[] = {
        0x48, 0xc7, 0xc0, 0x27, 0x00, 0x00, 0x00,
        0xcd, 0x80,
        0x48, 0x85, 0xc0,
        0x7e, 0x2b,
        0x48, 0xc7, 0xc0, 0xe7, 0x03, 0x00, 0x00,
        0xcd, 0x80,
        0x48, 0x83, 0xf8, 0xda,
        0x75, 0x2d,
        0x48, 0xc7, 0xc0, 0x19, 0x00, 0x00, 0x00,
        0xcd, 0x80,
        0x48, 0x83, 0xf8, 0xda,
        0x75, 0x2f,
        0x48, 0xc7, 0xc0, 0x3c, 0x00, 0x00, 0x00,
        0x48, 0x31, 0xff,
        0xcd, 0x80,
        0xf4,
        0x48, 0xc7, 0xc0, 0x3c, 0x00, 0x00, 0x00,
        0x48, 0xc7, 0xc7, 0x01, 0x00, 0x00, 0x00,
        0xcd, 0x80,
        0xf4,
        0x48, 0xc7, 0xc0, 0x3c, 0x00, 0x00, 0x00,
        0x48, 0xc7, 0xc7, 0x02, 0x00, 0x00, 0x00,
        0xcd, 0x80,
        0xf4,
        0x48, 0xc7, 0xc0, 0x3c, 0x00, 0x00, 0x00,
        0x48, 0xc7, 0xc7, 0x03, 0x00, 0x00, 0x00,
        0xcd, 0x80,
        0xf4,
    };

    uint8_t elf_img[0x1000 + sizeof(linux_probe_code)];
    memset(elf_img, 0, sizeof(elf_img));

    uint8_t *p = elf_img;
    p[0] = 0x7f; p[1] = 'E'; p[2] = 'L'; p[3] = 'F';
    p[4] = 2; p[5] = 1; p[6] = 1;
    *(uint16_t *)(p + 16) = 2;
    *(uint16_t *)(p + 18) = 0x3e;
    *(uint32_t *)(p + 20) = 1;
    *(uint64_t *)(p + 24) = 0x400000;
    *(uint64_t *)(p + 32) = 64;
    *(uint16_t *)(p + 52) = 64;
    *(uint16_t *)(p + 54) = 56;
    *(uint16_t *)(p + 56) = 2;

    uint8_t *ph0 = elf_img + 64;
    *(uint32_t *)(ph0 + 0)  = 1;
    *(uint32_t *)(ph0 + 4)  = 5;
    *(uint64_t *)(ph0 + 8)  = 0x1000;
    *(uint64_t *)(ph0 + 16) = 0x400000;
    *(uint64_t *)(ph0 + 24) = 0x400000;
    *(uint64_t *)(ph0 + 32) = sizeof(linux_probe_code);
    *(uint64_t *)(ph0 + 40) = sizeof(linux_probe_code);
    *(uint64_t *)(ph0 + 48) = 0x1000;

    uint8_t *ph1 = elf_img + 120;
    *(uint32_t *)(ph1 + 0)  = 4;
    *(uint32_t *)(ph1 + 4)  = 4;
    *(uint64_t *)(ph1 + 8)  = 0x200;
    *(uint64_t *)(ph1 + 32) = 32;
    *(uint64_t *)(ph1 + 40) = 32;
    *(uint64_t *)(ph1 + 48) = 4;

    uint8_t *note = elf_img + 0x200;
    *(uint32_t *)(note + 0) = 4;
    *(uint32_t *)(note + 4) = 16;
    *(uint32_t *)(note + 8) = 1;
    memcpy(note + 12, "GNU\0", 4);
    *(uint32_t *)(note + 16) = 0;
    *(uint32_t *)(note + 20) = 4;
    *(uint32_t *)(note + 24) = 4;
    *(uint32_t *)(note + 28) = 0;

    memcpy(elf_img + 0x1000, linux_probe_code, sizeof(linux_probe_code));

    const char *probe_path = "/tmp/linux_probe.elf";
    int fd = open(probe_path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) {
        FAIL("linux_abi_compat", "failed to create probe ELF");
        return;
    }
    ssize_t written = write(fd, elf_img, sizeof(elf_img));
    close(fd);
    if (written != (ssize_t)sizeof(elf_img)) {
        FAIL("linux_abi_compat", "failed to write probe ELF");
        unlink(probe_path);
        return;
    }

    int64_t pid = fork();
    if (pid == 0) {
        char *argv[] = { (char *)probe_path, NULL };
        exec(probe_path, argv, NULL);
        _exit(127);
    }

    int st = -1;
    int64_t waited = waitpid(pid, &st, 0);
    unlink(probe_path);

    int exit_code = (st >= 0 && (st & 0x7f) == 0) ? ((st >> 8) & 0xff) : -1;
    CHECKF(waited == pid && exit_code == 0,
           "54_linux_abi_compat",
           "status=%d (exit=%d)", "status=%d (exit=%d)",
           st, exit_code);
}


// ── libc completion acceptance (T17) ─────────────────────────────────────
// The guest-side acceptance of the SHIPPED libc: every group the plan's
// coverage table promises is driven here on the real guest — the shipped
// libc.a, the real VFS, the real kernel, the real BusyBox shell.  The
// host groups own the fine-grained behaviour (and they compare against
// an independent oracle); these cases own the different question: "does
// the archive we ship actually work inside the guest?".
//
// The cases are named after the plan's guest case list (55_libc_alloc …
// 65_libc_append; 64_libc_regex is NOT implemented this cycle — the POSIX
// regex engine is out of scope, so regex.h is still the T1 inline stub).

// ── 55_libc_alloc ────────────────────────────────────────────────────────
// The allocator the shipped libc ships: alignment, the overflow checks
// and the growth path.
void test_libc_alloc(void)
{
    void *p;
    void *q;
    void *r;
    void *ap = NULL;
    unsigned char *bytes;
    char *keep;
    int zeroed = 1;
    int rc;

    p = malloc(64);
    CHECK3(p != NULL, "alloc malloc", p ? "64 bytes" : "NULL");
    CHECK3(p != NULL && ((uintptr_t)p % 16) == 0, "alloc alignment",
           (p && ((uintptr_t)p % 16) == 0) ? "16-byte aligned" : "misaligned");

    q = calloc(16, 8);
    bytes = (unsigned char *)q;
    for (int i = 0; q != NULL && i < 128; i++)
        if (bytes[i] != 0) zeroed = 0;
    CHECK3(q != NULL && zeroed, "alloc calloc zeroes",
           (q && zeroed) ? "128 zero bytes" : "not zeroed");

    // The overflow check: the element count times the element size must
    // not wrap, or the caller would get a block far smaller than it asked
    // for.
    errno = 0;
    p = calloc((size_t)-1 / 2, 4);
    CHECK3(p == NULL && errno == ENOMEM, "alloc calloc overflow",
           (p == NULL && errno == ENOMEM) ? "ENOMEM" : "no overflow answer");

    errno = 0;
    r = reallocarray(NULL, (size_t)-1, 2);
    CHECK3(r == NULL && errno == ENOMEM, "alloc reallocarray overflow",
           (r == NULL && errno == ENOMEM) ? "ENOMEM" : "no overflow answer");

    // Growth keeps the content.
    keep = malloc(4);
    memcpy(keep, "abc", 4);
    keep = realloc(keep, 128);
    CHECK3(keep != NULL && strcmp(keep, "abc") == 0,
           "alloc realloc keeps the content",
           (keep && strcmp(keep, "abc") == 0) ? "abc" : "content lost");

    // Aligned allocation: posix_memalign reports through its return value
    // and never touches errno; aligned_alloc follows C11 (the size must be
    // a multiple of the alignment).
    errno = 0;
    rc = posix_memalign(&ap, 64, 128);
    CHECK3(rc == 0 && ap != NULL && ((uintptr_t)ap % 64) == 0,
           "alloc posix_memalign",
           (rc == 0 && ap && ((uintptr_t)ap % 64) == 0) ? "64-byte aligned"
                                                       : "not aligned");

    errno = 0;
    q = aligned_alloc(32, 96);
    CHECK3(q != NULL && ((uintptr_t)q % 32) == 0, "alloc aligned_alloc",
           (q && ((uintptr_t)q % 32) == 0) ? "32-byte aligned" : "not aligned");

    errno = 0;
    q = aligned_alloc(32, 100);
    CHECK3(q == NULL && errno == EINVAL, "alloc aligned_alloc size rule",
           (q == NULL && errno == EINVAL) ? "EINVAL" : "no EINVAL");

    free(p);
    free(keep);
    free(ap);
    free(q);
}


// ── 56_libc_env ──────────────────────────────────────────────────────────
// One environment table with per-entry ownership: setenv copies, putenv
// borrows, unsetenv removes, and the table reaches a child through
// fork + execve.
void test_libc_env(void)
{
    const char *got;
    int ok;

    setenv("OS01_LIBC_ENV", "one", 1);
    got = getenv("OS01_LIBC_ENV");
    ok = got != NULL && strcmp(got, "one") == 0;
    CHECK3(ok, "env setenv/getenv", ok ? "one" : "wrong value");

    // An overwrite replaces the value.
    setenv("OS01_LIBC_ENV", "two", 1);
    got = getenv("OS01_LIBC_ENV");
    ok = got != NULL && strcmp(got, "two") == 0;
    CHECK3(ok, "env overwrite", ok ? "two" : "wrong value");

    // putenv borrows the caller's string: the table must not copy it.
    {
        static char borrowed[] = "OS01_LIBC_PUT=put";
        ok = putenv(borrowed) == 0;
        got = getenv("OS01_LIBC_PUT");
        ok = ok && got != NULL && strcmp(got, "put") == 0;
        CHECK3(ok, "env putenv", ok ? "put" : "wrong value");
    }

    unsetenv("OS01_LIBC_ENV");
    CHECK3(getenv("OS01_LIBC_ENV") == NULL, "env unsetenv",
           getenv("OS01_LIBC_ENV") == NULL ? "removed" : "still there");

    // The table reaches a child: the child re-execs itself through the
    // startup probe, which compares the inherited value with the expected
    // one (exit 21).
    {
        char *av[3] = {(char *)"startup-probe", (char *)"--envinherit", NULL};
        char *ep[3] = {(char *)"OS01_ENV_CHILD=v", (char *)"OS01_ENV_EXPECT=v",
                       NULL};
        ok = run_startup_probe_case(av, ep, 21) == 1;
        CHECK3(ok, "env reaches the child", ok ? "exit 21" : "wrong exit");
    }
}


// ── 57_libc_stdio ────────────────────────────────────────────────────────
// The stream layer on a real file of the guest filesystem: the mode
// parser, complete-item IO, the seek family and the dynamic line reader.
void test_libc_stdio(void)
{
    const char *path = "/tmp/libc_stdio_case.dat";
    FILE *f;
    char buf[64];
    char *line = NULL;
    size_t cap = 0;
    ssize_t got;
    int ok;

    unlink(path);
    f = fopen(path, "w+");
    CHECK3(f != NULL, "stdio fopen w+", f ? path : "NULL");
    if (f == NULL) return;

    ok = fwrite("line1\nline2\n", 1, 12, f) == 12;
    CHECK3(ok, "stdio fwrite", ok ? "12 bytes" : "short write");
    ok = fflush(f) == 0;
    CHECK3(ok, "stdio fflush", ok ? "flushed" : "failed");

    ok = fseek(f, 0, SEEK_END) == 0 && ftell(f) == 12;
    CHECK3(ok, "stdio fseek/ftell", ok ? "at 12" : "wrong offset");
    ok = fseek(f, 0, SEEK_SET) == 0 && ftell(f) == 0;
    CHECK3(ok, "stdio rewind offset", ok ? "at 0" : "wrong offset");

    ok = fgets(buf, sizeof(buf), f) != NULL && strcmp(buf, "line1\n") == 0;
    CHECK3(ok, "stdio fgets", ok ? "line1" : "wrong line");

    got = getline(&line, &cap, f);
    ok = got > 0 && strcmp(line, "line2\n") == 0;
    CHECK3(ok, "stdio getline", ok ? "line2" : "wrong line");
    free(line);
    line = NULL;
    cap = 0;

    // At the end of the file the EOF indicator is set, and the error
    // indicator is not.
    ok = feof(f) == 0;            /* the last getline ended ON the newline */
    CHECK3(ok, "stdio feof is sticky only after a read past the end",
           ok ? "not at EOF yet" : "unexpected EOF");

    ok = ferror(f) == 0;
    CHECK3(ok, "stdio ferror", ok ? "no error" : "error flagged");
    fclose(f);

    // "a" must NOT truncate: the file keeps its content.
    f = fopen(path, "a");
    ok = f != NULL;
    CHECK3(ok, "stdio fopen a", ok ? "opened" : "NULL");
    if (f != NULL) {
        fputs("line3\n", f);
        fclose(f);
        f = fopen(path, "r");
        if (f != NULL) {
            int total = 0;
            while (fgets(buf, sizeof(buf), f) != NULL) total++;
            fclose(f);
            ok = total == 3;
            CHECK3(ok, "stdio append mode keeps the file", ok ? "3 lines"
                                                             : "wrong line count");
        }
    }
    unlink(path);
}


// ── 58_libc_integer ──────────────────────────────────────────────────────
// The integer conversion core: the boundaries, the endptr and the
// overflow answers.
void test_libc_integer(void)
{
    char *end;
    long v;
    unsigned long uv;
    long long llv;
    intmax_t im;
    uintmax_t um;
    int ok;

    errno = 0;
    v = strtol("123", &end, 10);
    ok = v == 123 && *end == '\0' && errno == 0;
    CHECK3(ok, "integer strtol", ok ? "123" : "wrong value");

    errno = 0;
    v = strtol("  -42abc", &end, 10);
    ok = v == -42 && strcmp(end, "abc") == 0;
    CHECK3(ok, "integer strtol endptr", ok ? "endptr at abc" : "wrong endptr");

    // One past LONG_MAX: the value saturates and errno says ERANGE.
    errno = 0;
    v = strtol("9223372036854775808", &end, 10);
    ok = v == LONG_MAX && errno == ERANGE;
    CHECK3(ok, "integer strtol overflow", ok ? "LONG_MAX + ERANGE"
                                             : "wrong overflow answer");

    /* strtoul() takes the minus sign and negates the result: "-1" is
     * ULONG_MAX and that is NOT an overflow, so errno stays 0.  (glibc
     * answers the same; the host group compares against that oracle.) */
    errno = 0;
    uv = strtoul("-1", &end, 10);
    ok = uv == ULONG_MAX && errno == 0;
    CHECK3(ok, "integer strtoul -1", ok ? "ULONG_MAX, no ERANGE"
                                       : "wrong answer");

    errno = 0;
    llv = strtoll("-9223372036854775809", &end, 10);
    /* The bound is spelled out instead of using LLONG_MIN: the macro's
     * literal does not fit a signed type, and the compiler says so. */
    ok = llv == (-9223372036854775807LL - 1) && errno == ERANGE;
    CHECK3(ok, "integer strtoll underflow", ok ? "LLONG_MIN + ERANGE"
                                               : "wrong overflow answer");

    /* One past INTMAX_MAX: the value saturates and errno says ERANGE.
     * (The bound is spelled out because INTMAX_MAX's own literal does
     * not fit a signed type.) */
    errno = 0;
    im = strtoimax("170141183460469231731687303715884105727", &end, 10);
    ok = im == INTMAX_MAX && errno == ERANGE;
    CHECK3(ok, "integer strtoimax overflow", ok ? "INTMAX_MAX + ERANGE"
                                                : "wrong overflow answer");

    errno = 0;
    im = strtoimax("9223372036854775807", &end, 10);
    ok = im == INTMAX_MAX && errno == 0;
    CHECK3(ok, "integer strtoimax max", ok ? "INTMAX_MAX" : "wrong value");

    errno = 0;
    um = strtoumax("18446744073709551616", &end, 10);
    ok = um == UINTMAX_MAX && errno == ERANGE;
    CHECK3(ok, "integer strtoumax overflow", ok ? "UINTMAX_MAX + ERANGE"
                                                : "wrong overflow answer");

    // The base handling: 0x with base 0 and with base 16 are the same.
    v = strtol("0x1f", &end, 16);
    ok = v == 31 && *end == '\0';
    CHECK3(ok, "integer base 16", ok ? "31" : "wrong value");
    v = strtol("0x1f", &end, 0);
    ok = v == 31 && *end == '\0';
    CHECK3(ok, "integer base 0", ok ? "31" : "wrong value");

    // An empty input converts nothing: the endptr stays at the input and
    // no error is raised.
    errno = 0;
    v = strtol("   ", &end, 10);
    ok = v == 0 && end != NULL && *end != '\0' && errno == 0;
    CHECK3(ok, "integer no conversion", ok ? "endptr unmoved" : "wrong answer");
}


// ── 59_libc_float ────────────────────────────────────────────────────────
// The precision-aware float scanner: the long double keeps more bits than
// the double, the hexadecimal form parses, and the range errors are
// visible.
void test_libc_float(void)
{
    char *end;
    double d;
    float f;
    long double ld;
    int ok;

    errno = 0;
    d = strtod("3.5", &end);
    ok = d == 3.5 && *end == '\0' && errno == 0;
    CHECK3(ok, "float strtod", ok ? "3.5" : "wrong value");

    // The long double must NOT be routed through a double: the value it
    // keeps is more precise than the double's.
    d = strtod("0.1", &end);
    ld = strtold("0.1", &end);
    ok = ld != (long double)d;
    CHECK3(ok, "float strtold keeps the extra bits",
           ok ? "long double is finer" : "same as double");

    errno = 0;
    d = strtod("0x1.8p+1", &end);
    ok = d == 3.0 && *end == '\0';
    CHECK3(ok, "float hexadecimal", ok ? "3.0" : "wrong value");

    // "1e+" is a complete "1" followed by junk: the exponent sign without
    // digits belongs to the tail.
    errno = 0;
    d = strtod("1e+", &end);
    ok = d == 1.0 && strcmp(end, "e+") == 0;
    CHECK3(ok, "float partial exponent", ok ? "consumed 1" : "wrong endptr");

    errno = 0;
    d = strtod("1e400", &end);
    ok = (d > 1e308) && errno == ERANGE;
    CHECK3(ok, "float overflow", ok ? "ERANGE" : "no ERANGE");

    errno = 0;
    f = strtof("2.5", &end);
    ok = f == 2.5f && *end == '\0';
    CHECK3(ok, "float strtof", ok ? "2.5" : "wrong value");

    errno = 0;
    d = strtod("", &end);
    ok = d == 0.0 && *end == '\0' && errno == 0;
    CHECK3(ok, "float no conversion", ok ? "0, endptr unmoved" : "wrong answer");
}


// ── 60_libc_scan ─────────────────────────────────────────────────────────
// The width-aware, typed scanf core.
void test_libc_scan(void)
{
    int a = 0;
    int b = 0;
    unsigned long addr = 0;
    char word[16];
    long long ll = 0;
    size_t used = 0;
    int n;
    int ok;

    n = sscanf("12 34", "%d %d", &a, &b);
    ok = n == 2 && a == 12 && b == 34;
    CHECK3(ok, "scan two integers", ok ? "12 34" : "wrong values");

    n = sscanf("0x1000", "%lx", &addr);
    ok = n == 1 && addr == 0x1000;
    CHECK3(ok, "scan %lx", ok ? "0x1000" : "wrong value");

    // The field width limits the scan set — and the NUL is NOT counted.
    memset(word, 'x', sizeof(word));
    n = sscanf("abcdefgh", "%4s", word);
    ok = n == 1 && strcmp(word, "abcd") == 0;
    CHECK3(ok, "scan %4s width", ok ? "abcd" : "wrong word");

    // %n stores the consumed length and is NOT a conversion.
    n = sscanf("42", "%d%n", &a, &used);
    ok = n == 1 && a == 42 && used == 2;
    CHECK3(ok, "scan %n", ok ? "count 1, used 2" : "wrong count");

    // A typed store writes the target's own width.
    n = sscanf("-5", "%lld", &ll);
    ok = n == 1 && ll == -5;
    CHECK3(ok, "scan %lld", ok ? "-5" : "wrong value");

    // An empty input answers EOF; a first-character mismatch answers 0.
    n = sscanf("", "%d", &a);
    ok = n == EOF;
    CHECK3(ok, "scan empty input", ok ? "EOF" : "wrong answer");
    n = sscanf("x1", "%d", &a);
    ok = n == 0;
    CHECK3(ok, "scan no match", ok ? "0" : "wrong answer");
}


// ── 61_libc_time ─────────────────────────────────────────────────────────
// The calendar core and the POSIX TZ parser.  The guest has NO real-time
// clock (SYS_time answers the boot counter), so every answer here is
// computed from a FIXED timestamp — the case must not depend on a wall
// clock that does not exist.
void test_libc_time(void)
{
    time_t t = 1700000000;          /* 2023-11-14 22:13:20 UTC */
    struct tm tm;
    struct tm back;
    char buf[64];
    int ok;

    setenv("TZ", "UTC0", 1);
    tzset();

    gmtime_r(&t, &tm);
    ok = tm.tm_year == 123 && tm.tm_mon == 10 && tm.tm_mday == 14 &&
         tm.tm_hour == 22 && tm.tm_min == 13 && tm.tm_sec == 20;
    CHECK3(ok, "time gmtime", ok ? "2023-11-14 22:13:20" : "wrong fields");

    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    ok = strcmp(buf, "2023-11-14 22:13:20") == 0;
    CHECK3(ok, "time strftime", ok ? buf : buf);

    // A fixed offset: "CST-8" is UTC+8, so the local time is 8 hours later.
    setenv("TZ", "CST-8", 1);
    tzset();
    localtime_r(&t, &tm);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    ok = strcmp(buf, "2023-11-15 06:13:20") == 0;
    CHECK3(ok, "time localtime with TZ", ok ? buf : buf);

    // mktime turns the broken-down time back into the timestamp.
    ok = mktime(&tm) == t;
    CHECK3(ok, "time mktime round-trip", ok ? "same timestamp" : "wrong");

    // timegm is the UTC counterpart.
    gmtime_r(&t, &tm);
    ok = timegm(&tm) == t;
    CHECK3(ok, "time timegm", ok ? "same timestamp" : "wrong");

    // strptime parses what strftime wrote.
    memset(&back, 0, sizeof(back));
    ok = strptime("2023-11-14 22:13:20", "%Y-%m-%d %H:%M:%S", &back) != NULL;
    ok = ok && back.tm_year == 123 && back.tm_mon == 10 && back.tm_mday == 14;
    CHECK3(ok, "time strptime", ok ? "parsed" : "wrong fields");

    ok = difftime(100, 40) == 60.0;
    CHECK3(ok, "time difftime", ok ? "60" : "wrong");

    // clock() answers the CPU time, which this kernel does not provide:
    // the case pins that it says so instead of pretending.
    errno = 0;
    ok = clock() == (clock_t)-1;
    CHECK3(ok, "time clock reports no CPU time", ok ? "unavailable" : "a value");
}


// ── 62_libc_path_exec ────────────────────────────────────────────────────
// realpath against the REAL guest filesystem and the execvp PATH search —
// including the ENOEXEC hand-off, which is only observable when a REAL
// shell runs the script.
void test_libc_path_exec(void)
{
    char resolved[256];
    char *auto_buf;
    pid_t pid;
    int status = 0;
    int fd;
    int ok;

    // A symlink on the real filesystem, resolved by realpath.  The link
    // lives on the ROOT filesystem (ext2): the tmpfs under /tmp has no
    // symlink operation, so the kernel answers EOPNOTSUPP there.
    unlink("/libc_path_link");
    unlink("/libc_path_target");
    fd = open("/libc_path_target", O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) close(fd);
    ok = symlink("/libc_path_target", "/libc_path_link") == 0;
    CHECK3(ok, "path symlink", ok ? "created" : "failed");

    ok = realpath("/libc_path_link", resolved) != NULL &&
         strcmp(resolved, "/libc_path_target") == 0;
    CHECK3(ok, "path realpath", ok ? resolved : "wrong answer");

    // resolved == NULL: the libc allocates the buffer and the caller owns
    // it.
    auto_buf = realpath("/libc_path_link", NULL);
    ok = auto_buf != NULL && strcmp(auto_buf, "/libc_path_target") == 0;
    CHECK3(ok, "path realpath(NULL)", ok ? "caller-owned buffer" : "NULL");
    free(auto_buf);

    // The ENOEXEC hand-off: a SCRIPT is not a binary, so the libc must
    // run it through /bin/sh — and it must hand the shell the RESOLVED
    // path, not the bare name.  The script is NOT in the current
    // directory, so a shell that received the bare name would not find
    // it at all and would answer 127 instead of the script's own code.
    unlink("/tmp/libc_script");
    fd = open("/tmp/libc_script", O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) {
        const char *body = "#!/bin/sh\nexit 42\n";
        write(fd, body, strlen(body));
        close(fd);
        chmod("/tmp/libc_script", 0755);
    }
    pid = fork();
    if (pid == 0) {
        char *av[2];
        chdir("/");
        setenv("PATH", "/tmp", 1);
        av[0] = (char *)"libc_script";
        av[1] = NULL;
        execvp("libc_script", av);
        _exit(127);
    }
    if (pid > 0) waitpid(pid, &status, 0);
    ok = pid > 0 && (status & 0x7f) == 0 && ((status >> 8) & 0xff) == 42;
    CHECK3(ok, "path execvp hands the shell the resolved path",
           ok ? "the script ran" : "the shell did not find the script");

    unlink("/tmp/libc_script");
    unlink("/libc_path_link");
    unlink("/libc_path_target");
}


// ── 63_libc_dir ──────────────────────────────────────────────────────────
// The directory descriptor and the sorted scan against the REAL guest
// filesystems (tmpfs under /tmp, ext2 under /bin).
void test_libc_dir(void)
{
    DIR *d;
    struct dirent *e;
    struct dirent **list = NULL;
    int fd;
    int dir_fd;
    int n;
    int dots = 0;
    int sorted = 1;
    int ok;

    d = opendir("/tmp");
    ok = d != NULL;
    CHECK3(ok, "dir opendir /tmp", ok ? "/tmp" : "NULL");
    if (d != NULL) {
        while ((e = readdir(d)) != NULL)
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
                dots++;
        ok = dots == 2;
        CHECK3(ok, "dir the kernel's dot entries", ok ? "2 dot entries"
                                                     : "missing dot entries");
        dir_fd = dirfd(d);
        ok = dir_fd >= 0;
        CHECK3(ok, "dir dirfd", ok ? "descriptor" : "no descriptor");
        ok = closedir(d) == 0;
        CHECK3(ok, "dir closedir", ok ? "closed" : "failed");
    }

    // fdopendir takes the descriptor over: closedir closes it.
    fd = open("/tmp", O_RDONLY);
    d = fdopendir(fd);
    ok = d != NULL && dirfd(d) == fd;
    CHECK3(ok, "dir fdopendir takes the descriptor", ok ? "taken over"
                                                        : "failed");
    if (d != NULL) closedir(d);
    errno = 0;
    ok = fcntl(fd, F_GETFD) == -1 && errno == EBADF;
    CHECK3(ok, "dir closedir closes the descriptor", ok ? "EBADF" : "still open");

    // scandir with alphasort: the order is the name order.
    n = scandir("/tmp", &list, NULL, alphasort);
    ok = n >= 2 && list != NULL;
    CHECK3(ok, "dir scandir", ok ? "entries collected" : "failed");
    if (ok) {
        for (int i = 1; i < n; i++)
            if (strcmp(list[i - 1]->d_name, list[i]->d_name) > 0) sorted = 0;
        CHECK3(sorted, "dir alphasort orders by name",
               sorted ? "name order" : "wrong order");
        for (int i = 0; i < n; i++) free(list[i]);
        free(list);
    }
}


// ── 65_libc_append ───────────────────────────────────────────────────────
// The append semantics through the LIBC surface: the "a" mode must not
// truncate, and two independent descriptors with O_APPEND must both land
// at the end of the file.
void test_libc_append(void)
{
    const char *path = "/tmp/libc_append_case.dat";
    FILE *f;
    char buf[64];
    int fd;
    int n;
    int ok;

    unlink(path);
    f = fopen(path, "w");
    ok = f != NULL;
    CHECK3(ok, "append fopen w", ok ? "created" : "NULL");
    if (f != NULL) {
        fputs("first\n", f);
        fclose(f);
    }

    // "a" appends and does NOT truncate.
    f = fopen(path, "a");
    ok = f != NULL;
    CHECK3(ok, "append fopen a", ok ? "opened" : "NULL");
    if (f != NULL) {
        fputs("second\n", f);
        fclose(f);
    }
    fd = open(path, O_RDONLY);
    n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
    if (fd >= 0) close(fd);
    if (n > 0) buf[n] = '\0';
    ok = n > 0 && strcmp(buf, "first\nsecond\n") == 0;
    CHECK3(ok, "append mode keeps the file", ok ? "both lines" : buf);

    // Two independent descriptors with O_APPEND: neither write may land
    // on top of the other.
    unlink(path);
    fd = open(path, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) close(fd);
    {
        int a = open(path, O_WRONLY | O_APPEND);
        int b = open(path, O_WRONLY | O_APPEND);
        if (a >= 0) { write(a, "A", 1); close(a); }
        if (b >= 0) { write(b, "B", 1); close(b); }
    }
    fd = open(path, O_RDONLY);
    n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
    if (fd >= 0) close(fd);
    if (n > 0) buf[n] = '\0';
    ok = n == 2 && buf[0] != buf[1] && (buf[0] == 'A' || buf[0] == 'B') &&
         (buf[1] == 'A' || buf[1] == 'B');
    CHECK3(ok, "append two descriptors", ok ? "both bytes" : "overlapped");

    unlink(path);
}

