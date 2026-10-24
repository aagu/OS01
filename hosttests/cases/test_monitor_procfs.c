/*
 * test_monitor_procfs.c — task identity + procfs snapshot producers
 *
 * Monitor plan Task 4 / monitor design spec §5.1, §5.2, §6
 * (docs/superpowers/specs/2026-10-09-monitor-design.md):
 *   - identity: fork assigns a fresh birth_id + zero runtime_ticks and
 *     copies ONLY the parent's comm; exec success renames comm to the
 *     basename and touches nothing else; exec failure keeps the old
 *     comm (production kernel/sched/identity.c, driven directly);
 *   - Name encoding (spec §6): 63 non-printable bytes → exactly 252
 *     encoded bytes on one line; backslash doubles; printable ASCII
 *     (incl. ':') verbatim; \xHH for everything else; one byte short
 *     of the reserved capacity → -EOVERFLOW;
 *   - status format: Version + SampleNs plus the new BirthId/StartNs/
 *     RuntimeTicks keys, existing fields kept, Name always one line;
 *   - stat/meminfo/thermal formats: Version: 1 + SampleNs everywhere,
 *     meminfo keeps kB, stat reports valid=0 cores instead of
 *     fabricating busy/idle, thermal weak defaults are
 *     unsupported/backend_unavailable/none and the reason table covers
 *     every enum value;
 *   - snapshot lifecycle: a status snapshot opened before the task
 *     disappears stays byte-stable across segmented reads after the
 *     task is deleted + poisoned + freed (no UAF, stable EOF); a
 *     vanished PID reopens as -ENOENT;
 *   - rollback: allocation failure at open → -ENOMEM with nothing
 *     attached and no leaked buffer; formatting overflow → -EOVERFLOW;
 *   - readdir: the PID range takes task_list_lock (observed via the
 *     lock hook), a concurrently deleted task (delete wins the same
 *     lock, memory poisoned) is never listed nor dereferenced, and a
 *     TASK_ZOMBIE task stays listed.
 *
 * Build: the PRODUCTION kernel/fs/procfs_snap.c, kernel/fs/procfs_fmt.c,
 * kernel/sched/identity.c and kernel/arch/thermal.c are host-compiled
 * via mock/procfs_test_runtime.h (test-controlled task list, clock,
 * kmalloc fail injection, lock observation hook).  sched_accounting_read
 * and pmm_get_stats are fixture stubs here: the real accounting.c and
 * pmm.c units have their own Task-2/Task-3 harnesses.
 */

#include "test_framework.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <fs/procfs.h>
#include <fs/procfs_fmt.h>
#include <fs/file.h>
#include <fs/vfs.h>
#include <sched/accounting.h>
#include <arch/thermal.h>
#include <memory/pmm.h>

/* Included explicitly because the test TU does not preinclude it; the
 * production TUs get it via -include on the Makefile rule. */
#include "../mock/procfs_test_runtime.h"

/* ── Controlled runtime surface (declared in procfs_test_runtime.h) ── */

procfs_test_task_union_t procfs_test_init_task_union;
spinlock_T task_list_lock = { 1 };
task_t *procfs_test_current;
uint64_t procfs_test_now_ns_storage;

/* Controlled monotonic clock for the production objects (identity.c
 * start_ns, snapshot sample times). */
uint64_t arch_clocksource_read_ns(void)
{
    return procfs_test_now_ns_storage;
}

/* ── kmalloc/kfree: counting + fail injection ──────────────────────── */

static long kmalloc_balance;
static int kmalloc_fail_after;   /* <0 never fail; 0 fail now; n>0 after n */

void *kmalloc(size_t size)
{
    if (kmalloc_fail_after == 0) return NULL;
    if (kmalloc_fail_after > 0) kmalloc_fail_after--;
    void *p = malloc(size ? size : 1);
    if (p) kmalloc_balance++;
    return p;
}

size_t kfree(void *ptr)
{
    if (ptr) { free(ptr); kmalloc_balance--; }
    return 0;
}

/* ── Stubbed data sources (real units covered by Tasks 2/3) ────────── */

static struct pmm_stats test_pmm_stats;

void pmm_get_stats(struct pmm_stats *out)
{
    *out = test_pmm_stats;
}

static struct cpu_usage_snapshot acct_snap[NR_CPUS];

int sched_accounting_read(unsigned cpu, struct cpu_usage_snapshot *out)
{
    if (!out || cpu >= NR_CPUS)
        return -EINVAL;
    *out = acct_snap[cpu];
    return 0;
}

/* vfs_read fallback (legacy nodes): must NOT be reached by the four
 * statistics nodes — counted so the fixture can assert that. */
static int vfs_read_calls;

int vfs_read(vfs_node_t *node, uint64_t offset, uint64_t size, void *buffer)
{
    (void)node; (void)offset; (void)size; (void)buffer;
    vfs_read_calls++;
    return -EIO;
}

/* ── Lock observation + simulated concurrent delete ─────────────────
 * Fires on EVERY irqsave acquisition (weak hook from the spinlock
 * stub).  When lock_delete_target is set, the "other CPU" wins the
 * task_list_lock race: it unlinks the task, POISONS the memory and
 * frees it — the reader holding/awaiting the lock must never touch it. */

static unsigned long lock_acquires;
static task_t *lock_delete_target;

void poll_test_spinlock_acquire(spinlock_T *lock)
{
    lock_acquires++;
    if (lock == &task_list_lock && lock_delete_target) {
        task_t *t = lock_delete_target;
        lock_delete_target = NULL;
        list_del(&t->list);
        memset(t, 0xAB, sizeof(*t));   /* poison */
        free(t);
        kmalloc_balance--;             /* was kmalloc'd by the fixture */
    }
}

/* ── Fixture task list helpers ─────────────────────────────────────── */

/* the .bss list head is garbage until initialized once */
static void fixture_task_list_ensure_init(void)
{
    static int list_ready;
    if (!list_ready) {
        list_init(&procfs_test_init_task_union.task.list);
        list_ready = 1;
    }
}

static task_t *fixture_task_add(int64_t pid, const char *comm, int64_t state)
{
    fixture_task_list_ensure_init();
    task_t *t = calloc(1, sizeof(*t));
    assert_not_null(t);
    t->pid = pid;
    t->state = state;
    t->priority = 5;
    t->cpu = 0;
    snprintf(t->comm, TASK_COMM_LEN, "%s", comm);
    list_init(&t->list);
    uint64_t fl = spin_lock_irqsave(&task_list_lock);
    list_add_to_before(&init_task_union.task.list, &t->list);
    spin_unlock_irqrestore(&task_list_lock, fl);
    kmalloc_balance++;   /* freed via free() in the hook / reset */
    return t;
}

static void fixture_task_list_reset(void)
{
    fixture_task_list_ensure_init();
    list_t *pos = procfs_test_init_task_union.task.list.next;
    while (pos != &procfs_test_init_task_union.task.list) {
        task_t *t = container_of(pos, task_t, list);
        pos = pos->next;
        free(t);
        kmalloc_balance--;
    }
    list_init(&procfs_test_init_task_union.task.list);
    lock_delete_target = NULL;
}

/* ── Snapshot-file plumbing: a bare vfs node + file_t carrying the
 * real procfs hooks, mirroring what SYS_open's FD_VFS branch builds. ── */

static const vfs_file_ops_t procfs_hook_fops = {
    .open_file    = procfs_open_file,
    .read_file    = procfs_read_file,
    .release_file = procfs_release_file,
};

static void test_node_init(struct vfs_node *n, uint32_t type, uint32_t pid)
{
    memset(n, 0, sizeof(*n));
    n->type = VFS_FILE;
    n->f_ops = &procfs_hook_fops;
    n->fs_data = PROCFS_ENCODE(type, pid);
    n->refcount = 1;
}

static void test_file_init(file_t *f, struct vfs_node *n)
{
    memset(f, 0, sizeof(*f));
    f->type = FD_VFS;
    f->node = n;
    f->flags = 0;   /* O_RDONLY */
}

/* Snapshot of a whole open file (single read). */
static int64_t snapshot_read_all(file_t *f, char *buf, size_t bufsz)
{
    return procfs_read_file(f, 0, bufsz, buf);
}

static int count_bytes(const char *s, size_t len, char c)
{
    int n = 0;
    for (size_t i = 0; i < len; i++)
        if (s[i] == c) n++;
    return n;
}

static long balance_at_test_start;

/* Fixture-local capacities mirroring procfs_snap.c (status >= 4096 per
 * brief Step 4; the others are their open-time caps). */
#define T_STATUS_CAP  4096
#define T_THERMAL_CAP 512
#define T_MEMINFO_CAP 512

static void test_setup(void)
{
    fixture_task_list_reset();
    task_list_lock.lock = 1;
    procfs_test_current = NULL;
    procfs_test_now_ns_storage = 1000;
    /* kmalloc_balance deliberately accumulates across tests — the final
     * test_malloc_balance_zero case requires every test to release what
     * it opened. */
    kmalloc_fail_after = -1;
    vfs_read_calls = 0;
    lock_acquires = 0;
    memset(&test_pmm_stats, 0, sizeof(test_pmm_stats));
    memset(acct_snap, 0, sizeof(acct_snap));
    balance_at_test_start = 0;
}

/* ── 1. Identity: fork birth/runtime/comm + exec rename ────────────── */

static TEST_FUNC(test_identity_birth_runtime_comm)
{
    test_setup();

    task_t parent, child_a, child_b;
    memset(&parent, 0, sizeof(parent));
    memset(&child_a, 0, sizeof(child_a));
    memset(&child_b, 0, sizeof(child_b));

    task_comm_set(&parent, "ash");
    parent.runtime_ticks = 55;      /* parent has been running */
    parent.birth_id = task_next_birth_id();
    parent.start_ns = 5;

    procfs_test_now_ns_storage = 4321;

    /* The fork path: identity init pre-publication, then comm inherit
     * under task_list_lock. */
    task_identity_init(&child_a, NULL);
    task_identity_init(&child_b, NULL);

    assert_true(child_a.birth_id != parent.birth_id &&
                child_a.runtime_ticks == 0);
    assert_true(child_b.birth_id != child_a.birth_id);
    assert_true(child_a.birth_id > parent.birth_id);
    assert_true(child_b.birth_id > child_a.birth_id);
    assert_eq(child_a.start_ns, 4321);
    assert_eq(child_a.runtime_ticks, 0);
    assert_eq(child_b.runtime_ticks, 0);

    /* birth ids are globally sequential */
    uint64_t b1 = task_next_birth_id();
    uint64_t b2 = task_next_birth_id();
    assert_eq(b2, b1 + 1);

    /* fork copies comm — and ONLY comm */
    task_comm_inherit(&child_a, &parent);
    assert_str_eq(child_a.comm, "ash");
    assert_true(child_a.birth_id != parent.birth_id);
    assert_true(child_a.start_ns != parent.start_ns);
    assert_eq(child_a.runtime_ticks, 0);

    /* inherit always leaves a terminated comm, even at the cap */
    char longname[TASK_COMM_LEN + 16];
    memset(longname, 'x', sizeof(longname));
    task_t longparent;
    memset(&longparent, 0, sizeof(longparent));
    memcpy(longparent.comm, longname, sizeof(longparent.comm));
    longparent.comm[TASK_COMM_LEN - 1] = '\0';
    task_comm_inherit(&child_b, &longparent);
    assert_eq(strlen(child_b.comm), TASK_COMM_LEN - 1);
    assert_eq(child_b.comm[TASK_COMM_LEN - 1], '\0');
}

static TEST_FUNC(test_exec_comm_lifecycle)
{
    test_setup();

    task_t t;
    memset(&t, 0, sizeof(t));
    task_identity_init(&t, "old");
    uint64_t birth = t.birth_id;
    uint64_t start = t.start_ns;
    t.runtime_ticks = 9;

    /* failure paths keep the old name */
    assert_eq(task_comm_apply_exec(&t, NULL), -EINVAL);
    assert_str_eq(t.comm, "old");
    assert_eq(task_comm_apply_exec(&t, ""), -EINVAL);
    assert_str_eq(t.comm, "old");
    assert_eq(task_comm_apply_exec(&t, "/"), -EINVAL);
    assert_str_eq(t.comm, "old");

    /* success changes ONLY comm */
    assert_eq(task_comm_apply_exec(&t, "/bin/busybox"), 0);
    assert_str_eq(t.comm, "busybox");
    assert_eq(t.birth_id, birth);
    assert_eq(t.start_ns, start);
    assert_eq(t.runtime_ticks, 9);

    /* basename semantics */
    assert_eq(task_comm_apply_exec(&t, "/a/b/c"), 0);
    assert_str_eq(t.comm, "c");
    assert_eq(task_comm_apply_exec(&t, "plain"), 0);
    assert_str_eq(t.comm, "plain");

    /* >63-byte basename truncates to the comm cap, still terminated */
    char longpath[128];
    memset(longpath, 'y', sizeof(longpath));
    memcpy(longpath, "/bin/", 5);
    longpath[sizeof(longpath) - 1] = '\0';
    assert_eq(task_comm_apply_exec(&t, longpath), 0);
    assert_eq(strlen(t.comm), TASK_COMM_LEN - 1);
    assert_eq(t.comm[TASK_COMM_LEN - 1], '\0');
}

/* ── 2. Name encoding (spec §6) ────────────────────────────────────── */

static TEST_FUNC(test_name_encoding)
{
    test_setup();

    /* 63 non-printable bytes → 252 encoded bytes, single line */
    char comm[PROCFS_COMM_MAX];
    for (int i = 0; i < 63; i++)
        comm[i] = (char)(0x01 + (i % 31));   /* 0x01..0x1f: all control */
    comm[63] = '\0';

    char out[PROCFS_NAME_ENC_MAX];
    int encoded_name_len = procfs_encode_name(comm, out, sizeof(out));
    assert_eq(encoded_name_len, 252);
    assert_eq(out[252], '\0');
    for (int i = 0; i < encoded_name_len; i++)
        assert_true(out[i] != '\n' && out[i] != '\0');

    /* one byte short of the reserved capacity → -EOVERFLOW */
    char small[PROCFS_NAME_ENC_MAX - 1];
    assert_eq(procfs_encode_name(comm, small, sizeof(small)), -EOVERFLOW);

    /* backslash doubles; printable ASCII (incl. ':') verbatim */
    assert_eq(procfs_encode_name("a\\b", out, sizeof(out)), 4);
    assert_mem_eq(((char[]){'a', 0x5c, 0x5c, 'b'}), out, 4);

    assert_eq(procfs_encode_name("mon:itor x", out, sizeof(out)), 10);
    assert_str_eq(out, "mon:itor x");

    /* newline (and any other control byte) encodes as \xHH */
    assert_eq(procfs_encode_name("a\nb", out, sizeof(out)), 6);
    assert_mem_eq(((char[]){'a', 0x5c, 'x', '0', 'a', 'b'}), out, 6);
    assert_true(count_bytes(out, 6, '\n') == 0);

    /* 0x7f (DEL) is outside 0x20–0x7e → escaped */
    char del[2] = { (char)0x7f, '\0' };
    assert_eq(procfs_encode_name(del, out, sizeof(out)), 4);
    assert_mem_eq(((char[]){0x5c, 'x', '7', 'f'}), out, 4);

    /* space is printable → verbatim */
    assert_eq(procfs_encode_name("a b", out, sizeof(out)), 3);

    assert_eq(procfs_encode_name(NULL, out, sizeof(out)), -EINVAL);
    assert_eq(procfs_encode_name("x", NULL, sizeof(out)), -EINVAL);
}

/* ── 3. status format ──────────────────────────────────────────────── */

static TEST_FUNC(test_status_format)
{
    test_setup();

    struct procfs_task_snap s;
    memset(&s, 0, sizeof(s));
    s.pid = 42; s.ppid = 3;
    s.birth_id = 105;
    s.start_ns = 18000000000ULL;
    s.sample_ns = 23500000000ULL;
    s.runtime_ticks = 27;
    s.last_cpu = 1;
    s.priority = 5;
    s.state_c = 'R';
    strcpy(s.comm, "mon\nitor");       /* newline must stay inside line 2 */

    char buf[T_STATUS_CAP];
    int n = procfs_fmt_status(&s, buf, sizeof(buf));
    assert_true(n > 0);
    buf[n] = '\0';

    /* Version + SampleNs on every statistics file; identity block */
    assert_true(strstr(buf, "Version:\t1\n") == buf);
    assert_true(strstr(buf, "Name:\tmon\\x0aitor\n") != NULL);
    assert_true(strstr(buf, "Pid:\t42\n") != NULL);
    assert_true(strstr(buf, "PPid:\t3\n") != NULL);
    assert_true(strstr(buf, "State:\tR\n") != NULL);
    assert_true(strstr(buf, "CPU:\t1\n") != NULL);
    assert_true(strstr(buf, "Priority:\t5\n") != NULL);
    assert_true(strstr(buf, "BirthId:\t105\n") != NULL);
    assert_true(strstr(buf, "StartNs:\t18000000000\n") != NULL);
    assert_true(strstr(buf, "SampleNs:\t23500000000\n") != NULL);
    assert_true(strstr(buf, "RuntimeTicks:\t27\n") != NULL);

    /* exactly one line per key: the newline in comm never leaks */
    assert_eq(count_bytes(buf, (size_t)n, '\n'), 11);

    /* overflow → -EOVERFLOW, never silent truncation */
    char tiny[16];
    assert_eq(procfs_fmt_status(&s, tiny, sizeof(tiny)), -EOVERFLOW);
    assert_eq(procfs_fmt_status(NULL, buf, sizeof(buf)), -EINVAL);
}

/* ── 4. stat format + NR_CPUS preallocation ────────────────────────── */

static TEST_FUNC(test_stat_format)
{
    test_setup();

    struct procfs_stat_snap s;
    memset(&s, 0, sizeof(s));
    s.sample_ns = 23500000000ULL;
    s.ncpu = 2;
    s.cpu[0].online = 1; s.cpu[0].valid = 1; s.cpu[0].tick_hz = 100;
    s.cpu[0].epoch = 1; s.cpu[0].busy_ticks = 1200; s.cpu[0].idle_ticks = 1150;
    s.cpu[1].online = 1; /* valid=0: timer never started — unavailable */

    char buf[1024];
    int n = procfs_fmt_stat(&s, buf, sizeof(buf));
    assert_true(n > 0);
    buf[n] = '\0';
    assert_true(strstr(buf, "Version:\t1\n") == buf);
    assert_true(strstr(buf, "SampleNs:\t23500000000\n") != NULL);
    assert_true(strstr(buf, "TickHz:\t100\n") != NULL);
    assert_true(strstr(buf,
        "cpu0:\tonline=1 valid=1 epoch=1 busy=1200 idle=1150\n") != NULL);
    assert_true(strstr(buf,
        "cpu1:\tonline=1 valid=0 epoch=0 busy=0 idle=0\n") != NULL);

    /* NR_CPUS-scale preallocation: all slots render */
    s.ncpu = NR_CPUS;
    n = procfs_fmt_stat(&s, buf, sizeof(buf));
    assert_true(n > 0);
    buf[n] = '\0';
    char key[32];
    snprintf(key, sizeof(key), "cpu%u:", (unsigned)(NR_CPUS - 1));
    assert_true(strstr(buf, key) != NULL);

    assert_eq(procfs_fmt_stat(&s, buf, 8), -EOVERFLOW);
    s.ncpu = NR_CPUS + 1;
    assert_eq(procfs_fmt_stat(&s, buf, sizeof(buf)), -EINVAL);
}

/* ── 5. thermal weak defaults + fixed reason table ─────────────────── */

static TEST_FUNC(test_thermal_weak_defaults_and_reason_table)
{
    test_setup();

    /* Pre-init read: weak default, never a fabricated temperature */
    struct thermal_sample s;
    thermal_read(0, &s);
    assert_eq(s.state, THERMAL_UNSUPPORTED);
    assert_eq(s.reason, THERMAL_REASON_BACKEND_UNAVAILABLE);
    assert_eq(s.source, THERMAL_SOURCE_NONE);
    assert_eq(s.cpu_id, 0);

    /* Init publishes the same weak-default group into the cache (the
     * cache's sampled_ns is taken at publish time) */
    procfs_test_now_ns_storage = 55555;
    assert_eq(thermal_subsys_init(), 0);
    thermal_read(0, &s);
    assert_eq(s.state, THERMAL_UNSUPPORTED);
    assert_eq(s.reason, THERMAL_REASON_BACKEND_UNAVAILABLE);
    assert_eq(s.source, THERMAL_SOURCE_NONE);
    assert_eq(s.sampled_ns, 55555);

    /* Out-of-range CPU: weak default, no cache access */
    thermal_read(NR_CPUS, &s);
    assert_eq(s.state, THERMAL_UNSUPPORTED);
    assert_eq(s.reason, THERMAL_REASON_BACKEND_UNAVAILABLE);

    /* Fixed reason table: every enum value maps to its exact string */
    static const char *expect[] = {
        "none", "backend_unavailable", "hypervisor", "vendor", "no_dts",
        "model", "not_ready", "worker_start", "msr", "target", "reading",
    };
    for (uint32_t r = 0; r <= THERMAL_REASON_READING; r++)
        assert_str_eq(expect[r], procfs_thermal_reason_str(r));
    assert_str_eq("unknown", procfs_thermal_reason_str(999));

    /* format: unsupported sample shows State/Reason/Source/SampleNs */
    struct procfs_thermal_snap ts;
    memset(&ts, 0, sizeof(ts));
    ts.state = THERMAL_UNSUPPORTED;
    ts.reason = THERMAL_REASON_BACKEND_UNAVAILABLE;
    ts.source = THERMAL_SOURCE_NONE;
    ts.sampled_ns = 42;
    char buf[T_THERMAL_CAP];
    int n = procfs_fmt_thermal(&ts, buf, sizeof(buf));
    assert_true(n > 0);
    buf[n] = '\0';
    assert_true(strstr(buf, "Version:\t1\n") == buf);
    assert_true(strstr(buf, "State:\tunsupported\n") != NULL);
    assert_true(strstr(buf, "Reason:\tbackend_unavailable\n") != NULL);
    assert_true(strstr(buf, "Source:\tnone\n") != NULL);
    assert_true(strstr(buf, "SampleNs:\t42\n") != NULL);
    assert_true(strstr(buf, "TempMilliC") == NULL);   /* not ok → no temp */

    /* state=ok adds the value + owning CPU */
    ts.state = THERMAL_OK;
    ts.reason = THERMAL_REASON_READING;
    ts.source = THERMAL_SOURCE_CORE_DTS;
    ts.temp_mc = -12500;
    ts.cpu_id = 0;
    n = procfs_fmt_thermal(&ts, buf, sizeof(buf));
    assert_true(n > 0);
    buf[n] = '\0';
    assert_true(strstr(buf, "State:\tok\n") != NULL);
    assert_true(strstr(buf, "Reason:\treading\n") != NULL);
    assert_true(strstr(buf, "Source:\tcore_dts\n") != NULL);
    assert_true(strstr(buf, "TempMilliC:\t-12500\n") != NULL);
    assert_true(strstr(buf, "CpuId:\t0\n") != NULL);

    assert_eq(procfs_fmt_thermal(&ts, buf, 8), -EOVERFLOW);
}

/* ── 6. meminfo keeps kB ───────────────────────────────────────────── */

static TEST_FUNC(test_meminfo_format)
{
    test_setup();

    struct procfs_meminfo_snap s;
    memset(&s, 0, sizeof(s));
    s.sample_ns = 777;
    s.managed_bytes = 6291456;    /* 6 MiB */
    s.reserved_bytes = 2097152;   /* 2 MiB */
    s.free_bytes = 4194304;       /* 4 MiB */

    char buf[T_MEMINFO_CAP];
    int n = procfs_fmt_meminfo(&s, buf, sizeof(buf));
    assert_true(n > 0);
    buf[n] = '\0';
    assert_true(strstr(buf, "Version:\t1\n") == buf);
    assert_true(strstr(buf, "SampleNs:\t777\n") != NULL);
    assert_true(strstr(buf, "MemTotal:\t6144 kB\n") != NULL);
    assert_true(strstr(buf, "MemUsed:\t2048 kB\n") != NULL);
    assert_true(strstr(buf, "MemFree:\t4096 kB\n") != NULL);
    assert_true(strstr(buf, "PageSize:\t2048 kB\n") != NULL);

    assert_eq(procfs_fmt_meminfo(&s, buf, 10), -EOVERFLOW);
}

/* Legacy node reads accept an exact-fit byte buffer. */

static TEST_FUNC(test_legacy_node_capacity_boundary)
{
    test_setup();
    fixture_task_add(77, "legacy boundary", TASK_RUNNING);
    const uint32_t types[] = {
        PROCFS_TYPE_STATUS, PROCFS_TYPE_STAT,
        PROCFS_TYPE_THERMAL, PROCFS_TYPE_MEMINFO,
    };

    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        struct vfs_node node;
        test_node_init(&node, types[i], 77);
        file_t f;
        test_file_init(&f, &node);
        assert_eq(procfs_open_file(&f), 0);

        /* Use the public per-file path as the expected byte stream.
         * Fixture clocks and data sources remain fixed across reads. */
        char expected[T_STATUS_CAP];
        int64_t len = snapshot_read_all(&f, expected, sizeof(expected));
        assert_true(len > 1 && len < T_STATUS_CAP);
        if (len <= 1 || len >= T_STATUS_CAP) {
            procfs_release_file(&f);
            continue;
        }
        procfs_release_file(&f);
        long balance_before = kmalloc_balance;

        char storage[T_STATUS_CAP + 2];
        char untouched[sizeof(storage)];
        memset(storage, 0x5a, sizeof(storage));
        memcpy(untouched, storage, sizeof(storage));
        assert_eq(procfs_generate_node(types[i], 77, storage + 1,
                                       (size_t)len - 1), -EOVERFLOW);
        assert_mem_eq(storage, untouched, sizeof(storage));
        assert_eq(kmalloc_balance, balance_before);

        /* The destination is a byte buffer, not a C string: exact fit
         * succeeds and must leave both adjacent canaries untouched. */
        assert_eq(procfs_generate_node(types[i], 77, storage + 1,
                                       (size_t)len), len);
        assert_mem_eq(storage + 1, expected, (size_t)len);
        assert_eq(storage[0], 0x5a);
        assert_eq(storage[len + 1], 0x5a);
        assert_eq(kmalloc_balance, balance_before);

        memset(storage, 0x5a, sizeof(storage));
        assert_eq(procfs_generate_node(types[i], 77, storage + 1,
                                       (size_t)len + 1), len);
        assert_mem_eq(storage + 1, expected, (size_t)len);
        assert_eq(storage[0], 0x5a);
        assert_eq(storage[len + 1], 0x5a); /* spare byte is not a NUL */
        assert_eq(storage[len + 2], 0x5a);
        assert_eq(kmalloc_balance, balance_before);
    }
}

/* ── 7. snapshot stable across task disappearance ──────────────────── */

static TEST_FUNC(test_snapshot_stable_across_disappearance)
{
    test_setup();

    task_t *t = fixture_task_add(77, "monitor", TASK_RUNNING);
    t->birth_id = 5001;
    t->start_ns = 111;
    t->runtime_ticks = 33;
    task_t init_stub;
    memset(&init_stub, 0, sizeof(init_stub));
    init_stub.pid = 1;
    t->parent = &init_stub;

    struct vfs_node node;
    test_node_init(&node, PROCFS_TYPE_STATUS, 77);
    file_t f;
    test_file_init(&f, &node);

    /* open snapshots the task while it is alive */
    unsigned long locks_before = lock_acquires;
    assert_eq(procfs_open_file(&f), 0);
    assert_not_null(f.fs_private);
    assert_true(lock_acquires > locks_before);   /* task_list_lock taken */

    char full[T_STATUS_CAP];
    int64_t total = snapshot_read_all(&f, full, sizeof(full) - 1);
    assert_true(total > 0);
    full[total] = '\0';
    assert_true(strstr(full, "Pid:\t77\n") != NULL);
    assert_true(strstr(full, "Name:\tmonitor\n") != NULL);
    assert_true(strstr(full, "BirthId:\t5001\n") != NULL);
    assert_true(strstr(full, "RuntimeTicks:\t33\n") != NULL);
    assert_eq(vfs_read_calls, 0);                /* snapshot path, not legacy */

    /* The task disappears (unlink + poison + free) while the file stays
     * open — the delete wins the same task_list_lock the next readdir
     * takes. */
    lock_delete_target = t;
    struct vfs_dirent ent;
    char entname[VFS_NAME_MAX];
    ent.name = entname;
    assert_eq(procfs_readdir_tasks(4, &ent, 4), 0);
    assert_true(lock_delete_target == NULL);     /* hook consumed it */
    assert_true(entname[0] == '\0' || strcmp(entname, "77") != 0);

    /* Segmented reads of the STILL-OPEN file are byte-stable */
    char seg[T_STATUS_CAP];
    memset(seg, 0, sizeof(seg));
    uint64_t off = 0;
    for (;;) {
        int64_t n = procfs_read_file(&f, off, 7, seg + off);
        assert_true(n >= 0);
        if (n == 0) break;
        off += (uint64_t)n;
        assert_true(off <= (uint64_t)total);
    }
    assert_eq((int64_t)off, total);
    assert_mem_eq(full, seg, (size_t)total);

    /* EOF is the stable snapshot length */
    char probe[8];
    assert_eq(procfs_read_file(&f, (uint64_t)total, sizeof(probe), probe), 0);

    procfs_release_file(&f);
    assert_null(f.fs_private);

    /* reopening the vanished PID is -ENOENT, nothing attached */
    file_t f2;
    test_file_init(&f2, &node);
    assert_eq(procfs_open_file(&f2), -ENOENT);
    assert_null(f2.fs_private);
    procfs_release_file(&f2);   /* no-op after failed open */
}

/* ── 8. open rollback on allocation failure ────────────────────────── */

static TEST_FUNC(test_open_rollback_oom)
{
    test_setup();

    fixture_task_add(88, "rol", TASK_RUNNING);

    struct vfs_node node;
    test_node_init(&node, PROCFS_TYPE_STATUS, 88);
    file_t f;
    test_file_init(&f, &node);

    /* fail the FIRST allocation (the status snapshot buffer) */
    kmalloc_fail_after = 0;
    balance_at_test_start = kmalloc_balance;
    assert_eq(procfs_open_file(&f), -ENOMEM);
    assert_null(f.fs_private);
    assert_eq(kmalloc_balance, balance_at_test_start);  /* nothing leaked */

    /* release after a failed open must be a safe no-op */
    procfs_release_file(&f);
    assert_eq(kmalloc_balance, balance_at_test_start);

    /* recover: same node opens cleanly afterwards */
    kmalloc_fail_after = -1;
    assert_eq(procfs_open_file(&f), 0);
    assert_not_null(f.fs_private);
    procfs_release_file(&f);
    assert_eq(kmalloc_balance, balance_at_test_start);
    assert_null(f.fs_private);
}

/* ── 9. statistics nodes route through the snapshot, not vfs_read ──── */

static TEST_FUNC(test_stat_thermal_meminfo_open_paths)
{
    test_setup();

    /* stat */
    acct_snap[0].online = 1; acct_snap[0].valid = 1;
    acct_snap[0].tick_hz = 100; acct_snap[0].epoch = 3;
    acct_snap[0].busy_ticks = 10; acct_snap[0].idle_ticks = 90;

    struct vfs_node node;
    file_t f;
    char buf[T_STATUS_CAP];
    int64_t n;

    test_node_init(&node, PROCFS_TYPE_STAT, 0);
    test_file_init(&f, &node);
    procfs_test_now_ns_storage = 4242;
    assert_eq(procfs_open_file(&f), 0);
    n = snapshot_read_all(&f, buf, sizeof(buf) - 1);
    assert_true(n > 0);
    buf[n] = '\0';
    assert_true(strstr(buf, "Version:\t1\n") == buf);
    assert_true(strstr(buf, "SampleNs:\t4242\n") != NULL);
    assert_true(strstr(buf, "TickHz:\t100\n") != NULL);
    assert_true(strstr(buf, "epoch=3 busy=10 idle=90\n") != NULL);
    procfs_release_file(&f);

    /* thermal */
    assert_eq(thermal_subsys_init(), 0);
    test_node_init(&node, PROCFS_TYPE_THERMAL, 0);
    test_file_init(&f, &node);
    assert_eq(procfs_open_file(&f), 0);
    n = snapshot_read_all(&f, buf, sizeof(buf) - 1);
    assert_true(n > 0);
    buf[n] = '\0';
    assert_true(strstr(buf, "State:\tunsupported\n") != NULL);
    assert_true(strstr(buf, "Reason:\tbackend_unavailable\n") != NULL);
    procfs_release_file(&f);

    /* meminfo */
    test_pmm_stats.managed_bytes = 2097152;
    test_pmm_stats.reserved_bytes = 1048576;
    test_pmm_stats.free_bytes = 1048576;
    test_node_init(&node, PROCFS_TYPE_MEMINFO, 0);
    test_file_init(&f, &node);
    assert_eq(procfs_open_file(&f), 0);
    n = snapshot_read_all(&f, buf, sizeof(buf) - 1);
    assert_true(n > 0);
    buf[n] = '\0';
    assert_true(strstr(buf, "MemTotal:\t2048 kB\n") != NULL);
    assert_true(strstr(buf, "MemUsed:\t1024 kB\n") != NULL);
    assert_true(strstr(buf, "MemFree:\t1024 kB\n") != NULL);
    procfs_release_file(&f);

    /* none of the statistics nodes may fall into the legacy read path */
    assert_eq(vfs_read_calls, 0);
}

/* ── 10. readdir: zombie stays listed, deleted task never UAF ──────── */

static TEST_FUNC(test_readdir_zombie_and_concurrent_delete)
{
    test_setup();

    task_t *t1 = fixture_task_add(10, "a", TASK_RUNNING);
    task_t *t2 = fixture_task_add(11, "b", TASK_ZOMBIE);
    task_t *t3 = fixture_task_add(12, "c", TASK_INTERRUPTIBLE);
    (void)t1; (void)t3;

    struct vfs_dirent ent;
    char entname[VFS_NAME_MAX];
    ent.name = entname;

    unsigned long locks_before = lock_acquires;
    assert_eq(procfs_readdir_tasks(4, &ent, 4), 0);
    assert_str_eq(entname, "10");
    assert_eq(procfs_readdir_tasks(5, &ent, 4), 0);
    assert_str_eq(entname, "11");            /* zombie still listed */
    assert_eq(procfs_readdir_tasks(6, &ent, 4), 0);
    assert_str_eq(entname, "12");
    assert_eq(procfs_readdir_tasks(7, &ent, 4), 0);
    assert_eq(entname[0], '\0');             /* end of range */
    assert_true(lock_acquires > locks_before); /* each scan takes the lock */

    assert_eq(procfs_readdir_tasks(3, &ent, 4), -EINVAL);  /* below base */
    assert_eq(procfs_readdir_tasks(4, NULL, 4), -EINVAL);

    /* delete t2 conventionally; the scan shrinks without touching it */
    {
        uint64_t fl = spin_lock_irqsave(&task_list_lock);
        list_del(&t2->list);
        spin_unlock_irqrestore(&task_list_lock, fl);
        memset(t2, 0xAB, sizeof(*t2));
        free(t2);
        kmalloc_balance--;
    }
    assert_eq(procfs_readdir_tasks(4, &ent, 4), 0);
    assert_str_eq(entname, "10");
    assert_eq(procfs_readdir_tasks(5, &ent, 4), 0);
    assert_str_eq(entname, "12");
    assert_eq(procfs_readdir_tasks(6, &ent, 4), 0);
    assert_eq(entname[0], '\0');

    /* concurrent delete winning the lock race: poison+free fires inside
     * the acquisition hook; the scan must not list nor dereference it */
    lock_delete_target = t1;
    assert_eq(procfs_readdir_tasks(4, &ent, 4), 0);
    assert_str_eq(entname, "12");
    assert_true(lock_delete_target == NULL);
    assert_eq(procfs_readdir_tasks(5, &ent, 4), 0);
    assert_eq(entname[0], '\0');
}

static TEST_FUNC(test_malloc_balance_zero)
{
    test_setup();

    /* drain any task fixtures earlier tests left behind, then require
     * every snapshot buffer to have been released (open/close balance) */
    fixture_task_list_reset();
    assert_eq(kmalloc_balance, 0);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_identity_birth_runtime_comm),
    TEST_ENTRY(test_exec_comm_lifecycle),
    TEST_ENTRY(test_name_encoding),
    TEST_ENTRY(test_status_format),
    TEST_ENTRY(test_stat_format),
    TEST_ENTRY(test_thermal_weak_defaults_and_reason_table),
    TEST_ENTRY(test_meminfo_format),
    TEST_ENTRY(test_legacy_node_capacity_boundary),
    TEST_ENTRY(test_snapshot_stable_across_disappearance),
    TEST_ENTRY(test_open_rollback_oom),
    TEST_ENTRY(test_stat_thermal_meminfo_open_paths),
    TEST_ENTRY(test_readdir_zombie_and_concurrent_delete),
    TEST_ENTRY(test_malloc_balance_zero),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
