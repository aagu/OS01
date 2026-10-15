/*
 * test_monitor_render.c — monitor plan Task 8: terminal dashboard,
 * sampling loop and terminal lifecycle (spec §7;
 * docs/superpowers/specs/2026-10-09-monitor-design.md).
 *
 * Drives the PRODUCTION user/monitor/{render.c,monitor.c} host-compiled.
 * All byte output goes through one renamed seam (vfprintf ->
 * mon_test_vfprintf), so the fixture captures every byte the renderer
 * and the loop can emit; monitor.c's terminal calls are renamed onto
 * fixture shims (isatty/tcgetattr/tcsetattr/ioctl/poll/clock_gettime/
 * signal/read + monitor_collect/monitor_sample_destroy, errno ->
 * mon_test_errno) so the loop runs against a scripted clock, poll
 * actions and canned collection rounds with no real TTY.
 *
 * Asserted (plan Task 8 Step 1 / Global Constraints):
 *   - a 5000 (=50.00%) percentage prints "50.0%"; an invalid one "--";
 *   - memory shows the "PMM 占用" label and MiB (T==0 unavailable);
 *   - the temperature label follows the SOURCE (Core / Package Temp);
 *     a stale sample shows an explicit "expired" marker;
 *   - a Name containing ESC/backslash is re-escaped for display: the
 *     plain layout emits ZERO raw escape bytes and the name shows as
 *     the literal "\x1b"/"\\" text (never a terminal command);
 *   - a narrow terminal truncates VISIBLE text only: every escape
 *     sequence in the output is whole (no sequence is split), the line
 *     still closes its attribute before the newline, and the over-long
 *     name is truncated while the plain layout is not;
 *   - SIGINT, SIGTERM, 'q', stdin EOF and an init failure all take the
 *     single exit path: the original termios is written back and the
 *     cursor is shown (termios_restored && cursor_restored); a failed
 *     tcgetattr never writes back an uninitialized termios copy;
 *   - a collection that overruns by three seconds produces exactly ONE
 *     catch-up sample and advances the deadline past the missed periods
 *     (no busy catch-up);
 *   - --samples N is finite, plain (no escape bytes) and rejects an
 *     unknown argument;
 *   - the first round shows "--" for every CPU.
 */

#include "test_framework.h"

#include <assert.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <monitor/collect.h>
#include <monitor/model.h>
#include <monitor/render.h>

/* Production monitor.c is compiled with -Dmain=monitor_main so the
 * fixture can drive it and still define its own main(). */
int monitor_main(int argc, char **argv);

/* ══ Output capture seam (render.c + monitor.c -> mon_test_fprintf) ══ */

static char   cap_buf[1 << 16];
static size_t cap_len;
static int    g_cursor_hidden;
static int    g_cursor_shown;

int mon_test_fprintf(void *f, const char *fmt, ...)
{
    (void)f;
    va_list ap;
    va_start(ap, fmt);
    char tmp[4096];
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t add = (size_t)n;
        if (add > sizeof tmp - 1)
            add = sizeof tmp - 1;
        if (cap_len + add < sizeof cap_buf) {
            memcpy(cap_buf + cap_len, tmp, add);
            cap_len += add;
            cap_buf[cap_len] = '\0';
        }
    }
    if (strstr(tmp, "\x1b[?25l"))
        g_cursor_hidden = 1;
    if (strstr(tmp, "\x1b[?25h"))
        g_cursor_shown = 1;
    return n;
}

int mon_test_fflush(void *f)
{
    (void)f;
    return 0;
}

/* ══ errno (non-TLS) seam ══ */
int mon_test_errno;

/* ══ Terminal / syscall shims ══ */

static struct termios g_canned_orig;
static int  g_tcgetattr_fail;
static int  tcsetattr_raw_seen;
static int  tcsetattr_restored;

static int g_isatty_stdin  = 1;
static int g_isatty_stdout = 1;

static int g_ioctl_fail;
static unsigned g_win_rows = 24, g_win_cols = 80;

int mon_test_isatty(int fd)
{
    return fd == STDIN_FILENO ? g_isatty_stdin : g_isatty_stdout;
}

int mon_test_tcgetattr(int fd, struct termios *t)
{
    (void)fd;
    if (g_tcgetattr_fail)
        return -1;
    *t = g_canned_orig;
    return 0;
}

int mon_test_tcsetattr(int fd, int action, const struct termios *t)
{
    (void)fd;
    (void)action;
    if (t->c_lflag == g_canned_orig.c_lflag &&
        t->c_iflag == g_canned_orig.c_iflag &&
        t->c_oflag == g_canned_orig.c_oflag &&
        t->c_cflag == g_canned_orig.c_cflag)
        tcsetattr_restored = 1;
    else
        tcsetattr_raw_seen = 1;
    return 0;
}

int mon_test_ioctl(int fd, unsigned long request, ...)
{
    (void)fd;
    va_list ap;
    va_start(ap, request);
    if (request == TIOCGWINSZ) {
        struct winsize *ws = va_arg(ap, struct winsize *);
        if (g_ioctl_fail) {
            va_end(ap);
            return -1;
        }
        ws->ws_row = (unsigned short)g_win_rows;
        ws->ws_col = (unsigned short)g_win_cols;
        ws->ws_xpixel = 0;
        ws->ws_ypixel = 0;
        va_end(ap);
        return 0;
    }
    va_end(ap);
    return -1;
}

/* ── Scripted clock ── */
static uint64_t g_now_ns;
static int      g_clock_fail_after = -1;   /* <0 never; 0 now; n after n */

int mon_test_clock_gettime(int clk_id, struct timespec *tp)
{
    (void)clk_id;
    if (g_clock_fail_after == 0)
        return -1;
    if (g_clock_fail_after > 0)
        g_clock_fail_after--;
    tp->tv_sec = g_now_ns / 1000000000ULL;
    tp->tv_nsec = g_now_ns % 1000000000ULL;
    return 0;
}

/* ── Scripted poll ── */
enum poll_kind { PK_TIMEOUT, PK_INPUT, PK_EOF, PK_SIGNAL, PK_ERROR };
struct poll_act { int kind; uint64_t advance_ns; char byte; int signo; };

static struct poll_act g_acts[64];
static int g_act_count, g_act_idx;

static sighandler_t g_sigint_handler;
static sighandler_t g_sigterm_handler;

sighandler_t mon_test_signal(int sig, sighandler_t handler)
{
    if (sig == SIGINT)
        g_sigint_handler = handler;
    else if (sig == SIGTERM)
        g_sigterm_handler = handler;
    return handler;
}

static int  g_have_key;
static char g_pending_key;
static int  g_have_eof;

int mon_test_poll(struct pollfd *fds, nfds_t nfds, int timeout)
{
    if (nfds == 0) {
        /* plain sleep used by --samples: advance the virtual clock so
         * the deadline is reached. */
        if (timeout > 0)
            g_now_ns += (uint64_t)timeout * 1000000ULL;
        return 0;
    }
    g_have_key = 0;
    g_have_eof = 0;
    if (g_act_idx >= g_act_count) {
        /* default: signal EOF so a runaway loop cannot hang the test */
        g_have_eof = 1;
        fds[0].revents = POLLIN | POLLHUP;
        return 1;
    }
    struct poll_act a = g_acts[g_act_idx++];
    g_now_ns += a.advance_ns;
    switch (a.kind) {
    case PK_TIMEOUT:
        return 0;
    case PK_INPUT:
        g_have_key = 1;
        g_pending_key = a.byte;
        fds[0].revents = POLLIN;
        return 1;
    case PK_EOF:
        g_have_eof = 1;
        fds[0].revents = POLLIN | POLLHUP;
        return 1;
    case PK_SIGNAL:
        if (a.signo == SIGINT && g_sigint_handler)
            g_sigint_handler(SIGINT);
        else if (a.signo == SIGTERM && g_sigterm_handler)
            g_sigterm_handler(SIGTERM);
        mon_test_errno = EINTR;
        return -1;
    case PK_ERROR:
    default:
        mon_test_errno = EIO;
        return -1;
    }
}

int64_t mon_test_read(int fd, void *buf, uint64_t size)
{
    (void)fd;
    (void)size;
    if (g_have_eof) {
        g_have_eof = 0;
        return 0;
    }
    if (g_have_key) {
        *(char *)buf = g_pending_key;
        g_have_key = 0;
        return 1;
    }
    return 0;   /* EOF by default */
}

/* ── Canned collection rounds ── */
static struct monitor_sample g_rounds[8];
static int g_round_count, g_round_idx;
static int g_collect_calls;

int mon_test_collect(struct monitor_sample *out)
{
    memset(out, 0, sizeof *out);
    g_collect_calls++;
    if (g_round_idx < g_round_count) {
        const struct monitor_sample *src = &g_rounds[g_round_idx++];
        out->stat_valid = src->stat_valid;
        out->stat = src->stat;
        out->mem_valid = src->mem_valid;
        out->mem = src->mem;
        out->thermal_valid = src->thermal_valid;
        out->thermal = src->thermal;
    }
    return 0;
}

void mon_test_sample_destroy(struct monitor_sample *out)
{
    if (out)
        memset(out, 0, sizeof *out);
}

/* ══ Fixture helpers ══ */

static void cap_reset(void)
{
    cap_len = 0;
    cap_buf[0] = '\0';
    g_cursor_hidden = 0;
    g_cursor_shown = 0;
}

static const char *cap(void) { return cap_buf; }

static void fixture_reset(void)
{
    cap_reset();
    mon_test_errno = 0;

    memset(&g_canned_orig, 0, sizeof g_canned_orig);
    g_canned_orig.c_lflag = ISIG | ICANON | ECHO;
    g_canned_orig.c_iflag = ICRNL;
    g_canned_orig.c_oflag = OPOST;
    g_canned_orig.c_cflag = CS8 | CREAD;
    g_tcgetattr_fail = 0;
    tcsetattr_raw_seen = 0;
    tcsetattr_restored = 0;

    g_isatty_stdin = 1;
    g_isatty_stdout = 1;
    g_ioctl_fail = 0;
    g_win_rows = 24;
    g_win_cols = 80;

    g_now_ns = 0;
    g_clock_fail_after = -1;

    g_act_count = g_act_idx = 0;
    g_have_key = 0;
    g_have_eof = 0;
    g_sigint_handler = NULL;
    g_sigterm_handler = NULL;

    memset(g_rounds, 0, sizeof g_rounds);
    g_round_count = g_round_idx = 0;
    g_collect_calls = 0;
}

static void add_round_cpu(struct monitor_sample *s, uint32_t id, uint64_t epoch,
                          uint64_t busy, uint64_t idle)
{
    struct monitor_cpu_sample *c = &s->stat.cpu[s->stat.ncpu++];
    memset(c, 0, sizeof *c);
    c->cpu_id = id;
    c->online = 1;
    c->valid = 1;
    c->epoch = epoch;
    c->busy_ticks = busy;
    c->idle_ticks = idle;
    c->tick_hz = s->stat.tick_hz;
}

/* Round that yields a 50% total delta against the base round. */
static void script_two_rounds_50pct(void)
{
    struct monitor_sample *r0 = &g_rounds[g_round_count++];
    memset(r0, 0, sizeof *r0);
    r0->stat_valid = 1;
    r0->stat.tick_hz = 100;
    add_round_cpu(r0, 0, 1, 0, 0);
    add_round_cpu(r0, 1, 1, 0, 0);

    struct monitor_sample *r1 = &g_rounds[g_round_count++];
    memset(r1, 0, sizeof *r1);
    r1->stat_valid = 1;
    r1->stat.tick_hz = 100;
    add_round_cpu(r1, 0, 1, 100, 0);   /* core0 fully busy */
    add_round_cpu(r1, 1, 1, 0, 100);   /* core1 fully idle */
}

static void act_timeout(uint64_t ns)
{
    g_acts[g_act_count].kind = PK_TIMEOUT;
    g_acts[g_act_count].advance_ns = ns;
    g_act_count++;
}

static void act_input(char c)
{
    g_acts[g_act_count].kind = PK_INPUT;
    g_acts[g_act_count].byte = c;
    g_act_count++;
}

static void act_eof(void)
{
    g_acts[g_act_count].kind = PK_EOF;
    g_act_count++;
}

static void act_signal(int signo)
{
    g_acts[g_act_count].kind = PK_SIGNAL;
    g_acts[g_act_count].signo = signo;
    g_act_count++;
}

/* ── ANSI integrity scanner ── */
static int ansi_wellformed(const char *s, int *count)
{
    size_t n = strlen(s);
    int ok = 1, c = 0;
    for (size_t i = 0; i < n; i++) {
        if ((unsigned char)s[i] == 0x1b) {
            c++;
            if (i + 1 >= n || s[i + 1] != '[') { ok = 0; break; }
            size_t j = i + 2;
            while (j < n && !((unsigned char)s[j] >= 0x40 && (unsigned char)s[j] <= 0x7e))
                j++;
            if (j >= n) { ok = 0; break; }   /* ran off the end: split seq */
            i = j;
        }
    }
    if (count)
        *count = c;
    return ok;
}

static int max_run(const char *s, char ch)
{
    int best = 0, cur = 0;
    for (const char *p = s; *p; p++) {
        if (*p == ch) { cur++; if (cur > best) best = cur; }
        else cur = 0;
    }
    return best;
}

/* ══ Renderer tests (direct model -> monitor_render) ══ */

static struct monitor_process_row g_rows[16];

static void base_model(struct monitor_model *m)
{
    memset(m, 0, sizeof *m);
    m->proc_rows = g_rows;
    m->nproc_rows = 0;
}

static void test_percent_and_invalid(void)
{
    struct monitor_model m;
    base_model(&m);
    m.total_cpu.valid = true;
    m.total_cpu.percent_10000 = 5000;
    m.ncpu_rows = 1;
    m.cpu_rows[0].cpu_id = 0;
    m.cpu_rows[0].online = true;
    m.cpu_rows[0].pct.valid = true;
    m.cpu_rows[0].pct.percent_10000 = 5000;

    cap_reset();
    assert_eq(monitor_render(stdout, &m, 24, 0), 0);
    assert_not_null(strstr(cap(), "50.0%"));
    assert_true(memchr(cap(), 0x1b, cap_len) == NULL);   /* plain: no ANSI */

    /* invalid -> "--" */
    m.total_cpu.valid = false;
    m.cpu_rows[0].pct.valid = false;
    cap_reset();
    assert_eq(monitor_render(stdout, &m, 24, 0), 0);
    assert_not_null(strstr(cap(), "--"));
    assert_true(strstr(cap(), "50.0%") == NULL);

    /* one-decimal rounding of the 10000 scale */
    m.total_cpu.valid = true;
    m.total_cpu.percent_10000 = 3333;
    cap_reset();
    assert_eq(monitor_render(stdout, &m, 24, 0), 0);
    assert_not_null(strstr(cap(), "33.3%"));
}

static void test_pmm_label(void)
{
    struct monitor_model m;
    base_model(&m);
    m.mem_valid = true;
    m.mem_total_bytes = 98ULL << 20;    /* 98.0 MiB */
    m.mem_used_bytes  = 12ULL << 20;    /* 12.0 MiB */
    m.mem_free_bytes  = 86ULL << 20;
    m.mem_pct.valid = true;
    m.mem_pct.percent_10000 = 1224;     /* ~12.24% -> 12.2% */

    cap_reset();
    assert_eq(monitor_render(stdout, &m, 24, 0), 0);
    assert_not_null(strstr(cap(), "PMM 占用"));
    assert_not_null(strstr(cap(), "MiB"));
    assert_not_null(strstr(cap(), "12.0 MiB"));
    assert_not_null(strstr(cap(), "98.0 MiB"));

    m.mem_valid = false;
    cap_reset();
    assert_eq(monitor_render(stdout, &m, 24, 0), 0);
    assert_not_null(strstr(cap(), "PMM 占用: --"));
}

static void test_thermal_source_labels(void)
{
    struct monitor_model m;
    base_model(&m);
    m.thermal_valid = true;
    m.thermal.state = MONITOR_THERMAL_STATE_OK;
    m.thermal.cpu_id = 0;
    m.thermal.temp_mc = 45000;

    m.thermal.source = MONITOR_THERMAL_SOURCE_CORE_DTS;
    cap_reset();
    assert_eq(monitor_render(stdout, &m, 24, 0), 0);
    assert_not_null(strstr(cap(), "Core Temp"));
    assert_not_null(strstr(cap(), "45.0"));

    m.thermal.source = MONITOR_THERMAL_SOURCE_PACKAGE_DTS;
    cap_reset();
    assert_eq(monitor_render(stdout, &m, 24, 0), 0);
    assert_not_null(strstr(cap(), "Package Temp"));
    assert_true(strstr(cap(), "Core Temp") == NULL);

    m.thermal.source = MONITOR_THERMAL_SOURCE_NONE;
    cap_reset();
    assert_eq(monitor_render(stdout, &m, 24, 0), 0);
    assert_true(strstr(cap(), "Core Temp") == NULL);
    assert_true(strstr(cap(), "Package Temp") == NULL);
}

static void test_thermal_stale_and_unsupported(void)
{
    struct monitor_model m;
    base_model(&m);
    m.thermal_valid = true;
    m.thermal.state = MONITOR_THERMAL_STATE_STALE;
    cap_reset();
    assert_eq(monitor_render(stdout, &m, 24, 0), 0);
    assert_not_null(strstr(cap(), "expired"));

    m.thermal.state = MONITOR_THERMAL_STATE_UNSUPPORTED;
    cap_reset();
    assert_eq(monitor_render(stdout, &m, 24, 0), 0);
    assert_not_null(strstr(cap(), "unsupported"));
    assert_true(strstr(cap(), "expired") == NULL);
}

static void test_proc_name_escaped_plain(void)
{
    struct monitor_model m;
    base_model(&m);
    m.nproc_rows = 1;
    memset(&g_rows[0], 0, sizeof g_rows[0]);
    g_rows[0].pid = 7;
    g_rows[0].state = 'R';
    g_rows[0].comm[0] = (char)0x1b;   /* ESC control byte */
    g_rows[0].comm[1] = '\\';         /* backslash */
    g_rows[0].comm[2] = 'x';
    g_rows[0].comm[3] = '\0';

    cap_reset();
    assert_eq(monitor_render(stdout, &m, 24, 0), 0);    /* plain layout */
    /* No raw control byte is ever sent to the terminal... */
    assert_true(memchr(cap(), 0x1b, cap_len) == NULL);
    /* ...the ESC and the backslash are shown as literal text instead. */
    assert_not_null(strstr(cap(), "\\x1b"));
    assert_not_null(strstr(cap(), "\\\\x"));
}

static void test_narrow_truncation_keeps_ansi(void)
{
    struct monitor_model m;
    base_model(&m);
    m.nproc_rows = 1;
    memset(&g_rows[0], 0, sizeof g_rows[0]);
    g_rows[0].pid = 1;
    g_rows[0].state = 'R';
    for (int i = 0; i < 40; i++)
        g_rows[0].comm[i] = 'A';
    g_rows[0].comm[40] = '\0';

    cap_reset();
    assert_eq(monitor_render(stdout, &m, 24, 30), 0);   /* ANSI, narrow */
    int seqs = 0;
    assert_true(ansi_wellformed(cap(), &seqs));         /* no split sequences */
    assert_true(seqs > 0);                              /* ANSI mode active */
    assert_not_null(strstr(cap(), "\x1b[0m"));          /* attribute closed */
    assert_true(max_run(cap(), 'A') < 40);              /* name truncated */
    assert_true(max_run(cap(), 'A') >= 4);              /* ...but visible */

    /* plain layout does NOT truncate and emits no ANSI at all */
    cap_reset();
    assert_eq(monitor_render(stdout, &m, 24, 0), 0);
    assert_true(memchr(cap(), 0x1b, cap_len) == NULL);
    assert_true(max_run(cap(), 'A') == 40);
}

static void test_first_round_all_dashes(void)
{
    struct monitor_model m;
    base_model(&m);
    memset(&m, 0, sizeof m);        /* zeroed model, nothing valid */
    m.ncpu_rows = 2;
    m.cpu_rows[0].cpu_id = 0;
    m.cpu_rows[1].cpu_id = 1;
    cap_reset();
    assert_eq(monitor_render(stdout, &m, 24, 0), 0);
    assert_not_null(strstr(cap(), "CPU0: --"));
    assert_not_null(strstr(cap(), "CPU1: --"));
    assert_not_null(strstr(cap(), "CPU total: --"));
}

/* ══ Lifecycle tests (drive main through the shims) ══ */

static void test_unknown_argument(void)
{
    fixture_reset();
    char *av[] = { (char *)"monitor", (char *)"--bogus", NULL };
    int rc = monitor_main(2, av);
    assert_true(rc != 0);
    assert_true(strstr(cap(), "unknown argument") != NULL);
}

static void test_samples_mode_plain_finite(void)
{
    fixture_reset();
    g_isatty_stdin = 0;
    g_isatty_stdout = 0;
    script_two_rounds_50pct();
    char *av[] = { (char *)"monitor", (char *)"--samples", (char *)"2", NULL };
    int rc = monitor_main(3, av);
    assert_eq(rc, 0);
    assert_eq(g_collect_calls, 2);                       /* finite */
    assert_true(memchr(cap(), 0x1b, cap_len) == NULL);   /* no ANSI */
    assert_not_null(strstr(cap(), "50.0%"));
    /* a --samples run must not touch the terminal */
    assert_false(tcsetattr_raw_seen);
    assert_false(g_cursor_hidden);
}

static void test_stdin_eof_exits(void)
{
    fixture_reset();
    g_isatty_stdin = 0;              /* input pipe, output not a TTY */
    g_isatty_stdout = 0;
    script_two_rounds_50pct();
    act_timeout(1000000000ULL);
    act_timeout(1000000000ULL);
    act_eof();                       /* read() -> 0 */
    char *av[] = { (char *)"monitor", NULL };
    int rc = monitor_main(1, av);
    assert_eq(rc, 0);
    assert_true(g_collect_calls >= 1);
    /* non-TTY: plain, no escape bytes, no termios touched */
    assert_true(memchr(cap(), 0x1b, cap_len) == NULL);
    assert_false(tcsetattr_raw_seen);
}

static void test_q_restores_terminal(void)
{
    fixture_reset();
    script_two_rounds_50pct();
    act_timeout(1000000000ULL);      /* -> round 1 (-- ) */
    act_timeout(1000000000ULL);      /* -> round 2 (50.0%) */
    act_input('q');
    char *av[] = { (char *)"monitor", NULL };
    int rc = monitor_main(1, av);

    bool termios_restored = tcsetattr_restored;
    bool cursor_restored = g_cursor_shown;
    assert_eq(rc, 0);
    assert_true(tcsetattr_raw_seen);                 /* entered raw mode */
    assert_true(termios_restored && cursor_restored);/* KEY assertion */
    assert_true(strstr(cap(), "50.0%") != NULL);     /* KEY assertion */
    assert_true(g_cursor_hidden);                    /* cursor was hidden */
    /* the first round rendered an invalid (dim) percentage: "--" */
    assert_not_null(strstr(cap(), "\x1b[2m--"));
}

static void test_sigint_restores_terminal(void)
{
    fixture_reset();
    act_timeout(1000000000ULL);
    act_signal(SIGINT);
    char *av[] = { (char *)"monitor", NULL };
    int rc = monitor_main(1, av);
    bool termios_restored = tcsetattr_restored;
    bool cursor_restored = g_cursor_shown;
    assert_eq(rc, 0);
    assert_true(termios_restored && cursor_restored);
}

static void test_sigterm_restores_terminal(void)
{
    fixture_reset();
    act_timeout(1000000000ULL);
    act_signal(SIGTERM);
    char *av[] = { (char *)"monitor", NULL };
    int rc = monitor_main(1, av);
    bool termios_restored = tcsetattr_restored;
    bool cursor_restored = g_cursor_shown;
    assert_eq(rc, 0);
    assert_true(termios_restored && cursor_restored);
}

static void test_init_failure_after_terminal_setup(void)
{
    fixture_reset();
    g_clock_fail_after = 0;          /* deadline clock call fails */
    char *av[] = { (char *)"monitor", NULL };
    int rc = monitor_main(1, av);
    bool termios_restored = tcsetattr_restored;
    bool cursor_restored = g_cursor_shown;
    assert_true(rc != 0);
    assert_true(termios_restored && cursor_restored);
}

static void test_tcgetattr_failure_no_restore(void)
{
    fixture_reset();
    g_tcgetattr_fail = 1;
    char *av[] = { (char *)"monitor", NULL };
    int rc = monitor_main(1, av);
    assert_true(rc != 0);
    /* never write back an uninitialized termios copy */
    assert_false(tcsetattr_restored);
    assert_false(tcsetattr_raw_seen);
    /* never entered the screen, so the cursor was never hidden */
    assert_false(g_cursor_hidden);
}

static void test_three_second_delay_one_catchup(void)
{
    fixture_reset();
    script_two_rounds_50pct();
    act_timeout(3000000000ULL);      /* poll wakes 3s late */
    act_input('q');
    char *av[] = { (char *)"monitor", NULL };
    int rc = monitor_main(1, av);
    assert_eq(rc, 0);
    /* exactly ONE catch-up sample despite three elapsed seconds */
    assert_eq(g_collect_calls, 1);
    bool termios_restored = tcsetattr_restored;
    bool cursor_restored = g_cursor_shown;
    assert_true(termios_restored && cursor_restored);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_percent_and_invalid),
    TEST_ENTRY(test_pmm_label),
    TEST_ENTRY(test_thermal_source_labels),
    TEST_ENTRY(test_thermal_stale_and_unsupported),
    TEST_ENTRY(test_proc_name_escaped_plain),
    TEST_ENTRY(test_narrow_truncation_keeps_ansi),
    TEST_ENTRY(test_first_round_all_dashes),
    TEST_ENTRY(test_unknown_argument),
    TEST_ENTRY(test_samples_mode_plain_finite),
    TEST_ENTRY(test_stdin_eof_exits),
    TEST_ENTRY(test_q_restores_terminal),
    TEST_ENTRY(test_sigint_restores_terminal),
    TEST_ENTRY(test_sigterm_restores_terminal),
    TEST_ENTRY(test_init_failure_after_terminal_setup),
    TEST_ENTRY(test_tcgetattr_failure_no_restore),
    TEST_ENTRY(test_three_second_delay_one_catchup),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
