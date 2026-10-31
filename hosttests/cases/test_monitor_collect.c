/*
 * test_monitor_collect.c — monitor plan Task 6: user-space collection
 * and strict parsing of the procfs statistics files (spec §4.1, §5.2,
 * §6; docs/superpowers/specs/2026-10-09-monitor-design.md).
 *
 * Drives the PRODUCTION user/monitor/collect.c (host-compiled) against
 * a fixture-owned fake procfs.  The production TU is compiled with
 * -D open/read/close/opendir/readdir/closedir/malloc/free mapped to
 * mon_test_* shims, so the fixture can:
 *   - serve per-path text (or -ENOENT) and chunk reads,
 *   - count open fds to enforce the NOFILE=16 discipline
 *     (baseline + directory handle + one status file, never more),
 *   - fail allocations to prove OOM rolls the round back with -ENOMEM.
 *
 * Asserted (spec §6 validity rules: required fields missing, duplicate
 * recognized field, numeric overflow or illegal value -> -EINVAL;
 * unknown fields ignored; field order / whitespace count irrelevant;
 * Version must be 1):
 *   - /proc/stat:   multi-core parse, valid=0 cores kept, unknown keys
 *     and out-of-order fields tolerated; duplicate key/line, overflow,
 *     missing field, out-of-range cpu index, online!=0/1 rejected;
 *   - /proc/meminfo: kB -> bytes conversion (T/U/F), T=0 invalid;
 *   - /proc/<pid>/status: duplicate BirthId -> -EINVAL, Name decoding
 *     (only \\ and \xHH accepted; incomplete/unknown escape, decoded
 *     NUL and >63-byte names rejected; ':' stays inside the value),
 *     decoded name is a COPY (input text can be wiped afterwards);
 *   - /proc/thermal: fixed reason table per Task 4, unknown reason
 *     keeps an unknown marker (never mistaken for normal), State
 *     decides usability, ok requires TempMilliC + CpuId;
 *   - monitor_collect: PID strings all-decimal and representable,
 *     duplicate PIDs deduplicated, vanished PIDs (ENOENT) skipped, a
 *     single invalid status file only drops that process, overlong
 *     files invalid, per-PID open->read->close before the next open
 *     with max_open_fds <= baseline_open_fds + 2, OOM frees the
 *     round's array and returns -ENOMEM, destroy frees exactly once
 *     and is idempotent.
 */

#include "test_framework.h"

#include <assert.h>
#include <dirent.h>
#include <errno.h>

/* OS01's errno.h is non-TLS; on the host we keep the shims' errno in a
 * fixture-local variable (the production code never reads errno, it
 * only switches on open()'s -1 return). */
static int fake_errno;
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <monitor/collect.h>

/* ── Production seam: the collect TU is compiled with the libc calls
 * renamed onto these shims (see MONITOR_COLLECT_CFLAGS in the
 * hosttests Makefile).  Signatures match OS01's libc declarations. ── */

int         mon_test_open(const char *path, int flags, ...);
int         mon_test_close(int fd);
int64_t     mon_test_read(int fd, void *buf, uint64_t size);
DIR        *mon_test_opendir(const char *name);
struct dirent *mon_test_readdir(DIR *dir);
int         mon_test_closedir(DIR *dir);
void       *mon_test_malloc(size_t size);
void        mon_test_free(void *ptr);

/* ── Fake file table ─────────────────────────────────────────────── */

#define FAKE_MAX_FILES 48
#define FAKE_MAX_FD    16
#define FAKE_MAX_STATUS_OPENS 64

struct fake_file {
    const char *path;
    const char *text;
    size_t len;
    size_t chunk;        /* 0 = serve the whole file in one read */
    int    missing;      /* open -> -ENOENT (process vanished) */
};

static struct fake_file fake_files[FAKE_MAX_FILES];
static int fake_file_count;

static int  fd_slots[FAKE_MAX_FD];   /* -1 free, -2 external, -4 dir */
static size_t fd_offset[FAKE_MAX_FD];
static int  open_fds;
static int  max_open_fds;
static int  baseline_open_fds;

/* every open of a /proc/<pid>/status records the fd-watermark at open
 * time — the close-before-next-open discipline check reads this */
static int status_opens;
static int open_fds_at_status_open[FAKE_MAX_STATUS_OPENS];

/* fake /proc directory: the name list the next opendir serves */
static const char *dir_names[FAKE_MAX_FILES];
static int dir_name_count;
static DIR *dir_open_dir;
static int  dir_open_idx;

/* ── malloc shim state: fail injection + balance (collect TU only) ── */

static long mon_malloc_balance;
static int  mon_malloc_fail_after;   /* <0 never fail; 0 fail now; n after n */

/* ── Fixture helpers ─────────────────────────────────────────────── */

static void fixture_reset(int baseline)
{
    fake_file_count = 0;
    memset(fake_files, 0, sizeof(fake_files));
    memset(open_fds_at_status_open, 0, sizeof(open_fds_at_status_open));
    status_opens = 0;

    assert_true(baseline >= 0 && baseline < FAKE_MAX_FD);
    for (int i = 0; i < FAKE_MAX_FD; i++)
        fd_slots[i] = (i < baseline) ? -2 : -1;
    baseline_open_fds = baseline;
    open_fds = baseline;
    max_open_fds = baseline;

    dir_name_count = 0;
    dir_open_dir = NULL;
    dir_open_idx = 0;

    mon_malloc_balance = 0;
    mon_malloc_fail_after = -1;
}

static struct fake_file *fixture_add(const char *path, const char *text)
{
    assert_true(fake_file_count < FAKE_MAX_FILES);
    struct fake_file *f = &fake_files[fake_file_count++];
    memset(f, 0, sizeof(*f));
    f->path = path;
    f->text = text;
    f->len = strlen(text);
    return f;
}

static void fixture_dir(const char *const *names, int count)
{
    assert_true(count >= 0 && count <= FAKE_MAX_FILES);
    for (int i = 0; i < count; i++)
        dir_names[i] = names[i];
    dir_name_count = count;
}

/* ── Shim implementations ────────────────────────────────────────── */

static struct fake_file *fake_find(const char *path)
{
    for (int i = 0; i < fake_file_count; i++)
        if (strcmp(fake_files[i].path, path) == 0)
            return &fake_files[i];
    return NULL;
}

int mon_test_open(const char *path, int flags, ...)
{
    (void)flags;
    struct fake_file *f = fake_find(path);
    if (!f || f->missing) {
        fake_errno = ENOENT;
        return -1;
    }
    int fd = -1;
    for (int i = 0; i < FAKE_MAX_FD; i++) {
        if (fd_slots[i] == -1) { fd = i; break; }
    }
    if (fd < 0) {
        fake_errno = EMFILE;
        return -1;
    }
    fd_slots[fd] = (int)(f - fake_files);
    fd_offset[fd] = 0;
    open_fds++;
    if (open_fds > max_open_fds)
        max_open_fds = open_fds;

    /* record the discipline evidence for status files */
    if (strncmp(path, "/proc/", 6) == 0 &&
        strcmp(path + strlen(path) - 7, "/status") == 0) {
        assert_true(status_opens < FAKE_MAX_STATUS_OPENS);
        open_fds_at_status_open[status_opens++] = open_fds;
    }
    return fd;
}

int mon_test_close(int fd)
{
    if (fd < 0 || fd >= FAKE_MAX_FD || fd_slots[fd] < 0) {
        fake_errno = EBADF;
        return -1;
    }
    fd_slots[fd] = -1;
    open_fds--;
    return 0;
}

int64_t mon_test_read(int fd, void *buf, uint64_t size)
{
    if (fd < 0 || fd >= FAKE_MAX_FD || fd_slots[fd] < 0)
        return -1;
    struct fake_file *f = &fake_files[fd_slots[fd]];
    size_t remaining = f->len - fd_offset[fd];
    size_t limit = f->chunk ? f->chunk : remaining;
    size_t n = remaining < size ? remaining : size;
    if (n > limit)
        n = limit;
    memcpy(buf, f->text + fd_offset[fd], n);
    fd_offset[fd] += n;
    return (int64_t)n;
}

DIR *mon_test_opendir(const char *name)
{
    if (strcmp(name, "/proc") != 0) {
        fake_errno = ENOENT;
        return NULL;
    }
    int fd = -1;
    for (int i = 0; i < FAKE_MAX_FD; i++) {
        if (fd_slots[i] == -1) { fd = i; break; }
    }
    if (fd < 0) {
        fake_errno = EMFILE;
        return NULL;
    }
    fd_slots[fd] = -4;               /* directory handle */
    open_fds++;
    if (open_fds > max_open_fds)
        max_open_fds = open_fds;

    DIR *d = malloc(sizeof(*d));     /* real malloc: not collect's seam */
    assert_not_null(d);
    memset(d, 0, sizeof(*d));
    d->fd = fd;
    dir_open_dir = d;
    dir_open_idx = 0;
    return d;
}

struct dirent *mon_test_readdir(DIR *dir)
{
    if (!dir || dir != dir_open_dir)
        return NULL;
    if (dir_open_idx >= dir_name_count)
        return NULL;
    const char *name = dir_names[dir_open_idx++];
    snprintf(dir->result.d_name, sizeof(dir->result.d_name), "%s", name);
    return &dir->result;
}

int mon_test_closedir(DIR *dir)
{
    if (!dir || dir != dir_open_dir) {
        fake_errno = EBADF;
        return -1;
    }
    fd_slots[dir->fd] = -1;
    open_fds--;
    dir_open_dir = NULL;
    free(dir);
    return 0;
}

void *mon_test_malloc(size_t size)
{
    if (mon_malloc_fail_after == 0)
        return NULL;
    if (mon_malloc_fail_after > 0)
        mon_malloc_fail_after--;
    void *p = malloc(size ? size : 1);
    if (p)
        mon_malloc_balance++;
    return p;
}

void mon_test_free(void *ptr)
{
    if (ptr) {
        free(ptr);
        mon_malloc_balance--;
    }
}

/* ── Shared text fixtures (byte-matched to kernel/fs/procfs_fmt.c) ── */

static const char stat_text[] =
    "Version:\t1\n"
    "SampleNs:  23500000000\n"
    "ExtraKey:  whatever\n"
    "TickHz:  100\n"
    "cpu1:  online=1  valid=1  epoch=7  busy=300  idle=2050\n"
    "cpu0: online=1 valid=1 epoch=7 busy=1200 idle=1150\n"
    "cpu2:  online=0 valid=0 epoch=0 busy=0 idle=0\n";

static const char meminfo_text[] =
    "Version:  1\n"
    "SampleNs: 777\n"
    "PageSize:  2048 kB\n"
    "MemTotal:  6144 kB\n"
    "MemFree:  4096 kB\n"
    "MemUsed:  2048 kB\n";

static const char thermal_text[] =
    "Version:  1\n"
    "State:  unsupported\n"
    "Reason:  backend_unavailable\n"
    "Source:  none\n"
    "SampleNs:  23500000000\n";

/* out-of-order fields + extra whitespace + unknown keys */
static const char status_text_42[] =
    "  RuntimeTicks:   27\n"
    "Version:  1\n"
    "   Name:\tmon\\x0aitor\n"
    "Priority:\t5\n"
    "PPid: 3\n"
    "Pid:\t42\n"
    "State:  R  \n"
    "StrayKey: a = b\n"
    "CPU:  1\n"
    "BirthId: 105\n"
    "StartNs: 18000000000\n"
    "SampleNs: 23500000000\n";

static const char status_text_7[] =
    "Version:\t1\n"
    "Name:\tshell\n"
    "Pid:\t7\n"
    "PPid:\t1\n"
    "State:\tS\n"
    "CPU:\t0\n"
    "Priority:\t2\n"
    "BirthId:\t5001\n"
    "StartNs:\t111\n"
    "SampleNs:\t222\n"
    "RuntimeTicks:\t5\n";

static const char status_text_9[] =
    "Version:\t1\n"
    "Name:\tworker\n"
    "Pid:\t9\n"
    "PPid:\t1\n"
    "State:\tZ\n"
    "CPU:\t3\n"
    "Priority:\t2\n"
    "BirthId:\t5002\n"
    "StartNs:\t333\n"
    "SampleNs:\t444\n"
    "RuntimeTicks:\t9\n";

/* ── 1. /proc/stat ───────────────────────────────────────────────── */

static TEST_FUNC(test_stat_parse)
{
    struct monitor_stat_sample s;
    const char *t;

    /* valid: order-independent, whitespace-tolerant, unknown keys and
     * valid=0 cores kept */
    assert_eq(monitor_parse_stat(stat_text, sizeof(stat_text) - 1, &s), 0);
    assert_eq(s.sampled_ns, 23500000000ULL);
    assert_eq(s.tick_hz, 100);
    assert_eq(s.ncpu, 3);
    assert_eq(s.cpu[0].cpu_id, 0);
    assert_eq(s.cpu[0].busy_ticks, 1200);
    assert_eq(s.cpu[0].idle_ticks, 1150);
    assert_eq(s.cpu[0].epoch, 7);
    assert_eq(s.cpu[0].online, 1);
    assert_eq(s.cpu[0].valid, 1);
    assert_eq(s.cpu[0].tick_hz, 100);
    assert_eq(s.cpu[1].busy_ticks, 300);
    assert_eq(s.cpu[1].idle_ticks, 2050);
    assert_eq(s.cpu[2].valid, 0);          /* unavailable, not fabricated */
    assert_eq(s.cpu[2].busy_ticks, 0);

    /* no trailing newline on the final line is still one line */
    t = "Version:\t1\nSampleNs:\t5\nTickHz:\t100\n"
        "cpu0: online=1 valid=1 epoch=1 busy=2 idle=3";
    assert_eq(monitor_parse_stat(t, strlen(t), &s), 0);
    assert_eq(s.ncpu, 1);
    assert_eq(s.cpu[0].busy_ticks, 2);

    /* ── rejections (spec §6: duplicate/missing/overflow/illegal) ── */
    t = "Version:\t1\nVersion:\t1\nSampleNs:\t5\nTickHz:\t100\n";
    assert_eq(monitor_parse_stat(t, strlen(t), &s), -EINVAL);   /* dup Version */
    t = "Version:\t1\nSampleNs:\t5\nSampleNs:\t6\nTickHz:\t100\n";
    assert_eq(monitor_parse_stat(t, strlen(t), &s), -EINVAL);   /* dup SampleNs */
    t = "Version:\t2\nSampleNs:\t5\nTickHz:\t100\n";
    assert_eq(monitor_parse_stat(t, strlen(t), &s), -EINVAL);   /* bad Version */
    t = "SampleNs:\t5\nTickHz:\t100\n";
    assert_eq(monitor_parse_stat(t, strlen(t), &s), -EINVAL);   /* no Version */
    t = "Version:\t1\nTickHz:\t100\n";
    assert_eq(monitor_parse_stat(t, strlen(t), &s), -EINVAL);   /* no SampleNs */
    t = "Version:\t1\nSampleNs:\t5\n";
    assert_eq(monitor_parse_stat(t, strlen(t), &s), -EINVAL);   /* no TickHz */
    t = "Version:\t1\nSampleNs:\t5\nTickHz:\t100\n"
        "cpu0: online=1 valid=1 epoch=1 busy=18446744073709551616 idle=0\n";
    assert_eq(monitor_parse_stat(t, strlen(t), &s), -EINVAL);   /* u64 overflow */
    t = "Version:\t1\nSampleNs:\t5\nTickHz:\t100\n"
        "cpu0: online=1 valid=1 epoch=1 busy=1 idle=0\n"
        "cpu0: online=1 valid=1 epoch=1 busy=2 idle=3\n";
    assert_eq(monitor_parse_stat(t, strlen(t), &s), -EINVAL);   /* dup cpu line */
    t = "Version:\t1\nSampleNs:\t5\nTickHz:\t100\n"
        "cpu8: online=1 valid=1 epoch=1 busy=1 idle=1\n";
    assert_eq(monitor_parse_stat(t, strlen(t), &s), -EINVAL);   /* beyond 8-core */
    t = "Version:\t1\nSampleNs:\t5\nTickHz:\t100\n"
        "cpu0: online=2 valid=1 epoch=1 busy=1 idle=1\n";
    assert_eq(monitor_parse_stat(t, strlen(t), &s), -EINVAL);   /* illegal online */
    t = "Version:\t1\nSampleNs:\t5\nTickHz:\t100\n"
        "cpu0: online=1 valid=1 epoch=1 busy=1\n";
    assert_eq(monitor_parse_stat(t, strlen(t), &s), -EINVAL);   /* missing idle */
    t = "Version:\t1\nSampleNs:\t5\nTickHz:\t100\n"
        "cpu0: online=1 valid=1 epoch=1 busy=1 idle=1 busy=2\n";
    assert_eq(monitor_parse_stat(t, strlen(t), &s), -EINVAL);   /* dup subfield */
    t = "Version:\t1\nSampleNs:\t5\nTickHz:\t100\n"
        "cpu0: online=1 valid=1 epoch str=2 busy=1 idle=1\n";
    assert_eq(monitor_parse_stat(t, strlen(t), &s), -EINVAL);   /* malformed */
    t = "Version:\t1\nSampleNs:\t5\nTickHz:\t100\n"
        "cpu: online=1 valid=1 epoch=1 busy=1 idle=1\n";
    assert_eq(monitor_parse_stat(t, strlen(t), &s), -EINVAL);   /* no index */
    t = "Version:\t1\nSampleNs:\t5\nTickHz:\t100\n"
        "cpu0: online=1 valid=1 epoch=1 busy=1 idle=1\njunk-no-colon\n";
    assert_eq(monitor_parse_stat(t, strlen(t), &s), -EINVAL);   /* junk line */
    assert_eq(monitor_parse_stat(NULL, 0, &s), -EINVAL);
}

/* ── 2. /proc/meminfo (§4.1: kB kept, T/U/F, T=0 invalid) ────────── */

static TEST_FUNC(test_meminfo_parse)
{
    struct monitor_mem_sample m;
    const char *t;

    assert_eq(monitor_parse_meminfo(meminfo_text,
                                    sizeof(meminfo_text) - 1, &m), 0);
    assert_eq(m.sampled_ns, 777);
    assert_eq(m.total_bytes, 6144ULL * 1024);      /* T */
    assert_eq(m.used_bytes, 2048ULL * 1024);       /* U */
    assert_eq(m.free_bytes, 4096ULL * 1024);       /* F */

    t = "Version:\t1\nSampleNs:\t1\nMemTotal:\t0 kB\nMemUsed:\t0 kB\n"
        "MemFree:\t0 kB\n";
    assert_eq(monitor_parse_meminfo(t, strlen(t), &m), -EINVAL); /* T=0 */
    t = "Version:\t1\nSampleNs:\t1\nMemTotal:\t18014398509481984 kB\n"
        "MemUsed:\t0 kB\nMemFree:\t0 kB\n";
    assert_eq(monitor_parse_meminfo(t, strlen(t), &m), -EINVAL); /* kB*2^10 ovf */
    t = "Version:\t1\nSampleNs:\t1\nMemTotal:\t1 kB\nMemFree:\t0 kB\n";
    assert_eq(monitor_parse_meminfo(t, strlen(t), &m), -EINVAL); /* no MemUsed */
    t = "Version:\t1\nSampleNs:\t1\nMemTotal:\t1 kB\nMemTotal:\t2 kB\n"
        "MemUsed:\t0 kB\nMemFree:\t0 kB\n";
    assert_eq(monitor_parse_meminfo(t, strlen(t), &m), -EINVAL); /* dup total */
    t = "Version:\t0\nSampleNs:\t1\nMemTotal:\t1 kB\nMemUsed:\t0 kB\n"
        "MemFree:\t0 kB\n";
    assert_eq(monitor_parse_meminfo(t, strlen(t), &m), -EINVAL);
    t = "Version:\t1\nSampleNs:\tx\nMemTotal:\t1 kB\nMemUsed:\t0 kB\n"
        "MemFree:\t0 kB\n";
    assert_eq(monitor_parse_meminfo(t, strlen(t), &m), -EINVAL); /* non-numeric */
}

/* ── 3. /proc/<pid>/status + Name decoding ───────────────────────── */

/* complete status tail (string literal, so it concatenates onto the
 * single-line Name fixtures below) */
#define STATUS_TAIL \
    "Pid:\t1\nPPid:\t0\nState:\tR\nCPU:\t0\nBirthId:\t1\nStartNs:\t1\n" \
    "SampleNs:\t1\nRuntimeTicks:\t0\n"

static TEST_FUNC(test_status_parse_and_name_decode)
{
    struct monitor_process_sample p;
    const char *t;

    assert_eq(monitor_parse_status(status_text_42,
                                   sizeof(status_text_42) - 1, &p), 0);
    assert_eq(p.pid, 42);
    assert_eq(p.ppid, 3);
    assert_eq(p.birth_id, 105);
    assert_eq(p.start_ns, 18000000000ULL);
    assert_eq(p.sampled_ns, 23500000000ULL);
    assert_eq(p.runtime_ticks, 27);
    assert_eq(p.last_cpu, 1);
    assert_eq(p.state, 'R');
    assert_str_eq(p.comm, "mon\nitor");       /* \x0a decoded */
    assert_eq(strlen(p.comm), 8);   /* m,o,n,\n,i,t,o,r */

    /* duplicate BirthId fixture */
    {
        const char dup[] =
            "Version:\t1\nName:\tdupe\nPid:\t5\nPPid:\t1\nState:\tR\n"
            "CPU:\t0\nBirthId:\t9\nBirthId:\t9\nStartNs:\t1\n"
            "SampleNs:\t2\nRuntimeTicks:\t3\n";
        int parse_result = monitor_parse_status(dup, sizeof(dup) - 1, &p);
        assert(parse_result == -EINVAL); /* duplicate BirthId fixture */
    }

    /* missing required field (no RuntimeTicks) */
    t = "Version:\t1\nName:\tx\nPid:\t5\nPPid:\t1\nState:\tR\nCPU:\t0\n"
        "BirthId:\t9\nStartNs:\t1\nSampleNs:\t2\n";
    assert_eq(monitor_parse_status(t, strlen(t), &p), -EINVAL);
    /* duplicate Pid */
    t = "Version:\t1\nName:\tx\nPid:\t5\nPid:\t6\nPPid:\t1\nState:\tR\n"
        "CPU:\t0\nBirthId:\t9\nStartNs:\t1\nSampleNs:\t2\n"
        "RuntimeTicks:\t3\n";
    assert_eq(monitor_parse_status(t, strlen(t), &p), -EINVAL);
    /* Version unsupported */
    t = "Version:\t2\nName:\tx\nPid:\t5\nPPid:\t1\nState:\tR\nCPU:\t0\n"
        "BirthId:\t9\nStartNs:\t1\nSampleNs:\t2\nRuntimeTicks:\t3\n";
    assert_eq(monitor_parse_status(t, strlen(t), &p), -EINVAL);
    /* numeric overflow */
    t = "Version:\t1\nName:\tx\nPid:\t18446744073709551616\nPPid:\t1\n"
        "State:\tR\nCPU:\t0\nBirthId:\t9\nStartNs:\t1\nSampleNs:\t2\n"
        "RuntimeTicks:\t3\n";
    assert_eq(monitor_parse_status(t, strlen(t), &p), -EINVAL);
    /* CPU beyond u32 */
    t = "Version:\t1\nName:\tx\nPid:\t5\nPPid:\t1\nState:\tR\n"
        "CPU:\t4294967296\nBirthId:\t9\nStartNs:\t1\nSampleNs:\t2\n"
        "RuntimeTicks:\t3\n";
    assert_eq(monitor_parse_status(t, strlen(t), &p), -EINVAL);
    /* illegal state letter */
    t = "Version:\t1\nName:\tx\nPid:\t5\nPPid:\t1\nState:\tX\nCPU:\t0\n"
        "BirthId:\t9\nStartNs:\t1\nSampleNs:\t2\nRuntimeTicks:\t3\n";
    assert_eq(monitor_parse_status(t, strlen(t), &p), -EINVAL);
    /* kernel unknown-state marker '?' (procfs_snap.c snap_state_char
     * fallback): well-formed file must parse and keep the process row
     * with the state preserved — not silently dropped */
    t = "Version:\t1\nName:\todd\nPid:\t5\nPPid:\t1\nState:\t?\nCPU:\t2\n"
        "BirthId:\t9\nStartNs:\t1\nSampleNs:\t2\nRuntimeTicks:\t3\n";
    assert_eq(monitor_parse_status(t, strlen(t), &p), 0);
    assert_eq(p.state, '?');
    assert_eq(p.pid, 5);
    assert_eq(p.last_cpu, 2);
    /* multi-char state */
    t = "Version:\t1\nName:\tx\nPid:\t5\nPPid:\t1\nState:\tRS\nCPU:\t0\n"
        "BirthId:\t9\nStartNs:\t1\nSampleNs:\t2\nRuntimeTicks:\t3\n";
    assert_eq(monitor_parse_status(t, strlen(t), &p), -EINVAL);
    /* non-numeric BirthId */
    t = "Version:\t1\nName:\tx\nPid:\t5\nPPid:\t1\nState:\tR\nCPU:\t0\n"
        "BirthId:\tnine\nStartNs:\t1\nSampleNs:\t2\nRuntimeTicks:\t3\n";
    assert_eq(monitor_parse_status(t, strlen(t), &p), -EINVAL);
    assert_eq(monitor_parse_status(NULL, 0, &p), -EINVAL);

    /* ── Name decoding rules (spec §6) ───────────────────────────── */

    /* round trip: newline, backslash, colon, high byte */
    {
        /* bytes: a \n \ : 0xe9 z -> encoded "a\x0a" "\\" ":" "\xe9" "z" */
        char scratch[512];
        const char enc_head[] = "Version:\t1\nName:\ta\\x0a\\\\:\\xe9z\n";
        memcpy(scratch, enc_head, sizeof(enc_head) - 1);
        memcpy(scratch + sizeof(enc_head) - 1, STATUS_TAIL,
               sizeof(STATUS_TAIL));
        size_t enc_len = sizeof(enc_head) - 1 + sizeof(STATUS_TAIL) - 1;
        assert_eq(monitor_parse_status(scratch, enc_len, &p), 0);
        const unsigned char expect[] = { 'a', '\n', '\\', ':', 0xe9, 'z', 0 };
        assert_eq(strlen(p.comm), 6);
        assert_mem_eq(expect, p.comm, 7);

        /* decoded name is a copy: wiping the temporary text must not
         * affect the output (no pointer into the text survives) */
        memset(scratch, 0xEE, sizeof(scratch));
        assert_mem_eq(expect, p.comm, 7);
    }

    /* ':' inside the name belongs to the value, not a new key */
    {
        char scratch[512];
        const char enc_head[] = "Version:\t1\nName:\tkey:like\\x3aname\n";
        memcpy(scratch, enc_head, sizeof(enc_head) - 1);
        memcpy(scratch + sizeof(enc_head) - 1, STATUS_TAIL,
               sizeof(STATUS_TAIL));
        assert_eq(monitor_parse_status(
            scratch, sizeof(enc_head) - 1 + sizeof(STATUS_TAIL) - 1, &p), 0);
        assert_str_eq(p.comm, "key:like:name");
    }

    /* 63-byte name ok, 64 -> rejected */
    {
        char enc[512];
        char name[80];
        int n;
        memset(name, 'a', 63); name[63] = '\0';
        n = snprintf(enc, sizeof(enc),
            "Version:\t1\nName:\t%s\n%s", name, STATUS_TAIL);
        assert_true(n > 0);
        assert_eq(monitor_parse_status(enc, (size_t)n, &p), 0);
        assert_eq(strlen(p.comm), 63);

        memset(name, 'b', 64); name[64] = '\0';
        n = snprintf(enc, sizeof(enc),
            "Version:\t1\nName:\t%s\n%s", name, STATUS_TAIL);
        assert_eq(monitor_parse_status(enc, (size_t)n, &p), -EINVAL);
    }

    /* unknown escape */
    t = "Version:\t1\nName:\tbad\\qesc\n" STATUS_TAIL;
    assert_eq(monitor_parse_status(t, strlen(t), &p), -EINVAL);
    /* incomplete escape at end of line (no hex digits) */
    t = "Version:\t1\nName:\tterm\\x\n" STATUS_TAIL;
    assert_eq(monitor_parse_status(t, strlen(t), &p), -EINVAL);
    /* truncated \xHH (one hex digit before newline) */
    t = "Version:\t1\nName:\tterm\\xD\n" STATUS_TAIL;
    assert_eq(monitor_parse_status(t, strlen(t), &p), -EINVAL);
    /* non-hex digits after \x */
    t = "Version:\t1\nName:\tzz\\xzz\n" STATUS_TAIL;
    assert_eq(monitor_parse_status(t, strlen(t), &p), -EINVAL);
    /* escape marker at the very end of the text */
    t = "Version:\t1\nName:\tends\\";
    assert_eq(monitor_parse_status(t, strlen(t), &p), -EINVAL);
    /* decoded NUL rejected */
    t = "Version:\t1\nName:\tnul\\x00byte\n" STATUS_TAIL;
    assert_eq(monitor_parse_status(t, strlen(t), &p), -EINVAL);
    /* unknown uppercase marker is not an accepted escape either */
    t = "Version:\t1\nName:\tnul\\X00byte\n" STATUS_TAIL;
    assert_eq(monitor_parse_status(t, strlen(t), &p), -EINVAL);
}

/* ── 4. /proc/thermal ────────────────────────────────────────────── */

static TEST_FUNC(test_thermal_parse)
{
    struct monitor_thermal_sample t;
    const char *t2;

    assert_eq(monitor_parse_thermal(thermal_text,
                                    sizeof(thermal_text) - 1, &t), 0);
    assert_eq(t.state, MONITOR_THERMAL_STATE_UNSUPPORTED);
    assert_eq(t.reason, MONITOR_THERMAL_REASON_BACKEND_UNAVAILABLE);
    assert_eq(t.source, MONITOR_THERMAL_SOURCE_NONE);
    assert_eq(t.sampled_ns, 23500000000ULL);

    /* fixed reason table per Task 4 */
    static const struct { const char *s; int v; } reasons[] = {
        { "none",               MONITOR_THERMAL_REASON_NONE },
        { "backend_unavailable", MONITOR_THERMAL_REASON_BACKEND_UNAVAILABLE },
        { "hypervisor",         MONITOR_THERMAL_REASON_HYPERVISOR },
        { "vendor",             MONITOR_THERMAL_REASON_VENDOR },
        { "no_dts",             MONITOR_THERMAL_REASON_NO_DTS },
        { "model",              MONITOR_THERMAL_REASON_MODEL },
        { "not_ready",          MONITOR_THERMAL_REASON_NOT_READY },
        { "worker_start",       MONITOR_THERMAL_REASON_WORKER_START },
        { "msr",                MONITOR_THERMAL_REASON_MSR },
        { "target",             MONITOR_THERMAL_REASON_TARGET },
        { "reading",            MONITOR_THERMAL_REASON_READING },
    };
    for (size_t i = 0; i < sizeof(reasons) / sizeof(reasons[0]); i++) {
        char buf[256];
        int n = snprintf(buf, sizeof(buf),
            "Version:\t1\nState:\tunsupported\nReason:\t%s\n"
            "Source:\tnone\nSampleNs:\t9\n", reasons[i].s);
        assert_true(n > 0);
        assert_eq(monitor_parse_thermal(buf, (size_t)n, &t), 0);
        assert_eq(t.reason, reasons[i].v);
    }

    /* unknown reason keeps an unknown marker, never a normal one; the
     * file is still well-formed — usability is decided by State */
    t2 = "Version:\t1\nState:\tunsupported\nReason:\twarp_core\n"
         "Source:\tnone\nSampleNs:\t9\n";
    assert_eq(monitor_parse_thermal(t2, strlen(t2), &t), 0);
    assert_eq(t.reason, MONITOR_THERMAL_REASON_UNKNOWN);
    assert_true(t.state != MONITOR_THERMAL_STATE_OK);

    /* unknown source keeps an unknown marker */
    t2 = "Version:\t1\nState:\tunsupported\nReason:\tnone\n"
         "Source:\ttea_leaves\nSampleNs:\t9\n";
    assert_eq(monitor_parse_thermal(t2, strlen(t2), &t), 0);
    assert_eq(t.source, MONITOR_THERMAL_SOURCE_UNKNOWN);

    /* state=ok carries the value + owning CPU */
    t2 = "Version:\t1\nState:\tok\nReason:\treading\nSource:\tcore_dts\n"
         "SampleNs:\t5\nTempMilliC:\t-12500\nCpuId:\t0\n";
    assert_eq(monitor_parse_thermal(t2, strlen(t2), &t), 0);
    assert_eq(t.state, MONITOR_THERMAL_STATE_OK);
    assert_eq(t.temp_mc, -12500);
    assert_eq(t.cpu_id, 0);
    assert_eq(t.source, MONITOR_THERMAL_SOURCE_CORE_DTS);

    /* every kernel-table state string parses; usability is the state */
    {
        static const struct { const char *s; int v; } states[] = {
            { "ok",          MONITOR_THERMAL_STATE_OK },
            { "unsupported", MONITOR_THERMAL_STATE_UNSUPPORTED },
            { "invalid",     MONITOR_THERMAL_STATE_INVALID },
            { "read_error",  MONITOR_THERMAL_STATE_READ_ERROR },
            { "stale",       MONITOR_THERMAL_STATE_STALE },
            { "unknown",     MONITOR_THERMAL_STATE_UNKNOWN },
        };
        for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
            char buf[256];
            int n;
            /* state=ok must additionally carry the value fields */
            if (states[i].v == MONITOR_THERMAL_STATE_OK)
                n = snprintf(buf, sizeof(buf),
                    "Version:\t1\nState:\tok\nReason:\tnone\n"
                    "Source:\tnone\nSampleNs:\t9\nTempMilliC:\t1\n"
                    "CpuId:\t0\n");
            else
                n = snprintf(buf, sizeof(buf),
                    "Version:\t1\nState:\t%s\nReason:\tnone\n"
                    "Source:\tnone\nSampleNs:\t9\n", states[i].s);
            assert_true(n > 0);
            assert_eq(monitor_parse_thermal(buf, (size_t)n, &t), 0);
            assert_eq(t.state, states[i].v);
        }
    }

    /* rejections */
    t2 = "Version:\t1\nState:\tok\nReason:\treading\nSource:\tcore_dts\n"
         "SampleNs:\t5\nCpuId:\t0\n";
    assert_eq(monitor_parse_thermal(t2, strlen(t2), &t),
              -EINVAL);                          /* ok without TempMilliC */
    t2 = "Version:\t1\nState:\tok\nReason:\treading\nSource:\tcore_dts\n"
         "SampleNs:\t5\nTempMilliC:\t1\n";
    assert_eq(monitor_parse_thermal(t2, strlen(t2), &t),
              -EINVAL);                          /* ok without CpuId */
    t2 = "Version:\t1\nState:\tmeltdown\nReason:\tnone\nSource:\tnone\n"
         "SampleNs:\t5\n";
    assert_eq(monitor_parse_thermal(t2, strlen(t2), &t),
              -EINVAL);                          /* unknown state string */
    t2 = "Version:\t1\nState:\tok\nState:\tok\nReason:\tnone\n"
         "Source:\tnone\nSampleNs:\t5\nTempMilliC:\t1\nCpuId:\t0\n";
    assert_eq(monitor_parse_thermal(t2, strlen(t2), &t),
              -EINVAL);                          /* duplicate State */
    t2 = "Version:\t1\nState:\tok\nReason:\tnone\nSource:\tnone\n";
    assert_eq(monitor_parse_thermal(t2, strlen(t2), &t),
              -EINVAL);                          /* missing SampleNs */
    t2 = "Version:\t7\nState:\tok\nReason:\tnone\nSource:\tnone\n"
         "SampleNs:\t5\nTempMilliC:\t1\nCpuId:\t0\n";
    assert_eq(monitor_parse_thermal(t2, strlen(t2), &t), -EINVAL);
    t2 = "Version:\t1\nState:\tok\nReason:\tnone\nSource:\tnone\n"
         "SampleNs:\t5\nTempMilliC:\t2147483648\nCpuId:\t0\n";
    assert_eq(monitor_parse_thermal(t2, strlen(t2), &t),
              -EINVAL);                          /* int32 overflow */
    t2 = "Version:\t1\nState:\tok\nReason:\tnone\nSource:\tnone\n"
         "SampleNs:\t5\nTempMilliC:\tabs\nCpuId:\t0\n";
    assert_eq(monitor_parse_thermal(t2, strlen(t2), &t), -EINVAL);
    assert_eq(monitor_parse_thermal(NULL, 0, &t), -EINVAL);
}

/* ── 5. monitor_collect: scan, fd discipline, dedup, ENOENT ──────── */

/* Generated per-PID status texts in STATIC storage (fixture_add keeps
 * the pointers; the file's Pid matches its directory entry, as the
 * real kernel guarantees). */
static char gen_path[24][64];
static char gen_text[24][512];
static char gen_dname[24][16];

static const char *gen_status(int slot, int pid)
{
    snprintf(gen_path[slot], sizeof(gen_path[slot]), "/proc/%d/status", pid);
    snprintf(gen_dname[slot], sizeof(gen_dname[slot]), "%d", pid);
    snprintf(gen_text[slot], sizeof(gen_text[slot]),
        "Version:\t1\nName:\tapp%d\nPid:\t%d\nPPid:\t1\nState:\tR\n"
        "CPU:\t%u\nPriority:\t2\nBirthId:\t%d\nStartNs:\t10\n"
        "SampleNs:\t20\nRuntimeTicks:\t%d\n",
        pid, pid, (unsigned)(pid % 8), 500 + pid, pid);
    return gen_path[slot];
}

static TEST_FUNC(test_collect_scan_and_fd_discipline)
{
    struct monitor_sample s = { 0 };

    fixture_reset(5);   /* pretend stdin/out/err + 2 more already open */

    fixture_add("/proc/stat", stat_text);
    fixture_add("/proc/thermal", thermal_text);
    fixture_add("/proc/meminfo", meminfo_text);
    /* gen_status fills the static slot first; the returned path and
     * the filled text are then handed to fixture_add (no argument
     * evaluation-order dependence) */
    const char *st0 = gen_status(0, 10);
    fixture_add(st0, gen_text[0]);
    const char *st1 = gen_status(1, 11);
    fixture_add(st1, gen_text[1]);
    const char *st2 = gen_status(2, 12);
    fixture_add(st2, gen_text[2]);

    {
        static const char *names[] = { "10", "11", "12", "sh", "cpuinfo" };
        fixture_dir(names, 5);   /* non-decimal names must be skipped */
    }

    assert_eq(monitor_collect(&s), 0);
    assert_true(s.stat_valid);
    assert_true(s.mem_valid);
    assert_true(s.thermal_valid);
    assert_eq(s.nprocesses, 3);
    assert_str_eq(s.processes[0].comm, "app10");
    assert_eq(s.processes[0].pid, 10);
    assert_eq(s.processes[0].birth_id, 510);
    assert_str_eq(s.processes[1].comm, "app11");
    assert_eq(s.processes[1].pid, 11);
    assert_eq(s.processes[2].pid, 12);
    assert_eq(s.processes[2].state, 'R');

    /* fd discipline: directory handle + one status file, and every
     * status file was closed before the next one was opened */
    assert(max_open_fds <= baseline_open_fds + 2);
    assert_eq(max_open_fds, baseline_open_fds + 2);
    assert_eq(status_opens, 3);
    for (int i = 0; i < status_opens; i++)
        assert_eq(open_fds_at_status_open[i], baseline_open_fds + 2);
    assert_eq(open_fds, baseline_open_fds);          /* all released */
    assert_eq(mon_malloc_balance, 1);    /* the round's live array */

    monitor_sample_destroy(&s);
    assert_eq(mon_malloc_balance, 0);

    /* destroy is idempotent / safe on an already-destroyed struct */
    monitor_sample_destroy(&s);
    assert_eq(mon_malloc_balance, 0);
    assert_null(s.processes);
}

/* coherent record: the status Pid matches its /proc directory entry */
static const char status_text_coherent_30[] =
    "Version:\t1\nName:\tcoherent\nPid:\t30\nPPid:\t1\n"
    "State:\tR\nCPU:\t0\nBirthId:\t30\nStartNs:\t1\n"
    "SampleNs:\t1\nRuntimeTicks:\t1\n";

/* incoherent: /proc/31/status claims Pid 99 -> record dropped, because
 * the directory entry is the task identity (spec §5.2) */
static const char status_text_incoherent[] =
    "Version:\t1\nName:\tsneaky\nPid:\t99\nPPid:\t1\n"
    "State:\tR\nCPU:\t0\nBirthId:\t31\nStartNs:\t1\n"
    "SampleNs:\t1\nRuntimeTicks:\t1\n";

static const char status_text_dup_pid[] =
    "Version:\t1\nName:\tdup\nPid:\t13\nPid:\t13\nPPid:\t1\n"
    "State:\tR\nCPU:\t0\nBirthId:\t1\nStartNs:\t1\n"
    "SampleNs:\t1\nRuntimeTicks:\t1\n";

static TEST_FUNC(test_collect_dedup_enoent_invalid_drop)
{
    struct monitor_sample s = { 0 };
    struct fake_file *st10, *st11;

    fixture_reset(3);
    fixture_add("/proc/stat", stat_text);
    fixture_add("/proc/thermal", thermal_text);
    fixture_add("/proc/meminfo", meminfo_text);
    const char *dst0 = gen_status(0, 10);
    const char *dst2 = gen_status(2, 12);
    const char *dst1 = gen_status(1, 11);
    st10 = fixture_add(dst0, gen_text[0]);
    fixture_add(dst2, gen_text[2]);
    fixture_add("/proc/13/status", status_text_dup_pid);  /* invalid */
    st11 = fixture_add(dst1, gen_text[1]);
    (void)st10;
    fixture_add("/proc/30/status", status_text_coherent_30);
    fixture_add("/proc/31/status", status_text_incoherent);

    /* duplicate directory entries for the same PID must dedup, a
     * vanished PID (ENOENT) is skipped, an invalid status only drops
     * that one process, and an incoherent Pid drops the record */
    {
        static const char *names[] = {
            "10", "10", "11", "12", "13", "30", "31",
        };
        fixture_dir(names, 7);
    }
    assert_eq(monitor_collect(&s), 0);
    assert_eq(s.nprocesses, 4);      /* 10 deduped, 13 dropped, 31 dropped */
    assert_eq(s.processes[3].pid, 30);
    assert_str_eq(s.processes[3].comm, "coherent");
    assert_eq(s.processes[0].pid, 10);
    assert_eq(s.processes[1].pid, 11);
    assert_eq(s.processes[2].pid, 12);
    assert_eq(open_fds, baseline_open_fds);
    assert_eq(mon_malloc_balance, 1);    /* the round's live array */
    monitor_sample_destroy(&s);

    /* now let PIDs 10 and 11 vanish between readdir and open */
    st10->missing = 1;
    st11->missing = 1;
    {
        static const char *names[] = { "10", "11", "12" };
        fixture_dir(names, 3);
    }
    assert_eq(monitor_collect(&s), 0);
    assert_eq(s.nprocesses, 1);
    assert_eq(s.processes[0].pid, 12);
    assert_eq(open_fds, baseline_open_fds);
    assert_eq(mon_malloc_balance, 1);    /* the round's live array */
    monitor_sample_destroy(&s);
    assert_eq(mon_malloc_balance, 0);
}

static TEST_FUNC(test_collect_overlong_and_metric_isolation)
{
    struct monitor_sample s = { 0 };
    static char big_stat[1200];
    static char big_status[5000];

    fixture_reset(2);

    /* /proc/stat beyond the 8-core protocol cap -> that metric invalid,
     * everything else unaffected */
    memset(big_stat, 'x', sizeof(big_stat) - 2);
    big_stat[sizeof(big_stat) - 2] = '\n';
    big_stat[sizeof(big_stat) - 1] = '\0';
    fixture_add("/proc/stat", big_stat);
    fixture_add("/proc/thermal", thermal_text);
    fixture_add("/proc/meminfo", meminfo_text);
    fixture_add(gen_status(0, 20), gen_text[0]);

    /* overlong status file -> that PID dropped */
    memset(big_status, 'y', sizeof(big_status) - 2);
    big_status[sizeof(big_status) - 2] = '\n';
    big_status[sizeof(big_status) - 1] = '\0';
    fixture_add("/proc/21/status", big_status);

    {
        static const char *names[] = { "20", "21" };
        fixture_dir(names, 2);
    }

    assert_eq(monitor_collect(&s), 0);
    assert_false(s.stat_valid);
    assert_true(s.mem_valid);
    assert_true(s.thermal_valid);
    assert_eq(s.nprocesses, 1);
    assert_eq(s.processes[0].pid, 20);
    assert_eq(open_fds, baseline_open_fds);
    monitor_sample_destroy(&s);

    /* a missing/unparseable metric file invalidates only that metric */
    fixture_reset(2);
    fixture_add("/proc/stat", stat_text);
    fixture_add("/proc/thermal", thermal_text);
    fixture_add("/proc/meminfo", "invalid garbage, no keys\n");
    {
        const char *p20 = gen_status(1, 20);
        fixture_add(p20, gen_text[1]);
        static const char *names[] = { "20" };
        fixture_dir(names, 1);
    }
    assert_eq(monitor_collect(&s), 0);
    assert_true(s.stat_valid);
    assert_true(s.thermal_valid);
    assert_false(s.mem_valid);
    assert_eq(s.nprocesses, 1);
    monitor_sample_destroy(&s);
}

static TEST_FUNC(test_collect_oom_rolls_back_round)
{
    struct monitor_sample s = { 0 };

    fixture_reset(3);
    fixture_add("/proc/stat", stat_text);
    fixture_add("/proc/thermal", thermal_text);
    fixture_add("/proc/meminfo", meminfo_text);
    fixture_add(gen_status(0, 10), gen_text[0]);
    fixture_add(gen_status(1, 11), gen_text[1]);
    {
        static const char *names[] = { "10", "11" };
        fixture_dir(names, 2);
    }

    /* fail the very first allocation (the round's process array) */
    mon_malloc_fail_after = 0;
    assert_eq(monitor_collect(&s), -ENOMEM);
    assert_null(s.processes);
    assert_eq(s.nprocesses, 0);
    assert_eq(open_fds, baseline_open_fds);      /* dir handle released */
    assert_eq(mon_malloc_balance, 0);            /* nothing leaked */
    monitor_sample_destroy(&s);
    assert_eq(mon_malloc_balance, 0);

    /* recover */
    mon_malloc_fail_after = -1;
    assert_eq(monitor_collect(&s), 0);
    assert_eq(s.nprocesses, 2);
    assert_eq(mon_malloc_balance, 1);            /* one live array */
    monitor_sample_destroy(&s);
    assert_eq(mon_malloc_balance, 0);

    /* growth path: the array must grow past its initial capacity, and
     * a failed growth frees the round's partial array */
    fixture_reset(3);
    fixture_add("/proc/stat", stat_text);
    fixture_add("/proc/thermal", thermal_text);
    fixture_add("/proc/meminfo", meminfo_text);
    {
        static const char *names[20];
        for (int i = 0; i < 20; i++) {
            const char *path = gen_status(3 + i, 100 + i);  /* slots 3..22 */
            fixture_add(path, gen_text[3 + i]);
            names[i] = gen_dname[3 + i];    /* bare decimal PID name */
        }
        fixture_dir(names, 20);
    }

    mon_malloc_fail_after = 1;   /* initial array ok, growth fails */
    assert_eq(monitor_collect(&s), -ENOMEM);
    assert_null(s.processes);
    assert_eq(s.nprocesses, 0);
    assert_eq(open_fds, baseline_open_fds);
    assert_eq(mon_malloc_balance, 0);
    monitor_sample_destroy(&s);
    assert_eq(mon_malloc_balance, 0);

    mon_malloc_fail_after = -1;
    assert_eq(monitor_collect(&s), 0);
    assert_eq(s.nprocesses, 20);
    assert_eq(open_fds, baseline_open_fds);
    monitor_sample_destroy(&s);
    assert_eq(mon_malloc_balance, 0);
}

/* Fragment every field (including the escaped Name) across read calls. */
static TEST_FUNC(test_collect_short_reads)
{
    static const size_t chunks[] = { 1, 2, 7, 31 };
    static const char *names[] = { "42", "7" };
    for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
        struct monitor_sample s = { 0 };
        fixture_reset(5);
        fixture_add("/proc/stat", stat_text)->chunk = chunks[c];
        fixture_add("/proc/meminfo", meminfo_text)->chunk = chunks[c];
        fixture_add("/proc/thermal", thermal_text)->chunk = chunks[c];
        fixture_add("/proc/42/status", status_text_42)->chunk = chunks[c];
        fixture_add("/proc/7/status", status_text_7)->chunk = chunks[c];
        fixture_dir(names, 2);
        /* Reuse the target to exercise replacement of the previous round. */
        for (int round = 0; round < 2; round++) {
            assert_eq(monitor_collect(&s), 0);
            assert_true(s.stat_valid);
            assert_eq(s.stat.sampled_ns, 23500000000ULL);
            assert_eq(s.stat.ncpu, 3);
            assert_eq(s.stat.cpu[0].busy_ticks, 1200);
            assert_eq(s.stat.cpu[1].idle_ticks, 2050);
            assert_true(s.mem_valid);
            assert_eq(s.mem.total_bytes, 6144ULL * 1024);
            assert_true(s.thermal_valid);
            assert_eq(s.thermal.sampled_ns, 23500000000ULL);
            assert_eq(s.nprocesses, 2);
            assert_eq(s.processes[0].pid, 42);
            assert_str_eq(s.processes[0].comm, "mon\nitor");
            assert_eq(s.processes[0].runtime_ticks, 27);
            assert_eq(s.processes[1].pid, 7);
            assert_eq(open_fds, baseline_open_fds);
            assert_true(max_open_fds <= baseline_open_fds + 2);
            assert_eq(mon_malloc_balance, 1);
        }
        monitor_sample_destroy(&s);
        assert_eq(mon_malloc_balance, 0);
    }
}

/* Use parseable padding: truncating an overlong file would otherwise
 * produce a valid document, so these cases specifically test the cap. */
static TEST_FUNC(test_collect_read_cap_boundaries)
{
    static const struct {
        const char *path;
        const char *text;
        size_t cap;
    } files[] = {
        { "/proc/stat", stat_text, MONITOR_STAT_CAP },
        { "/proc/meminfo", meminfo_text, MONITOR_MEMINFO_CAP },
        { "/proc/thermal", thermal_text, MONITOR_THERMAL_CAP },
        { "/proc/42/status", status_text_42, MONITOR_STATUS_CAP },
    };
    static const size_t chunks[] = { 0, 1, 7 };
    static const char *names[] = { "42", "7" };
    char padded[MONITOR_STATUS_CAP + 2];
    for (size_t f = 0; f < sizeof(files) / sizeof(files[0]); f++) {
        for (size_t c = 0; c < sizeof(chunks) / sizeof(chunks[0]); c++) {
            for (int delta = -1; delta <= 1; delta++) {
                size_t len = files[f].cap + delta;
                size_t prefix = strlen(files[f].text);
                memcpy(padded, files[f].text, prefix);
                memset(padded + prefix, '\n', len - prefix);
                padded[len] = '\0';
                struct monitor_sample s = { 0 };
                fixture_reset(5);
                for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++)
                    fixture_add(files[i].path, i == f ? padded : files[i].text)
                        ->chunk = chunks[c];
                fixture_add("/proc/7/status", status_text_7)->chunk = chunks[c];
                fixture_dir(names, 2);
                assert_eq(monitor_collect(&s), 0);
                assert_eq(s.stat_valid, !(delta == 1 && f == 0));
                assert_eq(s.mem_valid, !(delta == 1 && f == 1));
                assert_eq(s.thermal_valid, !(delta == 1 && f == 2));
                assert_eq(s.nprocesses, delta == 1 && f == 3 ? 1 : 2);
                assert_eq(s.processes[s.nprocesses - 1].pid, 7);
                assert_eq(open_fds, baseline_open_fds);
                assert_true(max_open_fds <= baseline_open_fds + 2);
                monitor_sample_destroy(&s);
                assert_eq(mon_malloc_balance, 0);
            }
        }
    }
}

static TEST_FUNC(test_collect_zero_tick_hz)
{
    const char *zero = "Version: 1\nSampleNs: 5\nTickHz: 0\n"
        "cpu0: online=1 valid=1 epoch=1 busy=2 idle=3\n";
    struct monitor_stat_sample stat;
    assert_eq(monitor_parse_stat(zero, strlen(zero), &stat), -EINVAL);
    /* Positive rates are valid protocol values, not hardcoded to 100. */
    const char *one = "Version: 1\nSampleNs: 5\nTickHz: 1\n"
        "cpu0: online=1 valid=1 epoch=1 busy=2 idle=3\n";
    assert_eq(monitor_parse_stat(one, strlen(one), &stat), 0);
    assert_eq(stat.cpu[0].tick_hz, 1);

    struct monitor_sample s = { 0 };
    static const char *names[] = { "42" };
    fixture_reset(5);
    struct fake_file *file = fixture_add("/proc/stat", zero);
    file->chunk = 1;
    fixture_add("/proc/meminfo", meminfo_text)->chunk = 1;
    fixture_add("/proc/thermal", thermal_text)->chunk = 1;
    fixture_add("/proc/42/status", status_text_42)->chunk = 1;
    fixture_dir(names, 1);
    assert_eq(monitor_collect(&s), 0);
    assert_false(s.stat_valid);
    assert_true(s.mem_valid);
    assert_true(s.thermal_valid);
    assert_eq(s.nprocesses, 1);
    assert_eq(s.processes[0].pid, 42);
    assert_eq(open_fds, baseline_open_fds);
    /* A later valid sample recovers normally on the same target. */
    file->text = stat_text;
    file->len = strlen(stat_text);
    assert_eq(monitor_collect(&s), 0);
    assert_true(s.stat_valid);
    assert_eq(s.stat.tick_hz, 100);
    assert_eq(s.nprocesses, 1);
    assert_eq(open_fds, baseline_open_fds);
    monitor_sample_destroy(&s);
    assert_eq(mon_malloc_balance, 0);
}

static TEST_FUNC(test_collect_null_and_pid_validation)
{
    struct monitor_sample s = { 0 };

    assert_eq(monitor_collect(NULL), -EINVAL);
    monitor_sample_destroy(NULL);            /* safe no-op */

    /* a zeroed struct is a valid target */
    memset(&s, 0, sizeof(s));
    fixture_reset(3);
    fixture_add("/proc/stat", stat_text);
    fixture_add("/proc/thermal", thermal_text);
    fixture_add("/proc/meminfo", meminfo_text);
    {
        /* "007" is all-decimal (but its file is absent -> skipped);
         * "12x", "-5" and a >u64 digit run are not representable PIDs */
        static const char *names[] = {
            "007", "12x", "-5", "99999999999999999999999",
        };
        fixture_dir(names, 4);
    }
    assert_eq(monitor_collect(&s), 0);
    assert_eq(s.nprocesses, 0);
    assert_eq(open_fds, baseline_open_fds);
    assert_eq(status_opens, 0);   /* "007" died at the ENOENT open */
    monitor_sample_destroy(&s);
}

static TEST_FUNC(test_sample_init_on_garbage_memory)
{
    struct monitor_sample s;
    /* Poison all bytes to simulate uninitialized stack memory */
    memset(&s, 0x55, sizeof(s));

    mon_malloc_balance = 0;
    mon_malloc_fail_after = -1;

    /* Calling monitor_sample_init on garbage MUST NOT free garbage pointers */
    monitor_sample_init(&s);

    assert_eq(mon_malloc_balance, 0);
    assert_null(s.processes);
    assert_eq(s.nprocesses, 0);
    assert_eq(s.process_cap, 0);
    assert_eq(s.stat_valid, 0);
    assert_eq(s.mem_valid, 0);
    assert_eq(s.thermal_valid, 0);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_stat_parse),
    TEST_ENTRY(test_meminfo_parse),
    TEST_ENTRY(test_status_parse_and_name_decode),
    TEST_ENTRY(test_thermal_parse),
    TEST_ENTRY(test_collect_scan_and_fd_discipline),
    TEST_ENTRY(test_collect_dedup_enoent_invalid_drop),
    TEST_ENTRY(test_collect_overlong_and_metric_isolation),
    TEST_ENTRY(test_collect_oom_rolls_back_round),
    TEST_ENTRY(test_collect_short_reads),
    TEST_ENTRY(test_collect_read_cap_boundaries),
    TEST_ENTRY(test_collect_zero_tick_hz),
    TEST_ENTRY(test_collect_null_and_pid_validation),
    TEST_ENTRY(test_sample_init_on_garbage_memory),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
