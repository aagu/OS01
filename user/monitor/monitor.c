/*
 * user/monitor/monitor.c — monitor plan Task 8: initialization, sampling
 * loop, poll/deadline scheduling and the single terminal exit path
 * (spec §7; docs/superpowers/specs/2026-10-09-monitor-design.md).
 *
 * Scheduling (plan Global Constraints / Task 8 Step 3):
 *   - the deadline is CLOCK_MONOTONIC, advanced by exactly 1e9 ns per
 *     round; the clock is never derived from wall time;
 *   - poll() waits on stdin AND the next deadline; on EINTR the
 *     remaining time is recomputed from the clock (a signal handler
 *     only sets an exit flag);
 *   - a collection that overruns skips the missed periods forward with
 *     the REAL elapsed time — the deadline jumps past whole seconds and
 *     exactly one sample is taken, never a busy catch-up loop.  The
 *     differential model consumes each file's own SampleNs, so the
 *     displayed Δt is real regardless of scheduling jitter;
 *   - the poll timeout in milliseconds is rounded UP and cannot
 *     overflow (the remaining span is bounded by one period).
 *
 * Terminal lifecycle:
 *   - TTY mode: switch to alternate screen (?1049h), clear + home,
 *     hide the cursor, save the original termios and strip ICANON/ECHO
 *     keeping ISIG; signal handlers only set an exit flag;
 *   - non-TTY or --samples: plain text, no escape bytes;
 *   - ALL exit paths converge on one cleanup block that restores the
 *     terminal (show cursor, restore main screen ?1049l) exactly once
 *     and releases the collection and model.  If the original termios
 *     was never obtained it is never written back (no uninitialized
 *     copy is pushed to the driver).
 */

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/stat.h>     /* struct winsize (TIOCGWINSZ) */
#include <sys/time.h>

#include <monitor/collect.h>
#include <monitor/model.h>

#include "render.h"

#define MONITOR_PERIOD_NS 1000000000ULL   /* one sample per second */

/* Set by a signal handler, read by the loop; only a flag is touched in
 * signal context (spec §7, plan Task 8 Step 3). */
static volatile int g_exit_flag;

static void monitor_on_signal(int sig)
{
    (void)sig;
    g_exit_flag = 1;
}

static uint64_t now_ns(const struct timespec *ts)
{
    return (uint64_t)ts->tv_sec * 1000000000ULL + (uint64_t)ts->tv_nsec;
}

/* Parse a positive decimal count (no sign, no trailing junk).  Returns
 * 0 on success, -1 on any malformed input. */
static int parse_count(const char *s, int *out)
{
    if (!s || !*s)
        return -1;
    unsigned long v = 0;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9')
            return -1;
        v = v * 10UL + (unsigned long)(*p - '0');
        if (v > 1000000UL)
            return -1;
    }
    *out = (int)v;
    return 0;
}

int main(int argc, char **argv)
{
    int samples = -1;          /* -1 = run until EOF / signal */
    int rc = 0;
    int interactive = 0;

    g_exit_flag = 0;           /* fresh run: no stale signal state */
    int termios_obtained = 0;
    struct termios orig;
    unsigned rows = 0, cols = 0;

    /* ── arguments ─────────────────────────────────────────────── */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--samples") == 0) {
            if (i + 1 >= argc || parse_count(argv[i + 1], &samples) != 0) {
                monitor_puts(stdout,
                             "monitor: --samples requires a positive count\n");
                return 2;
            }
            i++;
        } else {
            monitor_puts(stdout, "monitor: unknown argument\n");
            return 2;
        }
    }

    struct monitor_model model;
    struct monitor_sample sample;
    monitor_model_init(&model);
    monitor_sample_init(&sample);

    /* ── terminal setup ────────────────────────────────────────── */
    interactive = (samples < 0) &&
                  isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);

    if (interactive) {
        if (tcgetattr(STDIN_FILENO, &orig) != 0) {
            /* Cannot read the original: never restore an uninitialized
             * copy — bail out before touching the terminal. */
            monitor_puts(stdout, "monitor: not a usable terminal\n");
            rc = 1;
            goto cleanup;
        }
        struct termios raw = orig;
        raw.c_lflag &= ~(tcflag_t)(ICANON | ECHO);   /* keep ISIG */
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        termios_obtained = 1;

        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
            cols = ws.ws_col;
            rows = ws.ws_row;
        } else {
            cols = 80;
            rows = 24;
        }

        monitor_term_enter(stdout);
        signal(SIGINT, monitor_on_signal);
        signal(SIGTERM, monitor_on_signal);
    }

    /* ── sampling loop ─────────────────────────────────────────── */
    {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
            /* Initialization failure AFTER terminal setup: still clean
             * up the terminal through the single exit path. */
            monitor_puts(stdout, "monitor: clock unavailable\n");
            rc = 1;
            goto cleanup;
        }
        uint64_t deadline = now_ns(&ts) + MONITOR_PERIOD_NS;
        unsigned rounds = 0;
        char inbuf[64];

        while (!g_exit_flag) {
            if (samples >= 0 && rounds >= (unsigned)samples)
                break;

            if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
                rc = 1;
                break;
            }
            uint64_t now = now_ns(&ts);

            if (now < deadline) {
                uint64_t rem = deadline - now;
                uint64_t ms = (rem + 999999ULL) / 1000000ULL;  /* round up */
                if (ms > 1000000ULL)
                    ms = 1000000ULL;                 /* no overflow / clamp */
                int timeout_ms = (int)ms;

                if (samples < 0) {
                    struct pollfd pfd;
                    pfd.fd = STDIN_FILENO;
                    pfd.events = POLLIN;
                    pfd.revents = 0;
                    int pr = poll(&pfd, 1, timeout_ms);
                    if (pr < 0) {
                        if (errno == EINTR)
                            continue;            /* recompute the deadline */
                        rc = 1;
                        break;
                    }
                    if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP | POLLERR))) {
                        int64_t n = read(STDIN_FILENO, inbuf, sizeof inbuf);
                        if (n <= 0)
                            break;               /* stdin EOF: leave */
                        if (interactive) {
                            int quit = 0;
                            for (int64_t i = 0; i < n; i++)
                                if (inbuf[i] == 'q' || inbuf[i] == 'Q')
                                    quit = 1;
                            if (quit)
                                break;
                        }
                    }
                } else {
                    /* --samples: plain finite run, no input channel. */
                    poll(NULL, 0, timeout_ms);
                }

                if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
                    rc = 1;
                    break;
                }
                now = now_ns(&ts);
                if (now < deadline)
                    continue;                    /* woke early: not yet due */
            }

            /* ── one sample round ─────────────────────────────────── */
            (void)monitor_collect(&sample);      /* failure => invalid round */
            (void)monitor_model_update(&model, &sample);
            monitor_render(stdout, &model, rows, cols);
            monitor_sample_destroy(&sample);
            rounds++;

            /* Advance the deadline by whole periods, skipping any
             * periods missed while sampling (real Δt, no catch-up). */
            deadline += MONITOR_PERIOD_NS;
            if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
                uint64_t t2 = now_ns(&ts);
                while (t2 >= deadline)
                    deadline += MONITOR_PERIOD_NS;
            }
        }
    }

cleanup:
    /* ── single exit path ──────────────────────────────────────── */
    if (interactive) {
        if (termios_obtained)
            tcsetattr(STDIN_FILENO, TCSANOW, &orig);
        monitor_term_leave(stdout);
    }
    monitor_sample_destroy(&sample);
    monitor_model_destroy(&model);
    return rc;
}
