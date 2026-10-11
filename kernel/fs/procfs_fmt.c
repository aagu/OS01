/*
 * kernel/fs/procfs_fmt.c — pure formatting + Name encoding for the
 * procfs statistics files (monitor plan Task 4, spec §5.2/§6).
 *
 * See fs/procfs_fmt.h for the contract.  This file never takes a lock
 * and never allocates: procfs_snap.c copies each data source under its
 * own lock into the plain snapshot structs and formats here, outside.
 */

#include <fs/procfs_fmt.h>
#include <arch/thermal.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ── emit: one checked snprintf step ────────────────────────────────
 * Any overrun (negative return or truncation) aborts with -EOVERFLOW —
 * the caller discards the whole buffer, so truncated text is never
 * attached to a file. */
static int emit(char *buf, size_t bufsz, size_t *off, const char *fmt, ...)
{
    if (*off >= bufsz)
        return -EOVERFLOW;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *off, bufsz - *off, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= bufsz - *off)
        return -EOVERFLOW;
    *off += (size_t)n;
    return 0;
}

/* ── Name encoding (spec §6) ──────────────────────────────────────── */
int procfs_encode_name(const char *comm, char *out, size_t outsz)
{
    if (!comm || !out || outsz == 0)
        return -EINVAL;

    size_t len = strnlen(comm, PROCFS_COMM_MAX);
    if (len > PROCFS_COMM_DATA_MAX)
        len = PROCFS_COMM_DATA_MAX;

    static const char hex[] = "0123456789abcdef";
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)comm[i];
        if (c >= 0x20 && c <= 0x7e && c != '\\') {
            if (o + 1 >= outsz) return -EOVERFLOW;
            out[o++] = (char)c;
        } else if (c == '\\') {
            if (o + 2 >= outsz) return -EOVERFLOW;
            out[o++] = '\\';
            out[o++] = '\\';
        } else {
            if (o + 4 >= outsz) return -EOVERFLOW;
            out[o++] = '\\';
            out[o++] = 'x';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0x0f];
        }
    }
    out[o] = '\0';
    return (int)o;
}

/* ── /proc/<pid>/status ──────────────────────────────────────────────
 * Keeps every pre-existing field (Name/Pid/PPid/State/CPU/Priority)
 * and adds the monitor identity block.  Every statistics file carries
 * Version + SampleNs (brief Step 4). */
int procfs_fmt_status(const struct procfs_task_snap *s,
                      char *buf, size_t bufsz)
{
    if (!s || !buf || bufsz == 0)
        return -EINVAL;

    char name[PROCFS_NAME_ENC_MAX];
    int nlen = procfs_encode_name(s->comm, name, sizeof(name));
    if (nlen < 0)
        return nlen;

    size_t off = 0;
    int rc;
    if ((rc = emit(buf, bufsz, &off, "Version:\t1\n")) < 0) return rc;
    if ((rc = emit(buf, bufsz, &off, "Name:\t%s\n", name)) < 0) return rc;
    if ((rc = emit(buf, bufsz, &off, "Pid:\t%lu\n",
                   (unsigned long)s->pid)) < 0) return rc;
    if ((rc = emit(buf, bufsz, &off, "PPid:\t%lu\n",
                   (unsigned long)s->ppid)) < 0) return rc;
    if ((rc = emit(buf, bufsz, &off, "State:\t%c\n", s->state_c)) < 0) return rc;
    if ((rc = emit(buf, bufsz, &off, "CPU:\t%u\n",
                   (unsigned)s->last_cpu)) < 0) return rc;
    if ((rc = emit(buf, bufsz, &off, "Priority:\t%d\n",
                   (int)s->priority)) < 0) return rc;
    if ((rc = emit(buf, bufsz, &off, "BirthId:\t%lu\n",
                   (unsigned long)s->birth_id)) < 0) return rc;
    if ((rc = emit(buf, bufsz, &off, "StartNs:\t%lu\n",
                   (unsigned long)s->start_ns)) < 0) return rc;
    if ((rc = emit(buf, bufsz, &off, "SampleNs:\t%lu\n",
                   (unsigned long)s->sample_ns)) < 0) return rc;
    if ((rc = emit(buf, bufsz, &off, "RuntimeTicks:\t%lu\n",
                   (unsigned long)s->runtime_ticks)) < 0) return rc;
    return (int)off;
}

/* ── /proc/stat ──────────────────────────────────────────────────────
 * OS01's own key-value format, not the Linux fixed-column ABI (§6).
 * Cores whose tick source never started carry valid=0 — the snapshot
 * never fabricates per-CPU zeros for them. */
int procfs_fmt_stat(const struct procfs_stat_snap *s,
                    char *buf, size_t bufsz)
{
    if (!s || !buf || bufsz == 0)
        return -EINVAL;
    if (s->ncpu > NR_CPUS)
        return -EINVAL;

    uint32_t tick_hz = 0;
    for (uint32_t i = 0; i < s->ncpu; i++) {
        if (s->cpu[i].valid) {
            tick_hz = s->cpu[i].tick_hz;
            break;
        }
    }

    size_t off = 0;
    int rc;
    if ((rc = emit(buf, bufsz, &off, "Version:\t1\n")) < 0) return rc;
    if ((rc = emit(buf, bufsz, &off, "SampleNs:\t%lu\n",
                   (unsigned long)s->sample_ns)) < 0) return rc;
    if ((rc = emit(buf, bufsz, &off, "TickHz:\t%u\n",
                   (unsigned)tick_hz)) < 0) return rc;
    for (uint32_t i = 0; i < s->ncpu; i++) {
        if ((rc = emit(buf, bufsz, &off,
                       "cpu%u:\tonline=%u valid=%u epoch=%lu busy=%lu idle=%lu\n",
                       (unsigned)i,
                       (unsigned)s->cpu[i].online,
                       (unsigned)s->cpu[i].valid,
                       (unsigned long)s->cpu[i].epoch,
                       (unsigned long)s->cpu[i].busy_ticks,
                       (unsigned long)s->cpu[i].idle_ticks)) < 0) return rc;
    }
    return (int)off;
}

/* ── /proc/thermal ─────────────────────────────────────────────────── */

static const char *thermal_state_str(uint32_t state)
{
    switch (state) {
    case THERMAL_OK:           return "ok";
    case THERMAL_UNSUPPORTED:  return "unsupported";
    case THERMAL_INVALID:      return "invalid";
    case THERMAL_READ_ERROR:   return "read_error";
    case THERMAL_STALE:        return "stale";
    default:                   return "unknown";
    }
}

/* Fixed reason table (brief Step 4) — exactly the ten transport
 * strings; out-of-range values cannot come from the enum but are still
 * mapped defensively instead of indexing out of bounds. */
const char *procfs_thermal_reason_str(uint32_t reason)
{
    switch (reason) {
    case THERMAL_REASON_NONE:               return "none";
    case THERMAL_REASON_BACKEND_UNAVAILABLE: return "backend_unavailable";
    case THERMAL_REASON_HYPERVISOR:         return "hypervisor";
    case THERMAL_REASON_VENDOR:             return "vendor";
    case THERMAL_REASON_NO_DTS:             return "no_dts";
    case THERMAL_REASON_MODEL:              return "model";
    case THERMAL_REASON_NOT_READY:          return "not_ready";
    case THERMAL_REASON_WORKER_START:       return "worker_start";
    case THERMAL_REASON_MSR:                return "msr";
    case THERMAL_REASON_TARGET:             return "target";
    case THERMAL_REASON_READING:            return "reading";
    default:                                return "unknown";
    }
}

static const char *thermal_source_str(uint32_t source)
{
    switch (source) {
    case THERMAL_SOURCE_CORE_DTS:    return "core_dts";
    case THERMAL_SOURCE_PACKAGE_DTS: return "package_dts";
    default:                         return "none";
    }
}

int procfs_fmt_thermal(const struct procfs_thermal_snap *s,
                       char *buf, size_t bufsz)
{
    if (!s || !buf || bufsz == 0)
        return -EINVAL;

    size_t off = 0;
    int rc;
    if ((rc = emit(buf, bufsz, &off, "Version:\t1\n")) < 0) return rc;
    if ((rc = emit(buf, bufsz, &off, "State:\t%s\n",
                   thermal_state_str(s->state))) < 0) return rc;
    if ((rc = emit(buf, bufsz, &off, "Reason:\t%s\n",
                   procfs_thermal_reason_str(s->reason))) < 0) return rc;
    if ((rc = emit(buf, bufsz, &off, "Source:\t%s\n",
                   thermal_source_str(s->source))) < 0) return rc;
    if ((rc = emit(buf, bufsz, &off, "SampleNs:\t%lu\n",
                   (unsigned long)s->sampled_ns)) < 0) return rc;
    if (s->state == THERMAL_OK) {
        if ((rc = emit(buf, bufsz, &off, "TempMilliC:\t%d\n",
                       (int)s->temp_mc)) < 0) return rc;
        if ((rc = emit(buf, bufsz, &off, "CpuId:\t%u\n",
                       (unsigned)s->cpu_id)) < 0) return rc;
    }
    return (int)off;
}

/* ── /proc/meminfo ───────────────────────────────────────────────────
 * Keeps the existing kB units; the snapshot is bytes (spec §4.1). */
int procfs_fmt_meminfo(const struct procfs_meminfo_snap *s,
                       char *buf, size_t bufsz)
{
    if (!s || !buf || bufsz == 0)
        return -EINVAL;

    size_t off = 0;
    int rc;
    if ((rc = emit(buf, bufsz, &off, "Version:\t1\n")) < 0) return rc;
    if ((rc = emit(buf, bufsz, &off, "SampleNs:\t%lu\n",
                   (unsigned long)s->sample_ns)) < 0) return rc;
    if ((rc = emit(buf, bufsz, &off, "MemTotal:\t%lu kB\n",
                   (unsigned long)(s->managed_bytes / 1024))) < 0) return rc;
    if ((rc = emit(buf, bufsz, &off, "MemFree:\t%lu kB\n",
                   (unsigned long)(s->free_bytes / 1024))) < 0) return rc;
    if ((rc = emit(buf, bufsz, &off, "MemUsed:\t%lu kB\n",
                   (unsigned long)(s->reserved_bytes / 1024))) < 0) return rc;
    if ((rc = emit(buf, bufsz, &off, "PageSize:\t2048 kB\n")) < 0) return rc;
    return (int)off;
}
