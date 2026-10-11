/*
 * user/monitor/render.c — monitor plan Task 8: terminal dashboard.
 *
 * Read-only over monitor_model (Task 7).  Emits two layouts through one
 * bounded writer (spec §7, plan Global Constraints / Task 8 Step 3):
 *
 *   - ANSI dashboard when cols > 0: the writer counts VISIBLE columns
 *     only, so clamping a line to the terminal width never splits an
 *     escape sequence; every line ends with an attribute reset and an
 *     erase-to-EOL, so a mid-line clamp cannot leak colour or leave
 *     stale text behind.
 *   - plain text when cols == 0 (non-TTY / --samples): no escape bytes
 *     and no clamp.
 *
 * Display rules:
 *   - 10000 = 100.00%; shown rounded to ONE decimal (3333 -> "33.3%").
 *   - an invalid percentage (first round, counter regression, epoch
 *     change, zero denominator) shows "--".
 *   - memory is shown in MiB under the label "PMM 占用"; T == 0 is
 *     unavailable ("--").
 *   - the temperature label follows the SOURCE (Core DTS -> "Core Temp",
 *     Package DTS -> "Package Temp"); a stale sample shows an explicit
 *     expiry marker, an unsupported backend shows "unsupported".
 *   - a process Name is re-escaped for display exactly like the kernel
 *     encodes it (§6): printable bytes except backslash verbatim,
 *     backslash -> "\\", everything else (control bytes such as ESC)
 *     -> "\xHH".  A control byte from comm is therefore NEVER sent to
 *     the terminal as a command.
 *   - a process row flagged stale shows an explicit expiry marker; its
 *     old CPU% is never presented as current (spec §5.3).
 */

#include "render.h"

#include <stdio.h>
#include <string.h>

/* ── ANSI helpers ────────────────────────────────────────────────── */

#define C_RESET  "\x1b[0m"
#define C_DIM    "\x1b[2m"
#define C_RED    "\x1b[31m"
#define C_GREEN  "\x1b[32m"
#define C_YELLOW "\x1b[33m"
#define C_WHITE  "\x1b[37m"

#define C_CLEAR_SCREEN "\x1b[2J\x1b[H"
#define C_HIDE_CURSOR  "\x1b[?25l"
#define C_SHOW_CURSOR  "\x1b[?25h"
#define C_HOME         "\x1b[H"
#define C_ERASE_EOL    "\x1b[K"

/* ── Bounded terminal writer ─────────────────────────────────────── */

struct tw {
    FILE    *out;
    unsigned cols;      /* 0 = plain/unlimited */
    unsigned width;     /* visible columns already emitted this line */
    int      ansi;      /* emit escape sequences */
};

static void tw_raw(struct tw *w, const char *s)
{
    fprintf(w->out, "%s", s);
}

/* Escape sequences are written whole and never consume a visible
 * column, so clamping can never cut one. */
static void tw_ansi(struct tw *w, const char *seq)
{
    if (w->ansi)
        tw_raw(w, seq);
}

static void tw_putc(struct tw *w, char c)
{
    if (w->cols && w->width >= w->cols)
        return;                 /* clamped: visible text stops here */
    fprintf(w->out, "%c", c);
    w->width++;
}

static void tw_str(struct tw *w, const char *s)
{
    while (*s)
        tw_putc(w, *s++);
}

static void tw_newline(struct tw *w)
{
    tw_ansi(w, C_RESET);        /* always close attributes, whole seq */
    tw_ansi(w, C_ERASE_EOL);
    fprintf(w->out, "%c", '\n');
    w->width = 0;
}

/* ── Percentage formatting ───────────────────────────────────────── */

/* Write "50.0%" / "--" into buf (no colour), used where the value is a
 * fixed-width table column. */
static void pct_to_str(char *buf, size_t n, const struct monitor_pct *p)
{
    if (!p->valid) {
        snprintf(buf, n, "--");
        return;
    }
    unsigned v = p->percent_10000;
    if (v > 10000u)
        v = 10000u;
    unsigned tenths = (v + 5u) / 10u;   /* round 10000-scale to 1 decimal */
    snprintf(buf, n, "%u.%u%%", tenths / 10u, tenths % 10u);
}

static void tw_pct(struct tw *w, const struct monitor_pct *p)
{
    if (!p->valid) {
        tw_ansi(w, C_DIM);
        tw_str(w, "--");
        tw_ansi(w, C_RESET);
        return;
    }
    unsigned v = p->percent_10000;
    if (v > 10000u)
        v = 10000u;
    unsigned tenths = (v + 5u) / 10u;
    tw_ansi(w, tenths >= 900u ? C_RED : tenths >= 700u ? C_YELLOW : C_GREEN);
    char b[12];
    snprintf(b, sizeof b, "%u.%u%%", tenths / 10u, tenths % 10u);
    tw_str(w, b);
    tw_ansi(w, C_RESET);
}

/* ── Memory (MiB, one decimal) ───────────────────────────────────── */

static void tw_mib(struct tw *w, uint64_t bytes)
{
    /* Tenths of a MiB, rounded to nearest; bytes*10 stays well inside
     * 64 bits for any real machine. */
    uint64_t t10 = (bytes * 10u + (1u << 19)) >> 20;
    char b[32];
    snprintf(b, sizeof b, "%llu.%llu MiB",
             (unsigned long long)(t10 / 10u),
             (unsigned long long)(t10 % 10u));
    tw_str(w, b);
}

/* ── Name escaping (§6: printable except backslash verbatim) ─────── */

static void tw_escape(struct tw *w, const char *s)
{
    static const char hex[] = "0123456789abcdef";
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (c == '\\') {
            tw_putc(w, '\\');
            tw_putc(w, '\\');
        } else if (c >= 0x20 && c <= 0x7e) {
            tw_putc(w, (char)c);
        } else {
            tw_putc(w, '\\');
            tw_putc(w, 'x');
            tw_putc(w, hex[(c >> 4) & 0xf]);
            tw_putc(w, hex[c & 0xf]);
        }
    }
}

/* ── History sparkline (valid points only; gaps stay blank) ──────── */

#define HIST_DISPLAY 40

static void tw_hist(struct tw *w, const struct monitor_history *h)
{
    static const char bars[] = ".:-=+*#@";
    unsigned n = h->count;
    if (n > HIST_DISPLAY)
        n = HIST_DISPLAY;
    for (unsigned k = 0; k < n; k++) {
        unsigned age = n - 1u - k;          /* oldest first */
        const struct monitor_hist_point *pt = monitor_history_get(h, age);
        if (!pt->valid) {
            tw_putc(w, ' ');                /* empty point, not a zero */
            continue;
        }
        unsigned v = pt->percent_10000;
        if (v > 10000u)
            v = 10000u;
        tw_putc(w, bars[(v * 7u) / 10000u]);
    }
}

/* ── Temperature ─────────────────────────────────────────────────── */

static void tw_temp(struct tw *w, const struct monitor_model *m)
{
    if (!m->thermal_valid) {
        tw_str(w, "Temp: --");
        return;
    }
    const struct monitor_thermal_sample *t = &m->thermal;

    if (t->state == MONITOR_THERMAL_STATE_STALE) {
        tw_ansi(w, C_YELLOW);
        tw_str(w, "Temp: expired");
        tw_ansi(w, C_RESET);
        return;
    }
    if (t->state == MONITOR_THERMAL_STATE_UNSUPPORTED) {
        tw_str(w, "Temp: unsupported");
        return;
    }
    if (t->state != MONITOR_THERMAL_STATE_OK) {
        tw_str(w, "Temp: --");
        return;
    }

    const char *label;
    switch (t->source) {
    case MONITOR_THERMAL_SOURCE_CORE_DTS:    label = "Core Temp";    break;
    case MONITOR_THERMAL_SOURCE_PACKAGE_DTS: label = "Package Temp"; break;
    default:                                 label = "Temp";         break;
    }

    int32_t mc = t->temp_mc;
    int neg = mc < 0;
    int32_t amc = neg ? -mc : mc;
    unsigned whole = (unsigned)(amc / 1000);
    unsigned frac = (unsigned)((amc % 1000) / 100);
    char b[80];
    snprintf(b, sizeof b, "Temp: CPU%u %s %s%u.%u'C",
             t->cpu_id, label, neg ? "-" : "", whole, frac);
    tw_str(w, b);
}

/* ── Frame ───────────────────────────────────────────────────────── */

int monitor_render(FILE *out, const struct monitor_model *m,
                   unsigned rows, unsigned cols)
{
    if (!out || !m)
        return -1;

    struct tw w;
    w.out = out;
    w.cols = cols;
    w.width = 0;
    w.ansi = cols > 0;
    unsigned budget = rows;         /* 0 = unbounded */
    unsigned line = 0;

    if (w.ansi) {
        tw_ansi(&w, C_HOME);
        /* The dashboard is larger than one screen: each line erases to
         * EOL so shorter updates do not leave stale text. */
    }

    tw_str(&w, "OS01 monitor");
    tw_newline(&w);
    line++;

    tw_str(&w, "CPU total: ");
    tw_pct(&w, &m->total_cpu);
    tw_newline(&w);
    line++;

    for (uint32_t i = 0; i < m->ncpu_rows; i++) {
        if (budget && line >= budget)
            break;
        char name[16];
        snprintf(name, sizeof name, "CPU%u: ", m->cpu_rows[i].cpu_id);
        tw_str(&w, name);
        tw_pct(&w, &m->cpu_rows[i].pct);
        if (!m->cpu_rows[i].online)
            tw_str(&w, " (offline)");
        tw_newline(&w);
        line++;
    }

    /* Memory: label "PMM 占用", MiB, T == 0 unavailable. */
    if (!(budget && line >= budget)) {
        tw_str(&w, "PMM 占用: ");
        if (m->mem_valid) {
            tw_mib(&w, m->mem_used_bytes);
            tw_str(&w, " / ");
            tw_mib(&w, m->mem_total_bytes);
            tw_str(&w, " (");
            tw_pct(&w, &m->mem_pct);
            tw_str(&w, ")");
        } else {
            tw_str(&w, "--");
        }
        tw_newline(&w);
        line++;
    }

    if (!(budget && line >= budget)) {
        tw_temp(&w, m);
        tw_newline(&w);
        line++;
    }

    if (!(budget && line >= budget)) {
        tw_str(&w, "CPU hist: ");
        tw_hist(&w, &m->total_history);
        tw_newline(&w);
        line++;
    }

    if (!(budget && line >= budget)) {
        tw_str(&w, "  PID    CPU%  S  NAME");
        tw_newline(&w);
        line++;
    }

    for (uint32_t i = 0; i < m->nproc_rows; i++) {
        if (budget && line >= budget)
            break;
        const struct monitor_process_row *r = &m->proc_rows[i];
        char ps[12];
        char b[64];
        pct_to_str(ps, sizeof ps, &r->cpu);
        snprintf(b, sizeof b, "%6llu %7s  %c  ",
                 (unsigned long long)r->pid, ps, r->state ? r->state : '?');
        tw_str(&w, b);
        tw_ansi(&w, C_WHITE);
        tw_escape(&w, r->comm);
        tw_ansi(&w, C_RESET);
        if (r->stale) {
            tw_ansi(&w, C_YELLOW);
            tw_str(&w, " (expired)");
            tw_ansi(&w, C_RESET);
        }
        tw_newline(&w);
        line++;
    }

    return 0;
}

/* ── Terminal frame control ─────────────────────────────────────── */

void monitor_term_enter(FILE *out)
{
    fprintf(out, "%s", C_CLEAR_SCREEN);   /* clear + home */
    fprintf(out, "%s", C_HIDE_CURSOR);
}

void monitor_term_leave(FILE *out)
{
    fprintf(out, "%s", C_RESET);          /* drop any attribute */
    fprintf(out, "%s", C_SHOW_CURSOR);    /* cursor restored */
    fprintf(out, "%s", C_HOME);
}

void monitor_puts(FILE *out, const char *s)
{
    fprintf(out, "%s", s);
}
