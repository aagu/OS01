/*
 * user/monitor/model.c — monitor plan Task 7: delta model and bounded
 * history (spec §2.3, §5.3, §7).
 *
 * Freestanding user-space code on OS01's libc (malloc/free only).
 * Design points:
 *
 *   - CPU deltas (spec §2.3): a core contributes only when it was
 *     valid in BOTH rounds with an unchanged (cpu_id, epoch, tick_hz)
 *     key; ΔB/ΔI come from compare-then-subtract (no wraparound), and
 *     a regression / epoch change / differing tick_hz / zero
 *     denominator invalidates that core while the NEW counters become
 *     the rebuilt baseline.  A newly seen (or re-armed) core never
 *     contributes its accumulated history to this period's delta.
 *     The total sums only the cores that produced a continuous delta
 *     this round: 10000 × ΣΔB / Σ(ΔB + ΔI).
 *   - Process deltas (spec §5.3): key (pid, birth_id); Δt uses THIS
 *     record's SampleNs, ΔR its RuntimeTicks;
 *     10000 × ΔR × 1e9 / (tick_hz × Δt).  First sight, birth_id
 *     change, Δt ≤ 0 and a runtime regression invalidate the row and
 *     keep the new baseline.  Migration does not reset runtime, exec
 *     keeps birth_id.  Records not seen in a round are removed with
 *     their history.  The tick_hz comes from the last valid /proc/stat
 *     (v1 is uniform 100Hz); a skipped stat round must not invalidate
 *     the task table.
 *   - 10000 = 100.00%.  All sums and scaled multiplies/divides run in
 *     __uint128_t; rounding uses the remainder n - (n/d)*d (never a
 *     128-bit modulo helper — the user runtime archive provides only
 *     division).  Process display clamps at 10000 while the
 *     diagnostic raw value keeps the unclamped ratio.
 *   - Bounded 60-slot history rings for the total CPU% and the memory
 *     ratio; a missing metric pushes an EMPTY point (valid bit clear),
 *     never a stale value copied forward.
 *   - Allocation discipline: both round arrays (sorted display rows
 *     and the new baseline) are allocated BEFORE data is committed,
 *     and model_compute() runs after them without further allocation —
 *     so -ENOMEM leaves the previous baseline and display rows exactly
 *     as they were. History still advances with empty points for that
 *     uncommitted sampling round.
 */

#include <monitor/model.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* ── small helpers ───────────────────────────────────────────────── */

/* Round num/den (both __uint128_t, den > 0) to the nearest integer,
 * capped at cap.  Remainder via n - (n/d)*d: the user runtime archive
 * has no 128-bit modulo entry point and none is introduced here. */
static __uint128_t div_round_cap(__uint128_t num, __uint128_t den,
                                 __uint128_t cap)
{
    __uint128_t q = num / den;
    __uint128_t r = num - q * den;

    if (q > cap)
        return cap;
    if (r * 2 >= den) {          /* r < den <= ~2^107 here: *2 fits */
        q++;
        if (q > cap)
            q = cap;
    }
    return q;
}

/* CPU% display value from the busy delta over the busy+idle sum:
 * 10000 × ΔB / (ΔB + ΔI), rounded; den > 0 guaranteed by the caller.
 * ΔB ≤ den, so the result is at most 10000 without a clamp. */
static uint32_t cpu_pct_10000(uint64_t dbusy, __uint128_t den,
                              __uint128_t *raw)
{
    __uint128_t q = div_round_cap((__uint128_t)10000 * dbusy, den, 10000);

    *raw = q;
    return (uint32_t)q;
}

/* Process% value from ΔR ticks over Δt ns at tick_hz:
 * 10000 × ΔR × 1e9 / (tick_hz × Δt), rounded.  The display value is
 * clamped at 10000 (spec §5.3); *raw keeps the unclamped ratio.  ΔR
 * and the 1e13 scale factor both fit comfortably in 128 bits. */
static uint32_t proc_pct_10000(uint64_t dr, uint64_t dt, uint32_t tick_hz,
                               __uint128_t *raw)
{
    __uint128_t num = (__uint128_t)dr * 10000000000000ULL;   /* 1e13 */
    __uint128_t den = (__uint128_t)tick_hz * dt;
    __uint128_t q = num / den;

    *raw = q;
    if (q > 10000)
        return 10000;
    {
        __uint128_t r = num - q * den;

        if (r * 2 >= den) {
            q++;
            return q > 10000 ? 10000 : (uint32_t)q;
        }
    }
    return (uint32_t)q;
}

static void push_history(struct monitor_history *h, bool valid,
                         uint32_t percent_10000)
{
    unsigned slot;

    if (h->count < MONITOR_MODEL_HISTORY) {
        slot = (h->head + h->count) % MONITOR_MODEL_HISTORY;
        h->count++;
    } else {
        slot = h->head;              /* overwrite the oldest point */
        h->head = (h->head + 1) % MONITOR_MODEL_HISTORY;
    }
    h->slot[slot].valid = valid;
    h->slot[slot].percent_10000 = percent_10000;
}

/* ── sorting (insertion sort; n is small and the predicate is stable) */

/* valid rows by raw ratio descending (key ascending as tie-break),
 * invalid rows last (key ascending) */
static bool proc_row_before(const struct monitor_process_row *a,
                            const struct monitor_process_row *b)
{
    if (a->cpu.valid != b->cpu.valid)
        return a->cpu.valid;
    if (a->cpu.valid && a->cpu.raw_10000 != b->cpu.raw_10000)
        return a->cpu.raw_10000 > b->cpu.raw_10000;
    return a->pid < b->pid;
}

static bool cpu_row_before(const struct monitor_cpu_row *a,
                           const struct monitor_cpu_row *b)
{
    if (a->pct.valid != b->pct.valid)
        return a->pct.valid;
    if (a->pct.valid && a->pct.raw_10000 != b->pct.raw_10000)
        return a->pct.raw_10000 > b->pct.raw_10000;
    return a->cpu_id < b->cpu_id;
}

static void proc_rows_sort(struct monitor_process_row *rows, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++) {
        struct monitor_process_row tmp = rows[i];
        uint32_t j = i;

        while (j > 0 && proc_row_before(&tmp, &rows[j - 1])) {
            rows[j] = rows[j - 1];
            j--;
        }
        rows[j] = tmp;
    }
}

static void cpu_rows_sort(struct monitor_cpu_row *rows, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++) {
        struct monitor_cpu_row tmp = rows[i];
        uint32_t j = i;

        while (j > 0 && cpu_row_before(&tmp, &rows[j - 1])) {
            rows[j] = rows[j - 1];
            j--;
        }
        rows[j] = tmp;
    }
}

/* ── lifecycle ───────────────────────────────────────────────────── */

static void model_free_arrays(struct monitor_model *m)
{
    free(m->proc_rows);
    free(m->proc_base);
    m->proc_rows = NULL;
    m->proc_base = NULL;
    m->nproc_rows = 0;
    m->nproc_base = 0;
}

void monitor_model_init(struct monitor_model *m)
{
    if (!m)
        return;
    model_free_arrays(m);
    memset(m, 0, sizeof(*m));
}

void monitor_model_destroy(struct monitor_model *m)
{
    if (!m)
        return;
    model_free_arrays(m);
    memset(m, 0, sizeof(*m));
}

/* ── per-round computation ───────────────────────────────────────── */

static struct monitor_cpu_base *cpu_base_slot(struct monitor_model *m,
                                              uint32_t cpu_id, bool alloc)
{
    struct monitor_cpu_base *free_slot = NULL;

    for (uint32_t i = 0; i < MONITOR_MAX_CPU; i++) {
        struct monitor_cpu_base *b = &m->cpu_base[i];

        if (b->seen && b->cpu_id == cpu_id)
            return b;
        if (!b->seen && !free_slot)
            free_slot = b;
    }
    return alloc ? free_slot : NULL;
}

static struct monitor_proc_base *proc_base_find(struct monitor_model *m,
                                                uint64_t pid)
{
    for (uint32_t i = 0; i < m->nproc_base; i++)
        if (m->proc_base[i].pid == pid)
            return &m->proc_base[i];
    return NULL;
}

/* Compute one round into the model's display arrays and baselines.
 * Called only after every allocation for this round has succeeded, so
 * the in-place mutation IS the commit: nothing below can fail.
 * new_base receives the new process baseline (nproc entries). */
static void model_compute(struct monitor_model *m,
                          const struct monitor_sample *s, uint32_t tick_hz,
                          struct monitor_pct *total,
                          uint32_t *ncpu_out, uint32_t *nproc_out,
                          uint32_t *nbase_out,
                          struct monitor_process_row *rows,
                          struct monitor_proc_base *new_base)
{
    uint32_t ncpu_rows = 0;
    uint32_t nproc_rows = 0;
    uint32_t nbase = 0;
    __uint128_t sum_dbusy = 0, sum_den = 0;

    memset(total, 0, sizeof(*total));

    /* ── CPU: per-core deltas + total over contributing cores ────── */
    if (s->stat_valid) {
        for (uint32_t i = 0; i < s->stat.ncpu && i < MONITOR_MAX_CPU; i++) {
            const struct monitor_cpu_sample *c = &s->stat.cpu[i];
            const struct monitor_cpu_base *old =
                cpu_base_slot(m, c->cpu_id, false);
            struct monitor_cpu_row *row = &m->cpu_rows[ncpu_rows];
            bool continuous = old && old->valid_seen && c->valid &&
                              old->epoch == c->epoch &&
                              old->tick_hz == c->tick_hz;
            struct monitor_cpu_base *slot;

            memset(row, 0, sizeof(*row));
            row->cpu_id = c->cpu_id;
            row->online = c->online != 0;
            ncpu_rows++;

            if (continuous) {
                /* compare first, subtract afterwards: a counter that
                 * went backwards is a reset, never a negative delta */
                if (c->busy_ticks >= old->busy_ticks &&
                    c->idle_ticks >= old->idle_ticks) {
                    uint64_t db = c->busy_ticks - old->busy_ticks;
                    uint64_t di = c->idle_ticks - old->idle_ticks;
                    __uint128_t den = (__uint128_t)db + di;

                    if (den != 0) {     /* zero denominator -> -- */
                        row->pct.valid = true;
                        row->pct.percent_10000 =
                            cpu_pct_10000(db, den, &row->pct.raw_10000);
                        sum_dbusy += db;
                        sum_den += den;
                    }
                }
            }

            /* rebuild/refresh this core's baseline with the NEW
             * counters whatever the outcome (spec: 重新建立基线) */
            slot = cpu_base_slot(m, c->cpu_id, true);
            if (slot) {
                slot->seen = true;
                slot->valid_seen = c->valid != 0;
                slot->cpu_id = c->cpu_id;
                slot->epoch = c->epoch;
                slot->busy_ticks = c->busy_ticks;
                slot->idle_ticks = c->idle_ticks;
                slot->tick_hz = c->tick_hz;
                slot->online = c->online != 0;
            }
        }

        if (sum_den != 0) {
            total->valid = true;
            total->percent_10000 =
                cpu_pct_10000(sum_dbusy, sum_den, &total->raw_10000);
        }
    }

    /* ── processes: (pid, birth_id) deltas ───────────────────────── */
    for (uint32_t i = 0; i < s->nprocesses; i++) {
        const struct monitor_process_sample *p = &s->processes[i];
        struct monitor_process_row *row = &rows[nproc_rows];
        const struct monitor_proc_base *old = proc_base_find(m, p->pid);

        memset(row, 0, sizeof(*row));
        row->pid = p->pid;
        row->ppid = p->ppid;
        row->birth_id = p->birth_id;
        row->last_cpu = p->last_cpu;
        row->state = p->state;
        memcpy(row->comm, p->comm, sizeof(row->comm));
        row->stale = false;
        nproc_rows++;

        /* same pid AND same birth_id with a known tick_hz and a
         * strictly advancing clock; a runtime regression or Δt ≤ 0
         * invalidates the row (spec §5.3) */
        if (old && old->birth_id == p->birth_id && tick_hz != 0 &&
            p->runtime_ticks >= old->runtime_ticks &&
            p->sampled_ns > old->sampled_ns) {
            row->cpu.valid = true;
            row->cpu.percent_10000 =
                proc_pct_10000(p->runtime_ticks - old->runtime_ticks,
                               p->sampled_ns - old->sampled_ns,
                               tick_hz, &row->cpu.raw_10000);
        }

        /* new baseline record: every record seen this round (records
         * absent from the round simply vanish, history and all) */
        new_base[nbase].pid = p->pid;
        new_base[nbase].birth_id = p->birth_id;
        new_base[nbase].runtime_ticks = p->runtime_ticks;
        new_base[nbase].sampled_ns = p->sampled_ns;
        nbase++;
    }

    proc_rows_sort(rows, nproc_rows);
    cpu_rows_sort(m->cpu_rows, ncpu_rows);

    *ncpu_out = ncpu_rows;
    *nproc_out = nproc_rows;
    *nbase_out = nbase;
}

int monitor_model_update(struct monitor_model *m,
                         const struct monitor_sample *s)
{
    uint32_t tick_hz;
    uint32_t ncpu_rows = 0, nproc_rows = 0, nbase = 0;
    struct monitor_pct total, mem_pct;
    struct monitor_process_row *new_rows = NULL;
    struct monitor_proc_base *new_base = NULL;

    if (!m || !s)
        return -EINVAL;

    /* best-known stat tick_hz: this round's when present, else the one
     * remembered from the last valid stat (v1: uniform 100Hz) */
    if (s->stat_valid && s->stat.tick_hz != 0)
        tick_hz = s->stat.tick_hz;
    else
        tick_hz = m->base_tick_hz;

    /* memory ratio is instantaneous (no delta baseline) */
    memset(&mem_pct, 0, sizeof(mem_pct));
    if (s->mem_valid && s->mem.total_bytes != 0) {
        mem_pct.valid = true;
        mem_pct.raw_10000 = div_round_cap(
            (__uint128_t)10000 * s->mem.used_bytes,
            (__uint128_t)s->mem.total_bytes, 10000);
        mem_pct.percent_10000 = (uint32_t)mem_pct.raw_10000;
    }

    /* Allocate BOTH round arrays up front; model_compute() below does
     * not allocate, so a failure preserves all committed data. Only
     * the history rings record that a sampling round was lost. */
    if (s->nprocesses > 0) {
        new_rows = malloc(s->nprocesses * sizeof(*new_rows));
        if (!new_rows)
            goto oom;
        new_base = malloc(s->nprocesses * sizeof(*new_base));
        if (!new_base) {
            free(new_rows);
            goto oom;
        }
    }

    model_compute(m, s, tick_hz, &total, &ncpu_rows, &nproc_rows,
                  &nbase, new_rows, new_base);

    /* ── commit: nothing below can fail ──────────────────────────── */
    free(m->proc_rows);
    free(m->proc_base);
    m->proc_rows = new_rows;
    m->proc_base = new_base;
    m->nproc_rows = nproc_rows;
    m->ncpu_rows = ncpu_rows;
    m->nproc_base = nbase;

    m->total_cpu = total;
    m->mem_valid = mem_pct.valid;
    m->mem_pct = mem_pct;
    m->mem_total_bytes = s->mem.total_bytes;
    m->mem_used_bytes = s->mem.used_bytes;
    m->mem_free_bytes = s->mem.free_bytes;
    if (s->stat_valid && s->stat.tick_hz != 0)
        m->base_tick_hz = s->stat.tick_hz;

    m->thermal_valid = s->thermal_valid != 0;
    if (s->thermal_valid)
        m->thermal = s->thermal;
    else
        memset(&m->thermal, 0, sizeof(m->thermal));

    /* history: one point per metric per round; a missing metric is an
     * EMPTY point, never a stale value copied forward (spec §7) */
    push_history(&m->total_history, m->total_cpu.valid,
                 m->total_cpu.percent_10000);
    push_history(&m->mem_history, m->mem_pct.valid,
                 m->mem_pct.percent_10000);

    return 0;

oom:
    // The caller still counts this round. Keep its place in the time axis
    // without presenting old percentages as fresh history samples.
    push_history(&m->total_history, false, 0);
    push_history(&m->mem_history, false, 0);
    return -ENOMEM;
}
