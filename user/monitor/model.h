#ifndef MONITOR_MODEL_H
#define MONITOR_MODEL_H

/*
 * user/monitor/model.h — monitor plan Task 7: delta model and bounded
 * history (spec §2.3, §5.3, §7).
 *
 * Consumes one collected round (monitor_sample, Task 6) and turns it
 * into display-ready, DIFFERENTIAL data:
 *
 *   - per-CPU and total CPU% from consecutive valid snapshots (only
 *     cores valid in BOTH rounds with equal epoch and tick_hz
 *     contribute; a core's full accumulated counters never enter a
 *     period delta);
 *   - per-process CPU% keyed by (pid, birth_id) using that record's own
 *     SampleNs (spec §5.3);
 *   - bounded 60-slot display history for the total CPU% and the memory
 *     ratio; a round where a metric is missing pushes an EMPTY point
 *     (valid bit clear) — never a stale value copied forward.
 *
 * Percentages are integers scaled to 10000 = 100.00%, rounded to one
 * decimal for display.  Every sum and scaled multiply/divide runs in
 * __uint128_t; the unclamped ratio is kept alongside the display value
 * as a diagnostic.  All two-term counter/timestamp subtractions
 * compare first and subtract afterwards (no wraparound).  The model
 * never reads files and never talks to the kernel: garbage in the
 * sample is the collector's problem.
 *
 * Validity contract (spec §2.3/§5.3): first round, counter regression,
 * epoch change, differing tick_hz or a zero denominator all render as
 * invalid (display shows "--") while the NEW counters are kept as the
 * rebuilt baseline.  Records not seen in a round are removed together
 * with their history.  An allocation failure (-ENOMEM) must never
 * destroy the existing baseline or display rows: the failed round is
 * not committed as data, but both history rings advance with EMPTY
 * points so a lost round does not compress the time axis.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* target C library: bool/stdint via the OS01 libc headers */
#include <monitor/collect.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Display history length (spec §7: 60 time slots, ~60s at one round/s). */
#define MONITOR_MODEL_HISTORY 60

/* One displayed percentage.  valid == false means "--".  percent_10000
 * is the display value rounded to one decimal (10000 = 100.00%);
 * process rows clamp it at 10000 (spec §5.3: current single-task
 * display may be limited to 100%).  raw_10000 keeps the UNCLAMPED
 * rounded ratio for diagnostics and ordering; it is only meaningful
 * when valid is true. */
struct monitor_pct {
    bool valid;
    uint32_t percent_10000;
    __uint128_t raw_10000;
};

/* One per-CPU display row (sorted view, see monitor_model). */
struct monitor_cpu_row {
    uint32_t cpu_id;
    bool online;             /* carried from the sample for the header */
    struct monitor_pct pct;
};

/* One process display row (sorted view).  comm is a copy owned by the
 * model's row array. */
struct monitor_process_row {
    uint64_t pid, ppid, birth_id;
    uint32_t last_cpu;
    char state;
    char comm[MONITOR_COMM_MAX];
    /* Reserved for a future policy that keeps unreadable old rows
     * (spec §5.3: such rows must never display an old CPU% as
     * current).  The current model REMOVES unseen records, so rows
     * are always fresh; the flag exists so the renderer can treat the
     * case explicitly rather than by absence. */
    bool stale;
    struct monitor_pct cpu;
};

/* One history point.  valid == false is an empty slot (sample missing
 * or metric invalid this round); percent_10000 is the same display
 * scale as monitor_pct. */
struct monitor_hist_point {
    bool valid;
    uint32_t percent_10000;
};

/* Bounded ring of the last MONITOR_MODEL_HISTORY points: count is the
 * number of live points (0..MONITOR_MODEL_HISTORY), head the index of
 * the OLDEST one.  monitor_history_get() indexes by age. */
struct monitor_history {
    unsigned count;
    unsigned head;
    struct monitor_hist_point slot[MONITOR_MODEL_HISTORY];
};

/* age 0 = newest point, count-1 = oldest.  Caller must ensure
 * age < h->count. */
static inline const struct monitor_hist_point *monitor_history_get(
    const struct monitor_history *h, unsigned age)
{
    unsigned idx = (h->head + h->count - 1 - age) % MONITOR_MODEL_HISTORY;
    return &h->slot[idx];
}

/* Per-CPU delta baseline: the last round's raw counters plus the key
 * that must match (cpu_id, epoch, tick_hz) for a continuous delta
 * (spec §2.3).  Internal state — exposed only because monitor_model
 * is a plain struct. */
struct monitor_cpu_base {
    bool seen;               /* slot occupied */
    bool valid_seen;         /* stored counters came from a VALID core
                              * sample (both rounds must be valid to
                              * form a continuous delta) */
    uint32_t cpu_id;
    uint64_t epoch;
    uint64_t busy_ticks, idle_ticks;
    uint32_t tick_hz;
    bool online;
};

/* Process delta baseline record (spec §5.3 key (pid, birth_id)). */
struct monitor_proc_base {
    uint64_t pid;
    uint64_t birth_id;
    uint64_t runtime_ticks;
    uint64_t sampled_ns;
};

struct monitor_model {
    /* ── display view of the most recent committed round ─────────── */
    struct monitor_pct total_cpu;    /* ΣΔB / Σ(ΔB+ΔI) over contributing cores */

    bool mem_valid;                  /* mem ratio this round */
    struct monitor_pct mem_pct;      /* 10000 × U / T */
    uint64_t mem_total_bytes, mem_used_bytes, mem_free_bytes;

    bool thermal_valid;
    struct monitor_thermal_sample thermal;   /* passthrough for the header */

    uint32_t ncpu_rows;              /* sorted: pct desc, cpu_id asc, invalid last */
    struct monitor_cpu_row cpu_rows[MONITOR_MAX_CPU];

    uint32_t nproc_rows;             /* sorted: pct desc, pid asc, invalid last */
    struct monitor_process_row *proc_rows;   /* owned array */

    /* ── bounded display history (60 slots, valid bit per point) ─── */
    struct monitor_history total_history;
    struct monitor_history mem_history;

    /* ── internal baselines (do not modify by hand) ──────────────── */
    uint32_t base_tick_hz;           /* last known stat TickHz, 0 = unknown */
    struct monitor_cpu_base cpu_base[MONITOR_MAX_CPU];
    struct monitor_proc_base *proc_base;     /* owned array */
    uint32_t nproc_base;
};

/* Initialize a freshly-allocated or stack struct monitor_model.
 * Safe on an uninitialized (garbage) struct: zero-first, performs
 * no frees. To reset an already-initialized model that may own
 * heap arrays, use monitor_model_reset() instead. */
void monitor_model_init(struct monitor_model *m);

/* Reset an already-initialized model back to the freshly-initialized
 * state, releasing any owned heap arrays and zeroing *m.
 * [PRECONDITION] *m must have been initialized (via monitor_model_init,
 * {0}, or prior update). Safe on a previously used model or a zeroed struct. */
void monitor_model_reset(struct monitor_model *m);

/* Difference one collected round against the model's baseline and, on
 * success, commit it (new baseline, new sorted rows, one history point
 * per metric — an invalid metric pushes an empty point).  Returns 0,
 * -EINVAL for NULL arguments, or -ENOMEM when a round array could not
 * be allocated; -ENOMEM preserves the previous baseline and display
 * snapshot, but appends one EMPTY point to each history ring. Invalid
 * arguments return -EINVAL without changing any model state. */
int monitor_model_update(struct monitor_model *m,
                         const struct monitor_sample *s);

/* Release the owned arrays and zero *m.
 * [PRECONDITION] *m must have been initialized (via monitor_model_init,
 * {0}, or prior update). NULL and already-destroyed models are safe no-ops. */
void monitor_model_destroy(struct monitor_model *m);

#ifdef __cplusplus
}
#endif

#endif /* MONITOR_MODEL_H */
