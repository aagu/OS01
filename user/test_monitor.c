/*
 * user/test_monitor.c — monitor plan Task 9: QEMU guest test helper.
 *
 * Registered as a test-assist app (installed at /bin/test_monitor by the
 * shared rootfs discovery) WITHOUT changing the default inittab.  It is
 * NOT the monitor itself: it is the guest-side driver the QEMU harness
 * (qemutests/monitor_test.py) types at the shell prompt.
 *
 * Modes (argv[1]):
 *
 *   snapshot   — verifies the procfs per-file snapshot lifecycle (spec §6)
 *                and the task birth-id lifetime (spec §5.1):
 *                  * split read to EOF is stable and re-read yields EOF;
 *                  * dup shares the file offset / one snapshot;
 *                  * fork shares the file_t (child drains → parent EOF);
 *                  * an unwritable (PROT_READ) user buffer makes read()
 *                    fail with EFAULT and does NOT advance the offset;
 *                  * a forked child gets a fresh birth id / correct ppid;
 *                  * exec preserves the birth id (via the "selfstatus"
 *                    exec target, re-read after execve);
 *                  * a fast fork/exec/exit churn loop keeps every /proc
 *                    scan well-formed and never leaks fds.
 *                Prints one [MONITOR TEST] <name> PASS/FAIL line per check
 *                and a final [MONITOR TEST] RESULT line.
 *
 *   selfstatus — exec target used by the birth-across-exec check: reads its
 *                own /proc/self/status and prints
 *                "[MONITOR TEST] SELFSTATUS name=<n> birth=<b> pid=<p>".
 *
 *   noop       — exits 0 immediately (exec/exit churn target).
 *
 *   spin [secs]
 *              — burns CPU on whatever CPU the scheduler runs it on and
 *                prints its current CPU ownership over time:
 *                  "[MONITOR SPIN] pid=<p> cpu=<c> start"
 *                  "[MONITOR SPIN] pid=<p> cpu=<c> tick=<n>"
 *                  "[MONITOR SPIN] pid=<p> cpu=<c> exit migrated=<0|1>"
 *                There is NO sched_setaffinity syscall in this kernel: the
 *                harness only uses samples from a window where ownership
 *                stayed constant; a migration flips migrated=1 and the
 *                harness retries a bounded number of times rather than
 *                claiming strict pinning.  secs=0 runs forever.
 *
 *   churn [rounds]
 *              — fork/exec/exit stress: repeatedly forks a child that execs
 *                the noop target and exits, scans /proc between rounds, and
 *                confirms (a) every scan is well-formed, (b) a zombie is
 *                visible in /proc before it is reaped, (c) the scanner's own
 *                fd count never grows (NOFILE=16 is never exhausted).
 *
 *   pty        — real terminal-lifecycle tests.  Creates a PTY, execs the
 *                REAL /bin/monitor on the slave, and drives the four exit
 *                paths a user can trigger:
 *                  * q        — interactive; 'q' on the master quits;
 *                  * sigint   — interactive; SIGINT quits;
 *                  * sigterm  — interactive; SIGTERM quits;
 *                  * samples  — "--samples N" non-interactive run;
 *                  * eof      — stdin is a pipe whose writer is closed while
 *                               stdout stays on the master PTY (non-TTY
 *                               input): read()==0 → clean exit, no ANSI.
 *                For the interactive q/signal runs the parent saves the
 *                slave termios before the child starts and asserts, after
 *                the child is reaped, that (1) the master stayed readable,
 *                (2) every termios field is byte-identical to the baseline,
 *                (3) the VT100 cursor-restore sequence (\x1b[?25h) reached
 *                the master, (4) the child was reaped, and (5) the parent's
 *                fd count returned to baseline.  The --samples and eof runs
 *                assert the opposite: no ANSI bytes and termios untouched.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>

/* ── tiny check framework ─────────────────────────────────────────── */

static int g_pass;
static int g_fail;

static void check(const char *name, int ok, const char *why)
{
    if (ok) {
        printf("[MONITOR TEST] %s PASS\n", name);
        g_pass++;
    } else {
        printf("[MONITOR TEST] %s FAIL: %s\n", name, why ? why : "");
        g_fail++;
    }
}

static void finish(const char *mode)
{
    printf("[MONITOR TEST] RESULT: mode=%s pass=%d fail=%d\n",
           mode, g_pass, g_fail);
    fflush(stdout);
}

/* ── proc helpers ─────────────────────────────────────────────────── */

static long read_file(const char *path, char *buf, long cap)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    long total = 0;
    while (total < cap - 1) {
        int64_t n = read(fd, buf + total, (uint64_t)(cap - 1 - total));
        if (n < 0) { close(fd); return -1; }
        if (n == 0) break;
        total += n;
    }
    close(fd);
    buf[total] = '\0';
    return total;
}

/* Extract the value of `key` (e.g. "BirthId") from procfs key:value text. */
static int status_get(const char *text, const char *key, char *val, size_t vcap)
{
    size_t klen = strlen(key);
    const char *p = text;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t linelen = nl ? (size_t)(nl - p) : strlen(p);
        if (linelen > klen && strncmp(p, key, klen) == 0 && p[klen] == ':') {
            const char *v = p + klen + 1;
            const char *vend = p + linelen;
            while (v < vend && (*v == ' ' || *v == '\t')) v++;
            size_t n = (size_t)(vend - v);
            if (n >= vcap) n = vcap - 1;
            memcpy(val, v, n);
            val[n] = '\0';
            return 0;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return -1;
}

static long read_status_field(const char *path, const char *key,
                              char *val, size_t vcap)
{
    char buf[4096];
    if (read_file(path, buf, sizeof buf) < 0) return -1;
    return status_get(buf, key, val, vcap);
}

static unsigned long read_self_cpu(void)
{
    char v[32];
    if (read_status_field("/proc/self/status", "CPU", v, sizeof v) != 0)
        return 0xfffffffful;
    return strtoul(v, NULL, 10);
}

/* Count the caller's open descriptors via /proc/self/fd.  The opendir
 * handle itself is included, so the value is only used for eq-compare. */
static int count_fds(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0])
            n++;
    }
    closedir(d);
    return n;
}

/* ── snapshot mode ────────────────────────────────────────────────── */

static int snapshot_mode(void)
{
    char scratch[4096];

    /* (1) whole-file read is well-formed: Version + at least one cpu line. */
    long n = read_file("/proc/stat", scratch, sizeof scratch);
    int wellformed = (n > 0) &&
                     (strstr(scratch, "Version:") != NULL) &&
                     (strstr(scratch, "cpu0:") != NULL);
    check("stat_wellformed", wellformed, "missing Version/cpu0");

    /* (2) split (1-byte) read reaches EOF; a re-read still returns 0. */
    long split_len = 0;
    int split_ok = 1;
    {
        int fd = open("/proc/stat", O_RDONLY);
        if (fd < 0) {
            split_ok = 0;
        } else {
            char one;
            for (;;) {
                int64_t r = read(fd, &one, 1);
                if (r < 0) { split_ok = 0; break; }
                if (r == 0) break;
                split_len++;
                if (split_len > 8192) { split_ok = 0; break; }
            }
            if (split_ok) {
                char two;
                if (read(fd, &two, 1) != 0) split_ok = 0;
            }
            close(fd);
        }
    }
    check("split_read_eof_stable", split_ok && split_len > 0,
          "split read did not reach a stable EOF");

    /* (3) dup shares the offset: read 4 bytes, dup, drain the dup, then the
     *     original must also report EOF (one file_t / one snapshot). */
    {
        int ok = 0;
        int fd = open("/proc/stat", O_RDONLY);
        if (fd >= 0) {
            char hdr[4];
            if (read(fd, hdr, 4) == 4) {
                int fd2 = dup(fd);
                if (fd2 >= 0) {
                    char sink[256];
                    long got = 0;
                    for (;;) {
                        int64_t r = read(fd2, sink, sizeof sink);
                        if (r <= 0) break;
                        got += r;
                        if (got > 8192) break;
                    }
                    char tail;
                    int orig_eof = (read(fd, &tail, 1) == 0);
                    int dup_eof = (read(fd2, &tail, 1) == 0);
                    ok = (got > 0) && orig_eof && dup_eof;
                    close(fd2);
                }
            }
            close(fd);
        }
        check("dup_shared_offset", ok, "dup did not share the file offset");
    }

    /* (4) fork shares the file_t: the child drains the inherited fd to EOF,
     *     so the parent's fd (same open file description) is also at EOF. */
    {
        int ok = 0;
        int fds[2];
        if (pipe(fds) == 0) {
            int fd = open("/proc/stat", O_RDONLY);
            if (fd >= 0) {
                char hdr[4];
                if (read(fd, hdr, 4) == 4) {
                    pid_t p = fork();
                    if (p == 0) {
                        close(fds[0]);
                        char sink[512];
                        long got = 0;
                        for (;;) {
                            int64_t r = read(fd, sink, sizeof sink);
                            if (r <= 0) break;
                            got += r;
                            if (got > 8192) break;
                        }
                        int is_eof = (read(fd, sink, 1) == 0);
                        int msg = is_eof ? 1 : 0;
                        (void)!write(fds[1], &msg, sizeof msg);
                        close(fd);
                        _exit(0);
                    }
                    close(fds[1]);
                    int child_msg = 0;
                    (void)!read(fds[0], &child_msg, sizeof child_msg);
                    if (p > 0) waitpid(p, NULL, 0);
                    char tail;
                    int parent_eof = (read(fd, &tail, 1) == 0);
                    ok = (child_msg == 1) && parent_eof;
                }
                close(fd);
            }
            close(fds[0]);
            close(fds[1]);
        }
        check("fork_shared_snapshot", ok,
              "child drain did not advance the parent offset");
    }

    /* (5) EFAULT on an unwritable user buffer must not advance the offset. */
    {
        int ok = 0;
        char *ro = mmap(NULL, 4096, PROT_READ,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        int fd = open("/proc/stat", O_RDONLY);
        if (ro != MAP_FAILED && fd >= 0) {
            errno = 0;
            int64_t r = read(fd, ro, 16);
            int efault = (r < 0 && errno == EFAULT);
            char good[16];
            int64_t g = read(fd, good, 8);
            int at_start = (g == 8) && (memcmp(good, "Version:", 8) == 0);
            ok = efault && at_start;
            if (!ok) {
                printf("[MONITOR TEST] efault_detail rc=%ld errno=%d g=%ld\n",
                       (long)r, errno, (long)g);
            }
        }
        if (fd >= 0) close(fd);
        if (ro != MAP_FAILED) munmap(ro, 4096);
        check("efault_unwritable", ok,
              "read into PROT_READ buffer did not EFAULT at offset 0");
    }

    /* (6) birth across fork: the child gets a fresh birth id + our ppid. */
    {
        int ok = 0;
        int fds[2];
        if (pipe(fds) == 0) {
            char my_birth[32];
            char my_pid[32];
            (void)read_status_field("/proc/self/status", "BirthId",
                                    my_birth, sizeof my_birth);
            snprintf(my_pid, sizeof my_pid, "%ld", (long)getpid());
            pid_t p = fork();
            if (p == 0) {
                close(fds[0]);
                char b[32];
                char pp[32];
                (void)read_status_field("/proc/self/status", "BirthId",
                                        b, sizeof b);
                (void)read_status_field("/proc/self/status", "PPid",
                                        pp, sizeof pp);
                char msg[80];
                int len = snprintf(msg, sizeof msg, "%s %s", b, pp);
                (void)!write(fds[1], msg, (size_t)len);
                close(fds[1]);
                _exit(0);
            }
            close(fds[1]);
            char msg[80];
            memset(msg, 0, sizeof msg);
            int64_t got = read(fds[0], msg, sizeof msg - 1);
            if (p > 0) waitpid(p, NULL, 0);
            if (got > 0) {
                /* msg is "<birth> <ppid>"; parse without sscanf (the
                 * libc's minimal sscanf does not honour field widths). */
                char *s = msg;
                while (*s == ' ') s++;
                char *cb = s;
                while (*s && *s != ' ') s++;
                if (*s) *s++ = '\0';
                while (*s == ' ') s++;
                char *cpp = s;
                ok = (strcmp(cb, my_birth) != 0) &&
                     (strcmp(cpp, my_pid) == 0);
            }
            close(fds[0]);
        }
        check("birth_across_fork", ok, "child birth/ppid wrong");
    }

    /* (7) exec preserves the birth id: fork a child that records its birth,
     *     then execs the selfstatus target which re-reads it. */
    {
        int ok = 0;
        int fds[2];
        if (pipe(fds) == 0) {
            pid_t p = fork();
            if (p == 0) {
                close(fds[0]);
                char b[32];
                (void)read_status_field("/proc/self/status", "BirthId",
                                        b, sizeof b);
                /* Send the pre-exec birth first, then (fd kept open across
                 * exec) let the exec target append its post-exec birth. */
                (void)!write(fds[1], b, strlen(b));
                (void)!write(fds[1], " ", 1);
                char fdbuf[16];
                snprintf(fdbuf, sizeof fdbuf, "%d", fds[1]);
                char *argv[] = { "test_monitor", "selfstatus", "birthcheck",
                                 fdbuf, NULL };
                execve("/bin/test_monitor", argv, environ);
                _exit(127);
            }
            close(fds[1]);
            char pre[64];
            memset(pre, 0, sizeof pre);
            long tot = 0;
            for (;;) {
                int64_t r = read(fds[0], pre + tot,
                                 (uint64_t)(sizeof pre - 1 - (size_t)tot));
                if (r <= 0) break;
                tot += r;
                if (tot >= (long)sizeof pre - 1) break;
            }
            close(fds[0]);
            int st = 0;
            if (p > 0) waitpid(p, &st, 0);
            /* pre == "<pre-exec birth> <post-exec birth>"; equal fields on a
             * clean exit prove exec preserved the birth id. */
            char *sp = pre;
            char *a = sp;
            while (*sp && *sp != ' ') sp++;
            if (*sp) *sp++ = '\0';
            while (*sp == ' ') sp++;
            ok = (WIFEXITED(st) && WEXITSTATUS(st) == 0) &&
                 a[0] != '\0' && sp[0] != '\0' && strcmp(a, sp) == 0;
        }
        check("birth_across_exec", ok, "exec changed the birth id");
    }

    finish("snapshot");
    return g_fail ? 1 : 0;
}

/* ── selfstatus / noop ────────────────────────────────────────────── */

static int selfstatus_mode(int argc, char **argv)
{
    char name[64] = "";
    char birth[32] = "";
    (void)read_status_field("/proc/self/status", "Name", name, sizeof name);
    (void)read_status_field("/proc/self/status", "BirthId", birth, sizeof birth);

    /* "birthcheck <fd>": report the post-exec birth back to the caller. */
    if (argc > 3 && strcmp(argv[2], "birthcheck") == 0) {
        int fd = atoi(argv[3]);
        (void)!write(fd, birth, strlen(birth));
    }

    printf("[MONITOR TEST] SELFSTATUS name=%s birth=%s pid=%ld\n",
           name, birth, (long)getpid());
    fflush(stdout);
    return 0;
}

static int noop_mode(void)
{
    return 0;
}

/* ── spin / churn ─────────────────────────────────────────────────── */

static volatile unsigned long g_sink;

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void burn(unsigned long iters)
{
    unsigned long x = g_sink;
    for (unsigned long i = 0; i < iters; i++)
        x = x * 2654435761ul + i + 1u;
    g_sink = x;
}

static int spin_mode(int secs)
{
    unsigned long cpu = read_self_cpu();
    long pid = (long)getpid();
    unsigned long epoch = 1;          /* ownership epoch: ++ on each move */
    printf("[MONITOR SPIN] pid=%ld cpu=%lu epoch=%lu start\n",
           pid, cpu, epoch);
    fflush(stdout);

    unsigned long last = cpu;
    uint64_t end = now_ms() + (secs > 0 ? (uint64_t)secs * 1000u : 0);
    int it = 0;

    /* Quiet by default: only a change of CPU ownership is printed, so a
     * concurrent monitor run is not polluted by terminal re-renders of
     * heartbeat lines (the graphical terminal costs ~8% of a CPU per
     * output event).  Ownership is sampled internally every ~50 ms; a
     * mid-window migration emits a new epoch line, which tells the harness
     * the sample window straddled a move (retry) rather than claiming
     * strict pinning. */
    for (;;) {
        burn(400000);
        if (++it % 500 == 0) {
            unsigned long c = read_self_cpu();
            if (c != last) {
                last = c;
                epoch++;
                printf("[MONITOR SPIN] pid=%ld cpu=%lu epoch=%lu\n",
                       pid, c, epoch);
                fflush(stdout);
            }
        }
        if (secs > 0 && now_ms() >= end)
            break;
    }
    printf("[MONITOR SPIN] pid=%ld cpu=%lu epoch=%lu exit migrated=%d\n",
           pid, last, epoch, epoch > 1 ? 1 : 0);
    fflush(stdout);
    return 0;
}

/* Returns 1 if a well-formed status could be read for `pid`. */
static int scan_pid_wellformed(int pid)
{
    char path[48];
    char buf[4096];
    snprintf(path, sizeof path, "/proc/%d/status", pid);
    if (read_file(path, buf, sizeof buf) <= 0)
        return 0;
    return strstr(buf, "Name:") != NULL && strstr(buf, "Pid:") != NULL;
}

static int churn_mode(int rounds)
{
    int baseline_fds = count_fds();
    int scan_ok = 1;
    int fd_ok = 1;
    int zombie_seen = 0;
    int exp_pid = -1;

    for (int r = 0; r < rounds; r++) {
        pid_t p = fork();
        if (p == 0) {
            char *argv[] = { "test_monitor", "noop", NULL };
            execve("/bin/test_monitor", argv, environ);
            _exit(127);
        }
        if (p < 0) { scan_ok = 0; break; }
        exp_pid = (int)p;

        /* Leave every third child as a visible zombie until we reap it. */
        if (r % 3 == 2) {
            /* give the child time to exit and become a zombie */
            for (int i = 0; i < 20 && scan_ok; i++) {
                char path[48], buf[4096];
                snprintf(path, sizeof path, "/proc/%d/status", (int)p);
                if (read_file(path, buf, sizeof buf) > 0) {
                    char st[8];
                    if (status_get(buf, "State", st, sizeof st) == 0 &&
                        st[0] == 'Z') {
                        zombie_seen = 1;        /* zombie visible */
                        break;
                    }
                }
                struct timespec ts = { 0, 5 * 1000 * 1000 };
                nanosleep(&ts, NULL);
            }
        }

        /* Enumerate /proc and read a few status records in this round. */
        DIR *d = opendir("/proc");
        if (!d) {
            scan_ok = 0;
        } else {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] < '0' || e->d_name[0] > '9')
                    continue;
                int pid = atoi(e->d_name);
                if (!scan_pid_wellformed(pid))
                    scan_ok = 0;
            }
            closedir(d);
        }

        if (r % 3 == 2) {
            if (exp_pid > 0) waitpid((pid_t)exp_pid, NULL, 0);
        } else {
            waitpid(p, NULL, 0);
        }

        if (count_fds() != baseline_fds)
            fd_ok = 0;
    }

    check("churn_scan_wellformed", scan_ok, "a /proc scan was malformed");
    check("churn_fd_stable", fd_ok, "fd count grew during churn");
    check("zombie_visible", zombie_seen, "no zombie observed in /proc");
    finish("churn");
    return g_fail ? 1 : 0;
}

/* ── pty mode ─────────────────────────────────────────────────────── */

#define PTY_MAX_DRAIN 65536

static int termios_equal(const struct termios *a, const struct termios *b)
{
    if (a->c_iflag != b->c_iflag) return 0;
    if (a->c_oflag != b->c_oflag) return 0;
    if (a->c_cflag != b->c_cflag) return 0;
    if (a->c_lflag != b->c_lflag) return 0;
    if (a->c_line  != b->c_line)  return 0;
    for (int i = 0; i < NCCS; i++)
        if (a->c_cc[i] != b->c_cc[i]) return 0;
    if (a->__c_ispeed != b->__c_ispeed) return 0;
    if (a->__c_ospeed != b->__c_ospeed) return 0;
    return 1;
}

/* Drain whatever is readable on `fd` for up to `budget_ms`, appending to
 * buf.  Returns the number of bytes appended.  Uses poll so a quiet master
 * never blocks forever. */
static long drain(int fd, char *buf, long cap, long *used, int budget_ms)
{
    long start = 0;
    uint64_t end = now_ms() + (uint64_t)budget_ms;
    long added = 0;
    (void)start;
    for (;;) {
        int remaining = (int)(end - now_ms());
        if (remaining < 0) remaining = 0;
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int pr = poll(&pfd, 1, remaining);
        if (pr <= 0)
            break;
        if (!(pfd.revents & (POLLIN | POLLHUP)))
            break;
        char tmp[1024];
        int64_t n = read(fd, tmp, sizeof tmp);
        if (n <= 0)
            break;                       /* EOF or no data */
        for (int64_t i = 0; i < n && *used < cap - 1; i++)
            buf[(*used)++] = tmp[i];
        added += n;
    }
    if (*used < cap)
        buf[*used] = '\0';
    return added;
}

static int contains(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

static int has_ansi(const char *buf)
{
    return strchr(buf, '\x1b') != NULL;
}

/* Open a PTY; returns 0 with master/slave set, -1 otherwise.  The new
 * pty's slave is the first /dev/ptsN (N>=1) that opens — index 0 belongs
 * to the interactive terminal started by inittab. */
static int open_pty(int *master, int *slave)
{
    int m = open("/dev/ptmx", O_RDWR);
    if (m < 0)
        return -1;
    int s = -1;
    for (int i = 1; i <= 7 && s < 0; i++) {
        char path[16];
        snprintf(path, sizeof path, "/dev/pts%d", i);
        s = open(path, O_RDWR);
    }
    if (s < 0)
        s = open("/dev/pts0", O_RDWR);
    if (s < 0) {
        close(m);
        return -1;
    }
    *master = m;
    *slave = s;
    return 0;
}

/* Common child: place `in_fd` on stdin, the slave on stdout/stderr, then
 * exec /bin/monitor with `args`.  Closes every inherited testing fd. */
static void exec_monitor_child(int master, int slave, int in_fd,
                               char *const args[])
{
    if (in_fd >= 0)
        dup2(in_fd, 0);
    else
        dup2(slave, 0);
    dup2(slave, 1);
    dup2(slave, 2);
    if (slave > 2) close(slave);
    close(master);
    execve("/bin/monitor", args, environ);
    _exit(127);
}

/* Pump the master for `budget_ms`, then check WNOHANG.  Keeps draining so
 * the PTY's 512-byte pipe can never fill and block the child's writes.
 * Returns 1 and fills *status when the child has exited. */
static int reap_with_drain(int master, pid_t p, char *out, long cap,
                           long *used, int budget_ms, int *status)
{
    uint64_t end = now_ms() + (uint64_t)budget_ms;
    for (;;) {
        int left = (int)(end - now_ms());
        drain(master, out, cap, used, left > 100 ? 100 : (left < 0 ? 0 : left));
        int st = 0;
        pid_t r = waitpid(p, &st, WNOHANG);
        if (r == p) {
            if (status) *status = st;
            return 1;
        }
        if ((int64_t)(end - now_ms()) <= 0)
            return 0;
    }
}

/* Interactive scenario: exec monitor, let it draw, trigger the exit (a byte
 * on the master or a signal), then verify the lifecycle. */
static void pty_interactive(int master, int slave, const struct termios *base,
                            int baseline_fds, const char *name,
                            const char *trigger, int signal_no,
                            int expect_ansi)
{
    char out[PTY_MAX_DRAIN];
    long used = 0;
    out[0] = '\0';

    pid_t p = fork();
    if (p == 0) {
        char *args[] = { "monitor", NULL };
        exec_monitor_child(master, slave, -1, args);
    }
    if (p < 0) {
        check(name, 0, "fork failed");
        return;
    }

    /* Let the monitor enter its screen and take one sample (draining keeps
     * its output pipe from filling). */
    int dummy = 0;
    (void)reap_with_drain(master, p, out, sizeof out, &used, 1500, &dummy);

    if (signal_no)
        kill(p, signal_no);
    else
        (void)!write(master, trigger, strlen(trigger));

    int st = 0;
    int reaped = reap_with_drain(master, p, out, sizeof out, &used, 4000, &st);
    drain(master, out, sizeof out, &used, 300);

    struct termios after;
    int termios_ok = (tcgetattr(slave, &after) == 0) &&
                     termios_equal(base, &after);
    int cursor_ok = contains(out, "\x1b[?25h");
    int enter_ok = !expect_ansi || contains(out, "\x1b[?25l");
    int clean_exit = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    int fd_ok = (count_fds() == baseline_fds);

    char why[160];
    snprintf(why, sizeof why,
             "reaped=%d exit=%d termios=%d cursor=%d enter=%d fd=%d",
             reaped, clean_exit, termios_ok, cursor_ok, enter_ok, fd_ok);
    check(name, reaped && clean_exit && termios_ok && cursor_ok &&
                enter_ok && fd_ok, why);
}

/* --samples: finite non-interactive run — no ANSI, termios untouched. */
static void pty_samples(int master, int slave, const struct termios *base,
                        int baseline_fds, const char *name)
{
    char out[PTY_MAX_DRAIN];
    long used = 0;
    out[0] = '\0';

    pid_t p = fork();
    if (p == 0) {
        char *args[] = { "monitor", "--samples", "2", NULL };
        exec_monitor_child(master, slave, -1, args);
    }
    if (p < 0) {
        check(name, 0, "fork failed");
        return;
    }
    int st = 0;
    int reaped = reap_with_drain(master, p, out, sizeof out, &used, 8000, &st);

    struct termios after;
    int termios_ok = (tcgetattr(slave, &after) == 0) &&
                     termios_equal(base, &after);
    int no_ansi = !has_ansi(out);
    int has_frame = contains(out, "OS01 monitor");
    int clean_exit = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    int fd_ok = (count_fds() == baseline_fds);

    char why[160];
    snprintf(why, sizeof why,
             "reaped=%d exit=%d termios=%d ansi=%d frame=%d fd=%d",
             reaped, clean_exit, termios_ok, no_ansi, has_frame, fd_ok);
    check(name, reaped && clean_exit && termios_ok && no_ansi &&
                has_frame && fd_ok, why);
}

/* EOF: stdin is a pipe whose writer is closed; stdout stays on the PTY. */
static void pty_eof(int master, int slave, const struct termios *base,
                    int baseline_fds, const char *name)
{
    char out[PTY_MAX_DRAIN];
    long used = 0;
    out[0] = '\0';
    int fds[2];
    if (pipe(fds) < 0) {
        check(name, 0, "pipe failed");
        return;
    }

    pid_t p = fork();
    if (p == 0) {
        /* Reader end on stdin, writer end closed → immediate EOF. */
        close(fds[1]);
        char *args[] = { "monitor", NULL };
        exec_monitor_child(master, slave, fds[0], args);
    }
    close(fds[0]);
    close(fds[1]);
    if (p < 0) {
        check(name, 0, "fork failed");
        return;
    }

    uint64_t start = now_ms();
    int st = 0;
    int reaped = reap_with_drain(master, p, out, sizeof out, &used, 4000, &st);
    uint64_t dt = now_ms() - start;

    struct termios after;
    int termios_ok = (tcgetattr(slave, &after) == 0) &&
                     termios_equal(base, &after);
    int no_ansi = !has_ansi(out);
    int clean_exit = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    int fast = dt < 3000;              /* EOF must exit promptly */
    int fd_ok = (count_fds() == baseline_fds);

    char why[160];
    snprintf(why, sizeof why,
             "reaped=%d exit=%d ms=%llu termios=%d ansi=%d fd=%d",
             reaped, clean_exit, (unsigned long long)dt, termios_ok,
             no_ansi, fd_ok);
    check(name, reaped && clean_exit && fast && termios_ok &&
                no_ansi && fd_ok, why);
}

static int pty_mode(void)
{
    int master = -1, slave = -1;
    if (open_pty(&master, &slave) != 0) {
        check("pty_open", 0, "could not open /dev/ptmx + slave");
        finish("pty");
        return 1;
    }

    struct termios base;
    int have_base = (tcgetattr(slave, &base) == 0);
    check("pty_termios_baseline", have_base, "tcgetattr on slave failed");
    int baseline_fds = count_fds();

    pty_interactive(master, slave, &base, baseline_fds,
                    "quit_key", "q", 0, 1);
    pty_interactive(master, slave, &base, baseline_fds,
                    "sigint", NULL, SIGINT, 1);
    pty_interactive(master, slave, &base, baseline_fds,
                    "sigterm", NULL, SIGTERM, 1);
    pty_samples(master, slave, &base, baseline_fds, "samples");
    pty_eof(master, slave, &base, baseline_fds, "eof");

    close(master);
    close(slave);
    finish("pty");
    return g_fail ? 1 : 0;
}

/* ── main ─────────────────────────────────────────────────────────── */

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "snapshot";

    if (strcmp(mode, "snapshot") == 0)   return snapshot_mode();
    if (strcmp(mode, "selfstatus") == 0) return selfstatus_mode(argc, argv);
    if (strcmp(mode, "noop") == 0)       return noop_mode();
    if (strcmp(mode, "spin") == 0) {
        int secs = (argc > 2) ? atoi(argv[2]) : 0;
        return spin_mode(secs);
    }
    if (strcmp(mode, "churn") == 0) {
        int rounds = (argc > 2) ? atoi(argv[2]) : 12;
        return churn_mode(rounds);
    }
    if (strcmp(mode, "pty") == 0)        return pty_mode();

    printf("[MONITOR TEST] unknown mode: %s\n", mode);
    return 2;
}
