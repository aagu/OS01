#ifndef _FS_PROCFS_FMT_H
#define _FS_PROCFS_FMT_H

/*
 * fs/procfs_fmt.h — pure formatting + Name encoding for the procfs
 * statistics files (monitor design §5.2/§6, plan Task 4).
 *
 * Everything here is lock-free and allocation-free: the caller copies a
 * consistent snapshot from its data-source lock FIRST (procfs_snap.c)
 * and formats outside the lock.  Every formatter checks snprintf's
 * return value and returns -EOVERFLOW instead of silently truncating
 * into plausible-looking data (spec §6: "不能静默截断成合法数据").
 *
 * This TU is deliberately standalone (no kernel headers beyond stdint/
 * stddef) so the host harness compiles it unmodified.
 */

#include <stddef.h>
#include <stdint.h>

#ifndef NR_CPUS
#define NR_CPUS 8
#endif

/* The fallback above only fires when the real arch value is not visible
 * yet; if the arch NR_CPUS ever drifts away from 8, procfs_stat_snap
 * would silently mis-size cpu[] and /proc/stat would truncate cores.
 * Fail loudly at compile time instead (monitor design Task 4 review). */
_Static_assert(NR_CPUS == 8,
               "procfs_fmt.h fallback NR_CPUS != arch NR_CPUS: "
               "/proc/stat cpu[] sizing would truncate cores");

/* task_t comm is NUL-terminated and holds at most 63 bytes + NUL. */
#define PROCFS_COMM_MAX 64
#define PROCFS_COMM_DATA_MAX (PROCFS_COMM_MAX - 1)

/* §6: every comm byte encodes to at most 4 output bytes ('\xHH'), so
 * the encoded Name buffer reserves 63*4 bytes plus the terminator. */
#define PROCFS_NAME_ENC_MAX (PROCFS_COMM_DATA_MAX * 4 + 1)

/* ── Copied-out snapshots (no pointers — safe to format unlocked) ── */

/* /proc/<pid>/status — identity fields copied under task_list_lock. */
struct procfs_task_snap {
    uint64_t pid;
    uint64_t ppid;
    uint64_t birth_id;
    uint64_t start_ns;
    uint64_t runtime_ticks;
    uint64_t sample_ns;
    uint32_t last_cpu;
    int32_t  priority;
    char     state_c;                    /* mapped from task state flags */
    char     comm[PROCFS_COMM_MAX];
};

/* /proc/stat — one per-CPU entry per NR_CPUS slot, always preallocated
 * at the maximum scale (spec: "CPU 用 NR_CPUS 最大规模预分配"). */
struct procfs_stat_cpu {
    uint32_t online;
    uint32_t valid;
    uint32_t tick_hz;
    uint64_t epoch;
    uint64_t busy_ticks;
    uint64_t idle_ticks;
};

struct procfs_stat_snap {
    uint64_t sample_ns;
    uint32_t ncpu;                      /* number of valid cpu[] entries */
    struct procfs_stat_cpu cpu[NR_CPUS];
};

/* /proc/thermal — group copy of the thermal cache entry. */
struct procfs_thermal_snap {
    int32_t  temp_mc;
    uint32_t state;                     /* enum thermal_state */
    uint32_t reason;                    /* enum thermal_reason */
    uint32_t cpu_id;
    uint32_t source;                    /* enum thermal_source */
    uint64_t sampled_ns;
};

/* /proc/meminfo — bytes in, kB out (spec §4.1 keeps the kB units). */
struct procfs_meminfo_snap {
    uint64_t sample_ns;
    uint64_t managed_bytes;
    uint64_t reserved_bytes;            /* PMM 已占用（含池保留页） */
    uint64_t free_bytes;
};

/* ── Name encoding (spec §6) ────────────────────────────────────────
 * Printable ASCII (0x20–0x7e) except backslash passes through verbatim;
 * backslash encodes as two 0x5c bytes; any other byte encodes as
 * 0x5c 0x78 + two lowercase hex digits.  The result therefore never
 * contains a newline and always fits one line.  out must have room for
 * the terminator.  Returns the encoded length or -EOVERFLOW (-EINVAL
 * on NULL arguments). */
int procfs_encode_name(const char *comm, char *out, size_t outsz);

/* ── Formatters: all return the byte count (excluding NUL) or
 * -EOVERFLOW / -EINVAL.  Output is always NUL-terminated on success. */
int procfs_fmt_status(const struct procfs_task_snap *s,
                      char *buf, size_t bufsz);
int procfs_fmt_stat(const struct procfs_stat_snap *s,
                    char *buf, size_t bufsz);
int procfs_fmt_thermal(const struct procfs_thermal_snap *s,
                       char *buf, size_t bufsz);
int procfs_fmt_meminfo(const struct procfs_meminfo_snap *s,
                       char *buf, size_t bufsz);

/* Fixed reason→string table (spec §6 thermal output).  Returns one of
 * none/backend_unavailable/hypervisor/vendor/no_dts/model/not_ready/
 * worker_start/msr/target/reading, or "unknown" for an out-of-range
 * value (defensive — the enum cannot produce one). */
const char *procfs_thermal_reason_str(uint32_t reason);

#endif /* _FS_PROCFS_FMT_H */
