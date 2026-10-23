#ifndef MONITOR_COLLECT_H
#define MONITOR_COLLECT_H

/*
 * user/monitor/collect.h — monitor plan Task 6: user-space collection
 * and strict parsing of the procfs statistics files.
 *
 * Spec: docs/superpowers/specs/2026-10-09-monitor-design.md §4.1, §5.2,
 * §6.  This module owns everything /proc: it reads /proc/stat,
 * /proc/thermal, /proc/meminfo and /proc/<pid>/status with bounded
 * reads, parses the OS01 key-value format strictly, and hands the rest
 * of the monitor (model/render) plain structs.  It never keeps a
 * pointer into file text: every string (process Name) is decoded into
 * an owned buffer inside the output struct.
 *
 * Validity contract (spec §6), per file:
 *   - unknown fields are ignored; field order and whitespace count do
 *     not matter;
 *   - Version must be present and equal 1; /proc/stat TickHz must be
 *     positive (the parser does not hardcode the current 100 Hz rate);
 *   - a required field missing, a recognized field duplicated, a
 *     numeric overflow or an illegal value make the WHOLE file invalid
 *     (monitor_parse_* -> -EINVAL).  The single exception is status: a
 *     malformed per-PID file only drops that one process.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Protocol scale: the kernel's NR_CPUS (kernel/include/arch/cpu.h).
 * The 8-core stat text is what /proc/stat can maximally emit, and the
 * read caps below mirror the kernel snapshot capacities in
 * kernel/fs/procfs_snap.c, so a "file too long" on the user side
 * corresponds to what the kernel could never produce. */
#define MONITOR_MAX_CPU 8

/* comm/Name cap: 63 data bytes + terminator (kernel PROCFS_COMM_MAX). */
#define MONITOR_COMM_MAX 64

/* Read caps per statistics file (byte-matched to procfs_snap.c):
 *   status 4096; stat 1024 = 8-core protocol maximum text (3 header
 *   lines plus 8 "cpuN:" key=value lines of <= 100 bytes each);
 *   thermal/meminfo 512.  A file longer than its cap is invalid, never
 *   silently truncated. */
#define MONITOR_STATUS_CAP  4096
#define MONITOR_STAT_CAP    1024
#define MONITOR_THERMAL_CAP 512
#define MONITOR_MEMINFO_CAP 512

/* ── One CPU's published accounting (sched/accounting.h fields plus
 * its cpu_id; tick_hz comes from the file-wide TickHz line) ──────── */
struct monitor_cpu_sample {
    uint32_t cpu_id;
    uint32_t online;
    uint32_t valid;          /* 0 = tick source never started */
    uint64_t epoch;          /* changes on reset / re-onlining */
    uint64_t busy_ticks;
    uint64_t idle_ticks;
    uint32_t tick_hz;
};

struct monitor_stat_sample {
    uint64_t sampled_ns;
    uint32_t tick_hz;
    uint32_t ncpu;           /* populated entries in cpu[] */
    struct monitor_cpu_sample cpu[MONITOR_MAX_CPU];
};

/* PMM picture in BYTES (meminfo keeps kB on the wire; §4.1):
 * T = managed, U = used (PMM-occupied, incl. pool retention),
 * F = free; a normal snapshot satisfies T == U + F.  T == 0 means the
 * memory sample is unavailable. */
struct monitor_mem_sample {
    uint64_t sampled_ns;
    uint64_t total_bytes;    /* T */
    uint64_t used_bytes;     /* U */
    uint64_t free_bytes;     /* F */
};

/* Mirrors the kernel enum thermal_state (arch/thermal.h) plus a marker
 * for state strings outside the fixed table. */
enum monitor_thermal_state {
    MONITOR_THERMAL_STATE_OK = 0,
    MONITOR_THERMAL_STATE_UNSUPPORTED,
    MONITOR_THERMAL_STATE_INVALID,
    MONITOR_THERMAL_STATE_READ_ERROR,
    MONITOR_THERMAL_STATE_STALE,
    MONITOR_THERMAL_STATE_UNKNOWN,
};

/* Mirrors the kernel's fixed transport table (procfs_thermal_reason_str)
 * plus an unknown marker: a Reason string outside the table is kept as
 * UNKNOWN and never mistaken for a normal/expected condition. */
enum monitor_thermal_reason {
    MONITOR_THERMAL_REASON_NONE = 0,
    MONITOR_THERMAL_REASON_BACKEND_UNAVAILABLE,
    MONITOR_THERMAL_REASON_HYPERVISOR,
    MONITOR_THERMAL_REASON_VENDOR,
    MONITOR_THERMAL_REASON_NO_DTS,
    MONITOR_THERMAL_REASON_MODEL,
    MONITOR_THERMAL_REASON_NOT_READY,
    MONITOR_THERMAL_REASON_WORKER_START,
    MONITOR_THERMAL_REASON_MSR,
    MONITOR_THERMAL_REASON_TARGET,
    MONITOR_THERMAL_REASON_READING,
    MONITOR_THERMAL_REASON_UNKNOWN,
};

enum monitor_thermal_source {
    MONITOR_THERMAL_SOURCE_NONE = 0,
    MONITOR_THERMAL_SOURCE_CORE_DTS,
    MONITOR_THERMAL_SOURCE_PACKAGE_DTS,
    MONITOR_THERMAL_SOURCE_UNKNOWN,
};

struct monitor_thermal_sample {
    enum monitor_thermal_state state;    /* usability is decided here */
    enum monitor_thermal_reason reason;
    enum monitor_thermal_source source;
    int32_t temp_mc;         /* millidegrees; only when state == OK */
    uint32_t cpu_id;
    uint64_t sampled_ns;
};

/* One task line (spec §5.1 process_sample plus start_ns).  comm is the
 * DECODED name, owned by this struct — never a pointer into file
 * text. */
struct monitor_process_sample {
    uint64_t pid, ppid, birth_id;
    uint64_t sampled_ns, start_ns, runtime_ticks;
    uint32_t last_cpu;       /* last/current scheduling CPU */
    /* R / S / D / T / Z, or '?' — the kernel's defensive fallback
     * (snap_state_char) for state bits matching none of the five; the
     * row is kept and display decides how to render it. */
    char state;
    char comm[MONITOR_COMM_MAX];
};

/* One full collection round.  The process array is dynamically
 * allocated and OWNED by this struct: release it with
 * monitor_sample_destroy (exactly once; idempotent afterwards). */
struct monitor_sample {
    int stat_valid;          /* 0 = /proc/stat unusable this round */
    struct monitor_stat_sample stat;

    int mem_valid;
    struct monitor_mem_sample mem;

    int thermal_valid;
    struct monitor_thermal_sample thermal;

    struct monitor_process_sample *processes;  /* owned array */
    uint32_t nprocesses;
    uint32_t process_cap;    /* internal: allocated slots */
};

/* Collect one round.  Each primary metric (stat/mem/thermal) that
 * fails to read or parse is merely marked invalid in *out; the round
 * still succeeds.  Per-PID status problems drop only that process.
 * Returns 0, or -ENOMEM when the round's process array could not be
 * provided (the partial array is freed and *out is left empty/valid to
 * destroy).  *out may be a fresh, zeroed or previously collected
 * struct; any previous round is released first. */
int monitor_collect(struct monitor_sample *out);

/* Release the owned process array and reset *out to the zeroed state.
 * NULL and already-destroyed structs are safe no-ops. */
void monitor_sample_destroy(struct monitor_sample *out);

/* ── Parsers: consume exactly len bytes of text (which need not be
 * NUL-terminated) and fill the output struct.  Return 0 on success,
 * -EINVAL for an invalid file, -ENOMEM if an allocation failed.  The
 * output is zeroed on entry and left zeroed on failure. ───────────── */
int monitor_parse_stat(const char *text, size_t len,
                       struct monitor_stat_sample *out);
int monitor_parse_meminfo(const char *text, size_t len,
                          struct monitor_mem_sample *out);
int monitor_parse_status(const char *text, size_t len,
                         struct monitor_process_sample *out);
int monitor_parse_thermal(const char *text, size_t len,
                          struct monitor_thermal_sample *out);

#ifdef __cplusplus
}
#endif

#endif /* MONITOR_COLLECT_H */
