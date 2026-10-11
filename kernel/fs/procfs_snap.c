/*
 * kernel/fs/procfs_snap.c — procfs statistics snapshot producers
 * (monitor plan Task 4, spec §5.1/§5.2/§6).
 *
 * The four statistics nodes (stat/thermal/meminfo/status) use the Task-1
 * per-file VFS hooks:
 *
 *   procfs_open_file    — allocates the fixed output buffer OUTSIDE any
 *                         data-source lock, copies each source under its
 *                         own lock into a plain snapshot struct, formats
 *                         outside the lock, and attaches the text to
 *                         f->fs_private.  Failure (-ENOMEM allocation,
 *                         -EOVERFLOW formatting, -ENOENT vanished PID)
 *                         leaves nothing attached: file_free's
 *                         release_file call treats NULL fs_private as
 *                         "nothing attached".
 *   procfs_read_file    — slices the open-time snapshot; EOF is the
 *                         stable snapshot length, so segmented reads
 *                         never mix two generations.  Nodes without a
 *                         snapshot (maps, fd entries) fall back to the
 *                         legacy node read.
 *   procfs_release_file — frees the snapshot exactly once (last put).
 *
 * Lock discipline (spec §5.1/§2.4):
 *   - status: find + full identity copy (incl. comm) under
 *     task_list_lock; NO task pointer is retained after unlock; the
 *     sample time is taken inside the same critical section; formatting
 *     happens after unlock.  No runqueue lock is ever taken here —
 *     State/CPU are single-field reads compatible with their rq-lock
 *     writers and are allowed to be mutually inconsistent.
 *   - stat: one sched_accounting_read() per CPU, each copying under its
 *     own per-CPU lock and unlocking immediately; never nested with
 *     task_list_lock.  NR_CPUS slots are always preallocated (maximum
 *     scale), so the output never fabricates per-CPU zeros for cores
 *     whose snapshot read fails or that report valid=0.
 *   - thermal: one thermal_read(0) group copy under the BSP cache lock.
 *   - meminfo: one pmm_get_stats() locked snapshot (Task 3).
 *
 * Split from procfs.c (which keeps readdir/legacy reads) so the lock-
 * taking producers + the pure formatters (procfs_fmt.c) are host-
 * testable (hosttests CASE=procfs) without the VFS/mmaps surface.
 */

#include <fs/procfs.h>
#include <fs/procfs_fmt.h>
#include <fs/file.h>
#include <fs/vfs.h>
#include <sched/task.h>
#include <sched/accounting.h>
#include <arch/thermal.h>
#include <arch/clocksource.h>
#include <arch/spinlock.h>
#include <memory/pmm.h>
#include <memory/slab.h>

#include <errno.h>
#include <kernel.h>      /* container_of */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Fixed open-time capacities.  status reserves >= 4096 bytes (brief
 * Step 4); the encoded Name alone can need 63*4+1. */
#define PROCFS_STATUS_CAP  4096
#define PROCFS_STAT_CAP    1024
#define PROCFS_THERMAL_CAP 512
#define PROCFS_MEMINFO_CAP 512

struct procfs_snapshot {
    size_t len;
    char   data[];
};

// ── state char for status (same mapping as the old gen_status) ──
static char snap_state_char(int64_t state)
{
    if (state & TASK_RUNNING)         return 'R';
    if (state & TASK_INTERRUPTIBLE)   return 'S';
    if (state & TASK_UNINTERRUPTIBLE) return 'D';
    if (state & TASK_ZOMBIE)          return 'Z';
    if (state & TASK_STOPPED)         return 'T';
    return '?';
}

/* Find a task by pid.  Caller MUST hold task_list_lock. */
static task_t *snap_find_task_locked(int pid)
{
    list_t *pos = init_task_union.task.list.next;
    while (pos != &init_task_union.task.list) {
        task_t *t = container_of(pos, task_t, list);
        if ((int)t->pid == pid)
            return t;
        pos = pos->next;
    }
    return NULL;
}

// ── /proc/<pid>/status ────────────────────────────────────────────
static int build_status(uint32_t pid, struct procfs_snapshot **out)
{
    struct procfs_snapshot *s =
        kmalloc(sizeof(*s) + PROCFS_STATUS_CAP);
    if (!s)
        return -ENOMEM;

    struct procfs_task_snap ts;
    memset(&ts, 0, sizeof(ts));

    int found = 0;
    uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
    task_t *t = snap_find_task_locked((int)pid);
    if (t) {
        ts.pid  = (uint64_t)t->pid;
        /* parent's PID is read under the same lock that protects the
         * task lifetime — the pointer itself is NOT retained. */
        ts.ppid = t->parent ? (uint64_t)t->parent->pid : 0;
        ts.state_c = snap_state_char(t->state);
        ts.last_cpu = t->cpu;
        ts.priority = (int32_t)t->priority;
        memcpy(ts.comm, t->comm, sizeof(ts.comm));
        ts.comm[sizeof(ts.comm) - 1] = '\0';
        ts.birth_id = t->birth_id;
        ts.start_ns = t->start_ns;
        /* runtime is tick-accumulated on any CPU — atomic read (§2.4) */
        ts.runtime_ticks =
            __atomic_load_n(&t->runtime_ticks, __ATOMIC_RELAXED);
        ts.sample_ns = arch_clocksource_read_ns();
        found = 1;
    }
    spin_unlock_irqrestore(&task_list_lock, tl_flags);

    if (!found) {
        kfree(s);
        return -ENOENT;
    }

    /* Formatting runs OUTSIDE task_list_lock (spec §5.1: no formatting
     * under the task lock). */
    int n = procfs_fmt_status(&ts, s->data, PROCFS_STATUS_CAP);
    if (n < 0) {
        kfree(s);
        return n;
    }
    s->len = (size_t)n;
    *out = s;
    return 0;
}

// ── /proc/stat ────────────────────────────────────────────────────
static int build_stat(struct procfs_snapshot **out)
{
    struct procfs_snapshot *s =
        kmalloc(sizeof(*s) + PROCFS_STAT_CAP);
    if (!s)
        return -ENOMEM;

    struct procfs_stat_snap ss;
    memset(&ss, 0, sizeof(ss));
    ss.sample_ns = arch_clocksource_read_ns();
    ss.ncpu = NR_CPUS;   /* preallocate at maximum scale */

    for (unsigned cpu = 0; cpu < NR_CPUS; cpu++) {
        struct cpu_usage_snapshot cs;
        /* Per-CPU lock copy, unlocked immediately — never nested with
         * task_list_lock or any other lock (spec §2.4). */
        if (sched_accounting_read(cpu, &cs) != 0)
            continue;   /* slot stays zeroed; never fabricate a CPU */
        ss.cpu[cpu].online      = cs.online;
        ss.cpu[cpu].valid       = cs.valid;
        ss.cpu[cpu].tick_hz     = cs.tick_hz;
        ss.cpu[cpu].epoch       = cs.epoch;
        ss.cpu[cpu].busy_ticks  = cs.busy_ticks;
        ss.cpu[cpu].idle_ticks  = cs.idle_ticks;
    }

    int n = procfs_fmt_stat(&ss, s->data, PROCFS_STAT_CAP);
    if (n < 0) {
        kfree(s);
        return n;
    }
    s->len = (size_t)n;
    *out = s;
    return 0;
}

// ── /proc/thermal ─────────────────────────────────────────────────
static int build_thermal(struct procfs_snapshot **out)
{
    struct procfs_snapshot *s =
        kmalloc(sizeof(*s) + PROCFS_THERMAL_CAP);
    if (!s)
        return -ENOMEM;

    /* First version shows the BSP core reading only (spec §3.1) — the
     * group copy (state/reason/value/time) comes from thermal_read. */
    struct thermal_sample t;
    thermal_read(0, &t);

    struct procfs_thermal_snap ts;
    ts.temp_mc   = t.temp_mc;
    ts.state     = t.state;
    ts.reason    = t.reason;
    ts.cpu_id    = t.cpu_id;
    ts.source    = t.source;
    ts.sampled_ns = t.sampled_ns;

    int n = procfs_fmt_thermal(&ts, s->data, PROCFS_THERMAL_CAP);
    if (n < 0) {
        kfree(s);
        return n;
    }
    s->len = (size_t)n;
    *out = s;
    return 0;
}

// ── /proc/meminfo ─────────────────────────────────────────────────
static int build_meminfo(struct procfs_snapshot **out)
{
    struct procfs_snapshot *s =
        kmalloc(sizeof(*s) + PROCFS_MEMINFO_CAP);
    if (!s)
        return -ENOMEM;

    /* One locked PMM snapshot (Task 3); text built outside the lock. */
    struct pmm_stats ps;
    pmm_get_stats(&ps);

    struct procfs_meminfo_snap ms;
    ms.sample_ns     = arch_clocksource_read_ns();
    ms.managed_bytes = ps.managed_bytes;
    ms.reserved_bytes = ps.reserved_bytes;
    ms.free_bytes    = ps.free_bytes;

    int n = procfs_fmt_meminfo(&ms, s->data, PROCFS_MEMINFO_CAP);
    if (n < 0) {
        kfree(s);
        return n;
    }
    s->len = (size_t)n;
    *out = s;
    return 0;
}

// ── Legacy no-file_t node read (Task 1 contract fallback) ─────────
// A direct vfs_lookup + vfs_read on the node — kernel-internal readers,
// the KERNEL_SELFTEST procfs case — never runs the open_file hook, and
// "无 hook 走旧 node read" keeps that path legal.  Regenerate CURRENT
// content here through the SAME snapshot producers the open_file hook
// uses, so there is exactly one source of formatting truth (the
// per-open stable snapshot remains the primary path for fd reads).
// Returns bytes written, or a negative errno (-ENOENT for a vanished
// status PID, -EOVERFLOW if the text exceeds cap).
int procfs_generate_node(uint32_t type, uint32_t pid, char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return -EINVAL;

    struct procfs_snapshot *s = NULL;
    int rc;
    switch (type) {
    case PROCFS_TYPE_STATUS:
        rc = build_status(pid, &s);
        break;
    case PROCFS_TYPE_STAT:
        rc = build_stat(&s);
        break;
    case PROCFS_TYPE_THERMAL:
        rc = build_thermal(&s);
        break;
    case PROCFS_TYPE_MEMINFO:
        rc = build_meminfo(&s);
        break;
    default:
        return -EINVAL;
    }
    if (rc < 0)
        return rc;

    if (s->len >= cap) {
        kfree(s);
        return -EOVERFLOW;
    }
    memcpy(buf, s->data, s->len);
    int n = (int)s->len;
    kfree(s);
    return n;
}

// ── Per-file snapshot lifecycle hooks (spec §6, Task 1 plumbing) ──

int procfs_open_file(struct file *f)
{
    if (!f || !f->node)
        return -EINVAL;

    uint32_t type = PROCFS_TYPE(f->node->fs_data);
    uint32_t pid  = PROCFS_PID(f->node->fs_data);

    /* "self" sentinel resolves to the current task's PID */
    if (pid == PROCFS_PID_SELF) {
        task_t *cur = current;
        if (!cur)
            return -ENOENT;
        pid = (uint32_t)cur->pid;
    }

    struct procfs_snapshot *s = NULL;
    int rc;
    switch (type) {
    case PROCFS_TYPE_STATUS:
        rc = build_status(pid, &s);
        break;
    case PROCFS_TYPE_STAT:
        rc = build_stat(&s);
        break;
    case PROCFS_TYPE_THERMAL:
        rc = build_thermal(&s);
        break;
    case PROCFS_TYPE_MEMINFO:
        rc = build_meminfo(&s);
        break;
    default:
        /* maps / fd entries keep the legacy regenerate-per-read path. */
        return 0;
    }

    if (rc < 0)
        return rc;   /* nothing attached — release stays a no-op */
    f->fs_private = s;
    return 0;
}

int64_t procfs_read_file(struct file *f, uint64_t offset,
                         uint64_t size, void *buffer)
{
    if (!f || !f->node || !buffer)
        return -EINVAL;

    struct procfs_snapshot *s = (struct procfs_snapshot *)f->fs_private;
    if (!s)
        return vfs_read(f->node, offset, size, buffer);  /* legacy nodes */

    if (offset >= s->len)
        return 0;                       /* stable EOF */
    uint64_t n = s->len - offset;
    if (n > size)
        n = size;
    memcpy(buffer, s->data + offset, n);
    return (int64_t)n;
}

void procfs_release_file(struct file *f)
{
    if (!f)
        return;
    /* A failed open left nothing attached: this must be a safe no-op,
     * never a double free. */
    if (f->fs_private) {
        kfree(f->fs_private);
        f->fs_private = NULL;
    }
}

// ── /proc root task-range readdir (spec §5.2) ─────────────────────
// One traversal under task_list_lock: a task deleted concurrently is
// unlinked under the same lock, so the scan can never step on freed
// task memory.  Across calls the scan is still dynamic — entries may
// appear/disappear between two readdir calls; user space dedups by PID
// and accepts next-cycle backfill.
int procfs_readdir_tasks(uint64_t index, struct vfs_dirent *entry,
                         uint64_t base)
{
    if (!entry || index < base)
        return -EINVAL;

    uint64_t kidx = index - base;
    int found = 0;

    uint64_t tl_flags = spin_lock_irqsave(&task_list_lock);
    list_t *pos = init_task_union.task.list.next;
    while (pos != &init_task_union.task.list) {
        task_t *t = container_of(pos, task_t, list);
        pos = pos->next;
        /* Zombies keep their task_struct until reaped and stay listed
         * (spec §5.2: default list shows the existing procfs range,
         * including zombies). */
        if (t->pid > 0 && !(t->flags & PF_KTHREAD)) {
            if (kidx == 0) {
                char name[32];
                int n = snprintf(name, sizeof(name), "%d", (int)t->pid);
                memcpy(entry->name, name, (size_t)n + 1);
                entry->type = VFS_DIR;
                entry->size = 0;
                entry->ino  = (uint32_t)(uintptr_t)PROCFS_ENCODE(
                    PROCFS_TYPE_PID_DIR, (uint32_t)t->pid);
                found = 1;
                break;
            }
            kidx--;
        }
    }
    spin_unlock_irqrestore(&task_list_lock, tl_flags);

    if (!found)
        entry->name[0] = '\0';
    return 0;
}
