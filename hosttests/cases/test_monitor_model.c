/*
 * test_monitor_model.c — monitor plan Task 7: delta model and bounded
 * history (spec §2.3, §5.3, §7;
 * docs/superpowers/specs/2026-10-09-monitor-design.md).
 *
 * Drives the PRODUCTION user/monitor/model.c (host-compiled) against
 * hand-built monitor_sample rounds.  The production TU's malloc/free
 * are renamed onto mon_model_* shims (see MONITOR_MODEL_CFLAGS in the
 * hosttests Makefile), so the fixture can inject an allocation failure
 * and prove an OOM round leaves the existing baseline intact.
 *
 * Asserted (spec §2.3 / §5.3 / plan Task 7 Step 1):
 *   - dual-core busy deltas 100/0 with idle 0/100 -> total 5000 and the
 *     busy single-thread process 10000 (100 × ΔR × 1e9 / (hz × Δt));
 *   - all cores busy -> 10000; per-core rounding (1/3 vs 2/3);
 *   - first round, zero denominator, counter regression, epoch change
 *     and differing tick_hz all -> invalid, with the new counters kept
 *     as the rebuilt baseline (a PID-same-value-new-birth row is
 *     invalid too);
 *   - migration does not reset runtime (delta continues across a
 *     last_cpu change); exec with the same birth_id continues the
 *     delta under a new comm;
 *   - rows for vanished PIDs are removed (and their history with them:
 *     the PID returning later is treated as first-seen again);
 *   - the 60-slot ring keeps exactly 60 points (oldest dropped) and an
 *     invalid round pushes an EMPTY point (valid bit clear), never a
 *     stale value copied forward;
 *   - counters near UINT64_MAX delta without overflow (compare first,
 *     then subtract; 128-bit scaled intermediates);
 *   - display ordering: processes by CPU% desc with PID as the stable
 *     ascending tie-break, invalid rows last; CPUs by CPU% desc;
 *   - single-task display clamps at 10000 while the diagnostic
 *     unclamped 128-bit value keeps the raw ratio;
 *   - OOM (shim-injected) returns -ENOMEM, keeps the previous display
 *     rows AND the previous baseline (the next good round deltas from
 *     the pre-OOM sample); destroy frees everything exactly once.
 */

#include "test_framework.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <monitor/collect.h>
#include <monitor/model.h>

/* ── Production seam: the model TU is compiled with malloc/free
 * renamed onto these shims. ── */
void *mon_model_malloc(size_t size);
void  mon_model_free(void *ptr);

static long model_malloc_balance;
static int  model_malloc_fail_after;    /* <0 never fail; 0 fail now; n after n */

void *mon_model_malloc(size_t size)
{
    if (model_malloc_fail_after == 0)
        return NULL;
    if (model_malloc_fail_after > 0)
        model_malloc_fail_after--;
    void *p = malloc(size ? size : 1);
    if (p)
        model_malloc_balance++;
    return p;
}

void mon_model_free(void *ptr)
{
    if (ptr) {
        free(ptr);
        model_malloc_balance--;
    }
}

/* ── Sample builders ─────────────────────────────────────────────── */

static struct monitor_sample mk_sample(void)
{
    struct monitor_sample s;
    memset(&s, 0, sizeof(s));
    s.stat_valid = 1;
    s.stat.tick_hz = 100;
    return s;
}

static void set_cpu(struct monitor_sample *s, uint32_t id, uint64_t epoch,
                    uint64_t busy, uint64_t idle)
{
    struct monitor_cpu_sample *c = &s->stat.cpu[s->stat.ncpu++];
    memset(c, 0, sizeof(*c));
    c->cpu_id = id;
    c->online = 1;
    c->valid = 1;
    c->epoch = epoch;
    c->busy_ticks = busy;
    c->idle_ticks = idle;
    c->tick_hz = s->stat.tick_hz;
}

static void set_proc(struct monitor_sample *s, uint64_t pid, uint64_t birth,
                     uint64_t ns, uint64_t rt, const char *comm)
{
    struct monitor_process_sample *p;
    struct monitor_process_sample *arr = realloc(
        s->processes, (s->nprocesses + 1) * sizeof(*arr));
    assert_not_null(arr);
    s->processes = arr;
    p = &s->processes[s->nprocesses++];
    memset(p, 0, sizeof(*p));
    p->pid = pid;
    p->ppid = 1;
    p->birth_id = birth;
    p->sampled_ns = ns;
    p->runtime_ticks = rt;
    p->last_cpu = 0;
    p->state = 'R';
    snprintf(p->comm, sizeof(p->comm), "%s", comm);
}

static void free_sample(struct monitor_sample *s)
{
    free(s->processes);
    s->processes = NULL;
    s->nprocesses = 0;
}

/* find a process row by pid (NULL when absent) */
static const struct monitor_process_row *find_proc(
    const struct monitor_model *m, uint64_t pid)
{
    for (uint32_t i = 0; i < m->nproc_rows; i++)
        if (m->proc_rows[i].pid == pid)
            return &m->proc_rows[i];
    return NULL;
}

static const struct monitor_cpu_row *find_cpu(const struct monitor_model *m,
                                              uint32_t cpu_id)
{
    for (uint32_t i = 0; i < m->ncpu_rows; i++)
        if (m->cpu_rows[i].cpu_id == cpu_id)
            return &m->cpu_rows[i];
    return NULL;
}

/* ── 1. dual-core 5000 total / 10000 process; all-busy 10000 ─────── */

static TEST_FUNC(test_dual_core_deltas)
{
    struct monitor_model m = { 0 };
    struct monitor_sample a = mk_sample();
    struct monitor_sample b = mk_sample();
    struct monitor_sample c = mk_sample();
    struct monitor_sample mem = mk_sample();

    model_malloc_balance = 0;
    model_malloc_fail_after = -1;
    monitor_model_init(&m);
    assert_eq(model_malloc_balance, 0);

    /* round A: baseline round (first -> everything invalid) */
    set_cpu(&a, 0, 7, 1000, 1000);
    set_cpu(&a, 1, 7, 500, 1500);
    set_proc(&a, 100, 7, 10000000000ULL, 1000, "spinner");
    assert_eq(monitor_model_update(&m, &a), 0);
    assert_false(m.total_cpu.valid);
    assert_false(find_proc(&m, 100)->cpu.valid);
    assert_false(find_cpu(&m, 0)->pct.valid);

    /* round B: cpu0 +100 busy / +0 idle, cpu1 +0 busy / +100 idle;
     * process +100 runtime ticks over exactly 1s at 100Hz */
    set_cpu(&b, 0, 7, 1100, 1000);
    set_cpu(&b, 1, 7, 500, 1600);
    set_proc(&b, 100, 7, 11000000000ULL, 1100, "spinner");
    assert_eq(monitor_model_update(&m, &b), 0);
    assert_true(m.total_cpu.valid);
    assert_eq(m.total_cpu.percent_10000, 5000);     /* 50.00% */
    assert_eq(find_cpu(&m, 0)->pct.percent_10000, 10000);
    assert_eq(find_cpu(&m, 1)->pct.percent_10000, 0);
    assert_true(find_proc(&m, 100)->cpu.valid);
    assert_eq(find_proc(&m, 100)->cpu.percent_10000, 10000);

    /* all-busy round: total 10000 */
    set_cpu(&c, 0, 7, 1200, 1000);
    set_cpu(&c, 1, 7, 600, 1600);
    assert_eq(monitor_model_update(&m, &c), 0);
    assert_true(m.total_cpu.valid);
    assert_eq(m.total_cpu.percent_10000, 10000);
    assert_eq(find_cpu(&m, 0)->pct.percent_10000, 10000);
    assert_eq(find_cpu(&m, 1)->pct.percent_10000, 10000);

    /* memory percentage is an instantaneous ratio of the round */
    mem.mem_valid = 1;
    mem.mem.total_bytes = 4 * 1024 * 1024;
    mem.mem.used_bytes = 1024 * 1024;
    mem.mem.free_bytes = 3 * 1024 * 1024;
    assert_eq(monitor_model_update(&m, &mem), 0);
    assert_true(m.mem_valid);
    assert_eq(m.mem_pct.percent_10000, 2500);       /* 25.00% */

    monitor_model_destroy(&m);
    assert_eq(model_malloc_balance, 0);
    free_sample(&a);
    free_sample(&b);
    free_sample(&c);
    free_sample(&mem);
}

/* ── 2. rounding to one decimal (10000ths of percent) ────────────── */

static TEST_FUNC(test_rounding)
{
    struct monitor_model m = { 0 };
    struct monitor_sample a = mk_sample();
    struct monitor_sample b = mk_sample();

    model_malloc_fail_after = -1;
    monitor_model_init(&m);

    set_cpu(&a, 0, 1, 0, 0);
    set_cpu(&a, 1, 1, 0, 0);
    assert_eq(monitor_model_update(&m, &a), 0);

    /* 1/3 busy -> 3333.33.. -> 3333; 2/3 busy -> 6666.67 -> 6667;
     * total 3 busy of 6 ticks -> exactly 5000 */
    set_cpu(&b, 0, 1, 1, 2);
    set_cpu(&b, 1, 1, 2, 1);
    assert_eq(monitor_model_update(&m, &b), 0);
    assert_eq(find_cpu(&m, 0)->pct.percent_10000, 3333);
    assert_eq(find_cpu(&m, 1)->pct.percent_10000, 6667);
    assert_eq(m.total_cpu.percent_10000, 5000);

    monitor_model_destroy(&m);
    free_sample(&a);
    free_sample(&b);
}

/* ── 3. invalid cases: zero denominator / regression / epoch ─────── */

static TEST_FUNC(test_invalid_cases_rebuild_baseline)
{
    struct monitor_model m = { 0 };
    struct monitor_sample a = mk_sample();
    struct monitor_sample b = mk_sample();
    struct monitor_sample c = mk_sample();
    struct monitor_sample d = mk_sample();

    model_malloc_fail_after = -1;
    monitor_model_init(&m);

    set_cpu(&a, 0, 3, 100, 100);
    set_proc(&a, 5, 9, 20000000000ULL, 50, "idleish");
    assert_eq(monitor_model_update(&m, &a), 0);

    /* zero denominator: identical counters, time advanced -> the core
     * is invalid and the new counters become the baseline.  (The
     * process also advances with zero runtime: ΔR = 0 over a positive
     * Δt is a VALID 0% — only Δt ≤ 0 / regression invalidate.) */
    set_cpu(&b, 0, 3, 100, 100);
    set_proc(&b, 5, 9, 21000000000ULL, 50, "idleish");
    assert_eq(monitor_model_update(&m, &b), 0);
    assert_false(m.total_cpu.valid);
    assert_false(find_cpu(&m, 0)->pct.valid);
    assert_true(find_proc(&m, 5)->cpu.valid);
    assert_eq(find_proc(&m, 5)->cpu.percent_10000, 0);

    /* regression: busy decreases -> invalid, new baseline kept (the
     * next round deltas from the REGRESSED counters, not the old) */
    set_cpu(&c, 0, 3, 90, 100);
    assert_eq(monitor_model_update(&m, &c), 0);
    assert_false(find_cpu(&m, 0)->pct.valid);
    set_cpu(&d, 0, 3, 100, 100);        /* +10 busy vs regressed 90 */
    assert_eq(monitor_model_update(&m, &d), 0);
    assert_true(find_cpu(&m, 0)->pct.valid);
    assert_eq(find_cpu(&m, 0)->pct.percent_10000, 10000);

    monitor_model_destroy(&m);
    free_sample(&a);
    free_sample(&b);
    free_sample(&c);
    free_sample(&d);
}

static TEST_FUNC(test_epoch_change_and_tick_hz)
{
    struct monitor_model m = { 0 };
    struct monitor_sample a = mk_sample();
    struct monitor_sample b = mk_sample();
    struct monitor_sample c = mk_sample();
    struct monitor_sample d = mk_sample();

    model_malloc_fail_after = -1;

    /* epoch change: same counters but a new epoch is a new lifecycle —
     * invalid and its accumulated history must not enter the delta */
    monitor_model_init(&m);
    set_cpu(&a, 0, 3, 100, 100);
    set_cpu(&a, 1, 3, 100, 100);
    assert_eq(monitor_model_update(&m, &a), 0);
    set_cpu(&b, 0, 4, 110, 100);        /* epoch 3 -> 4 */
    set_cpu(&b, 1, 3, 110, 100);
    assert_eq(monitor_model_update(&m, &b), 0);
    assert_false(find_cpu(&m, 0)->pct.valid);
    assert_true(find_cpu(&m, 1)->pct.valid);      /* unaffected core */
    /* total comes from continuously-valid cores only (core 1); the
     * re-armed core's full counters never enter the delta */
    assert_true(m.total_cpu.valid);
    assert_eq(m.total_cpu.percent_10000, 10000);
    monitor_model_destroy(&m);

    /* differing tick_hz on a core: rejected (v1 is uniform 100Hz) */
    monitor_model_init(&m);
    set_cpu(&c, 0, 3, 100, 100);
    assert_eq(monitor_model_update(&m, &c), 0);
    set_cpu(&d, 0, 3, 110, 100);
    d.stat.cpu[0].tick_hz = 250;
    assert_eq(monitor_model_update(&m, &d), 0);
    assert_false(find_cpu(&m, 0)->pct.valid);
    monitor_model_destroy(&m);

    free_sample(&a);
    free_sample(&b);
    free_sample(&c);
    free_sample(&d);
}

/* ── 4. process history: migration / exec / new birth / cleanup ──── */

static TEST_FUNC(test_process_history)
{
    struct monitor_model m = { 0 };
    struct monitor_sample a = mk_sample();
    struct monitor_sample b = mk_sample();
    struct monitor_sample c = mk_sample();
    struct monitor_sample d = mk_sample();
    struct monitor_sample e = mk_sample();
    struct monitor_sample f = mk_sample();

    model_malloc_fail_after = -1;
    monitor_model_init(&m);

    /* migration keeps runtime: last_cpu changes but the delta must be
     * computed (spec §5.3: 迁移不会清零 runtime) */
    set_proc(&a, 30, 77, 1000000000ULL, 200, "mig");
    assert_eq(monitor_model_update(&m, &a), 0);
    set_proc(&b, 30, 77, 2000000000ULL, 300, "mig");
    b.processes[0].last_cpu = 1;                    /* migrated */
    assert_eq(monitor_model_update(&m, &b), 0);
    assert_true(find_proc(&m, 30)->cpu.valid);
    assert_eq(find_proc(&m, 30)->cpu.percent_10000, 10000);
    assert_eq(find_proc(&m, 30)->last_cpu, 1);

    /* exec with the SAME birth_id continues the delta under a new comm */
    set_proc(&c, 30, 77, 3000000000ULL, 400, "execed");
    assert_eq(monitor_model_update(&m, &c), 0);
    assert_true(find_proc(&m, 30)->cpu.valid);
    assert_str_eq(find_proc(&m, 30)->comm, "execed");

    /* PID-same-value new birth: first round of a different task -> the
     * row is kept but its CPU% is invalid */
    set_proc(&d, 30, 78, 4000000000ULL, 99999, "recycled");
    assert_eq(monitor_model_update(&m, &d), 0);
    const struct monitor_process_row *new_birth_row = find_proc(&m, 30);
    assert_not_null(new_birth_row);
    assert(new_birth_row->cpu.valid == false);      /* key assertion */
    assert_eq(new_birth_row->birth_id, 78);

    /* vanished PIDs are removed: no stale row survives the round */
    assert_eq(monitor_model_update(&m, &e), 0);     /* no processes at all */
    assert_eq(m.nproc_rows, 0);
    /* ...and its history is gone: the PID returning is first-seen */
    set_proc(&f, 30, 78, 6000000000ULL, 100000, "back");
    assert_eq(monitor_model_update(&m, &f), 0);
    assert_false(find_proc(&m, 30)->cpu.valid);

    monitor_model_destroy(&m);
    free_sample(&a);
    free_sample(&b);
    free_sample(&c);
    free_sample(&d);
    free_sample(&e);
    free_sample(&f);
}

/* ── 5. process clamp at 10000, diagnostic raw kept unclamped ────── */

static TEST_FUNC(test_process_clamp_and_raw)
{
    struct monitor_model m = { 0 };
    struct monitor_sample a = mk_sample();
    struct monitor_sample b = mk_sample();

    model_malloc_fail_after = -1;
    monitor_model_init(&m);
    set_proc(&a, 9, 1, 0, 0, "hog");
    assert_eq(monitor_model_update(&m, &a), 0);

    /* 200 ticks over 1s at 100Hz = 200%: display clamps to 10000, the
     * diagnostic 128-bit raw keeps 20000 */
    set_proc(&b, 9, 1, 1000000000ULL, 200, "hog");
    assert_eq(monitor_model_update(&m, &b), 0);
    assert_true(find_proc(&m, 9)->cpu.valid);
    assert_eq(find_proc(&m, 9)->cpu.percent_10000, 10000);
    assert_true(find_proc(&m, 9)->cpu.raw_10000 == 20000);

    monitor_model_destroy(&m);
    free_sample(&a);
    free_sample(&b);
}

/* ── 6. process sorting: pct desc, PID ascending tie-break, invalid
 *       rows last; CPU rows pct desc ─────────────────────────────── */

static TEST_FUNC(test_sorting)
{
    struct monitor_model m = { 0 };
    struct monitor_sample a = mk_sample();
    struct monitor_sample b = mk_sample();

    model_malloc_fail_after = -1;
    monitor_model_init(&m);

    set_cpu(&a, 0, 1, 0, 1000);
    set_cpu(&a, 1, 1, 0, 1000);
    set_proc(&a, 40, 1, 0, 0, "p40");
    set_proc(&a, 20, 1, 0, 0, "p20");
    set_proc(&a, 60, 1, 0, 0, "p60");
    set_proc(&a, 10, 1, 0, 0, "p10");   /* stays invalid next round */
    assert_eq(monitor_model_update(&m, &a), 0);

    /* p40 + p60 each burn one core half of the round (50% each), p20
     * a quarter of one (25%); p10 keeps sleeping (valid 0%); new p50
     * is first-seen -> its row is kept but invalid */
    set_cpu(&b, 0, 1, 100, 1100);
    set_cpu(&b, 1, 1, 100, 1100);
    set_proc(&b, 40, 1, 1000000000ULL, 50, "p40");
    set_proc(&b, 20, 1, 1000000000ULL, 25, "p20");
    set_proc(&b, 60, 1, 1000000000ULL, 50, "p60");
    set_proc(&b, 10, 1, 1000000000ULL, 0, "p10");
    set_proc(&b, 50, 1, 1000000000ULL, 0, "p50");
    assert_eq(monitor_model_update(&m, &b), 0);

    assert_eq(m.nproc_rows, 5);
    assert_eq(m.proc_rows[0].pid, 40);  /* 50% */
    assert_eq(m.proc_rows[1].pid, 60);  /* 50%, higher PID second */
    assert_eq(m.proc_rows[2].pid, 20);  /* 25% */
    assert_eq(m.proc_rows[3].pid, 10);  /* 0%, still a valid row */
    assert_true(m.proc_rows[3].cpu.valid);
    assert_eq(m.proc_rows[3].cpu.percent_10000, 0);
    assert_false(m.proc_rows[4].cpu.valid);         /* invalid rows last */
    assert_eq(m.proc_rows[4].pid, 50);

    /* CPUs: equal 50% values ordered by cpu_id ascending */
    assert_eq(m.ncpu_rows, 2);
    assert_eq(m.cpu_rows[0].cpu_id, 0);
    assert_eq(m.cpu_rows[1].cpu_id, 1);
    assert_eq(m.cpu_rows[0].pct.percent_10000, 5000);

    monitor_model_destroy(&m);
    free_sample(&a);
    free_sample(&b);
}

/* ── 7. counters near UINT64_MAX must not overflow ───────────────── */

static TEST_FUNC(test_near_uint64_max)
{
    struct monitor_model m = { 0 };
    struct monitor_sample a = mk_sample();
    struct monitor_sample b = mk_sample();

    model_malloc_fail_after = -1;
    monitor_model_init(&m);

    set_cpu(&a, 0, 1, UINT64_MAX - 20, UINT64_MAX - 20);
    assert_eq(monitor_model_update(&m, &a), 0);

    /* +10 busy / +10 idle past the very top of the counter */
    set_cpu(&b, 0, 1, UINT64_MAX - 10, UINT64_MAX - 10);
    assert_eq(monitor_model_update(&m, &b), 0);
    assert_true(m.total_cpu.valid);
    assert_eq(m.total_cpu.percent_10000, 5000);
    assert_eq(find_cpu(&m, 0)->pct.percent_10000, 5000);
    monitor_model_destroy(&m);

    /* a fully busy core at the top of the counter range stays exact */
    monitor_model_init(&m);
    a.stat.ncpu = 0;                    /* fresh round, counters reused */
    b.stat.ncpu = 0;
    set_cpu(&a, 0, 1, UINT64_MAX - 2, 0);
    assert_eq(monitor_model_update(&m, &a), 0);
    set_cpu(&b, 0, 1, UINT64_MAX, 0);
    assert_eq(monitor_model_update(&m, &b), 0);
    assert_eq(find_cpu(&m, 0)->pct.percent_10000, 10000);
    monitor_model_destroy(&m);

    /* process runtime near the top: +100 ticks over 1s at 100Hz */
    monitor_model_init(&m);
    set_proc(&a, 7, 7, 0, UINT64_MAX - 100, "old");
    assert_eq(monitor_model_update(&m, &a), 0);
    set_proc(&b, 7, 7, 1000000000ULL, UINT64_MAX, "old");
    assert_eq(monitor_model_update(&m, &b), 0);
    assert_true(find_proc(&m, 7)->cpu.valid);
    assert_eq(find_proc(&m, 7)->cpu.percent_10000, 10000);
    monitor_model_destroy(&m);

    free_sample(&a);
    free_sample(&b);
}

/* ── 8. 60-slot ring: coverage, wraparound, invalid empty points ─── */

static TEST_FUNC(test_history_ring)
{
    struct monitor_model m = { 0 };
    struct monitor_sample a = mk_sample();
    struct monitor_sample bad = mk_sample();
    uint64_t busy, idle;
    int i;

    model_malloc_fail_after = -1;
    monitor_model_init(&m);

    /* alternating 100% / 0% total rounds (counters stay monotonic)
     * after one invalid baseline: 62 pushes in total, so the earliest
     * points must have been dropped by the 60-slot ring */
    set_cpu(&a, 0, 1, 0, 0);
    assert_eq(monitor_model_update(&m, &a), 0);     /* point 1: invalid */
    busy = idle = 0;
    for (i = 0; i < 61; i++) {
        struct monitor_sample s = mk_sample();
        if (i % 2 == 0)
            busy += 100;                    /* +100 busy -> 100% */
        else
            idle += 100;                    /* +100 idle -> 0% */
        set_cpu(&s, 0, 1, busy, idle);
        set_cpu(&s, 1, 1, 0, 200);          /* static core: never contributes */
        assert_eq(monitor_model_update(&m, &s), 0);
        free_sample(&s);
    }
    assert_eq(m.total_history.count, MONITOR_MODEL_HISTORY);

    /* newest = 100% (even i=60), alternating back through the ring;
     * the ring holds exactly 60 points, older ones dropped */
    assert_true(monitor_history_get(&m.total_history, 0)->valid);
    assert_eq(monitor_history_get(&m.total_history, 0)->percent_10000, 10000);
    assert_eq(monitor_history_get(&m.total_history, 1)->percent_10000, 0);
    assert_eq(monitor_history_get(&m.total_history, 2)->percent_10000, 10000);

    /* an invalid round pushes an EMPTY point: never a stale value */
    bad.stat_valid = 0;
    assert_eq(monitor_model_update(&m, &bad), 0);
    assert_eq(m.total_history.count, MONITOR_MODEL_HISTORY);
    assert_false(monitor_history_get(&m.total_history, 0)->valid);
    assert_eq(monitor_history_get(&m.total_history, 1)->percent_10000,
              10000);                   /* the last good point survives */
    assert_false(m.total_cpu.valid);        /* no old value copied as current */
    assert_eq(m.ncpu_rows, 0);              /* no stale CPU rows either */

    /* memory history mirrors the same contract */
    assert_eq(m.mem_history.count, MONITOR_MODEL_HISTORY);

    monitor_model_destroy(&m);
    free_sample(&a);
    free_sample(&bad);
}

/* ── 9. OOM must not corrupt the display rows nor the baseline ───── */

static TEST_FUNC(test_oom_preserves_baseline)
{
    struct monitor_model m = { 0 };
    struct monitor_sample a = mk_sample();
    struct monitor_sample b = mk_sample();
    struct monitor_sample c = mk_sample();

    model_malloc_fail_after = -1;
    monitor_model_init(&m);

    set_cpu(&a, 0, 1, 0, 1000);
    set_proc(&a, 5, 5, 0, 0, "p5");
    assert_eq(monitor_model_update(&m, &a), 0);

    /* fail the round's allocation: -ENOMEM, previous display rows and
     * baseline both survive untouched (no half-committed round) */
    model_malloc_fail_after = 0;
    set_cpu(&b, 0, 1, 100, 1000);
    set_proc(&b, 5, 5, 1000000000ULL, 100, "p5");
    assert_eq(monitor_model_update(&m, &b), -ENOMEM);
    assert_false(m.total_cpu.valid);
    assert_false(find_proc(&m, 5)->cpu.valid);
    model_malloc_fail_after = -1;

    /* the next good round deltas from the PRE-OOM baseline: the failed
     * round's sample never entered the history (Δt = 2s but ΔR = 0) */
    set_cpu(&c, 0, 1, 100, 1000);
    set_proc(&c, 5, 5, 2000000000ULL, 100, "p5");
    assert_eq(monitor_model_update(&m, &c), 0);
    /* ΔR=100 over Δt=2s from the round-A baseline -> exactly 50%; a
     * committed round B would have given ΔR=0/Δt=1s instead */
    assert_true(find_proc(&m, 5)->cpu.valid);
    assert_eq(find_proc(&m, 5)->cpu.percent_10000, 5000);
    monitor_model_destroy(&m);

    /* again, but now the surviving baseline is proven by a valid delta:
     * fail round B's alloc, then round C deltas from round A */
    monitor_model_init(&m);
    a.stat.ncpu = 0;                    /* fresh rounds, counters reused */
    a.nprocesses = 0;
    b.stat.ncpu = 0;
    b.nprocesses = 0;
    c.stat.ncpu = 0;
    c.nprocesses = 0;
    set_cpu(&a, 0, 1, 0, 1000);
    assert_eq(monitor_model_update(&m, &a), 0);
    model_malloc_fail_after = 0;
    set_cpu(&b, 0, 1, 50, 1000);
    set_proc(&b, 6, 6, 0, 0, "p6");     /* a process forces the alloc */
    assert_eq(monitor_model_update(&m, &b), -ENOMEM);
    model_malloc_fail_after = -1;
    set_cpu(&c, 0, 1, 50, 1000);
    assert_eq(monitor_model_update(&m, &c), 0);
    /* Δ vs the ROUND-A baseline = +50 busy -> 100%; a committed round
     * B would have given Δ = 0 -> zero denominator -> invalid */
    assert_true(find_cpu(&m, 0)->pct.valid);
    assert_eq(find_cpu(&m, 0)->pct.percent_10000, 10000);
    c.stat.ncpu = 0;
    set_cpu(&c, 0, 1, 50, 1100);                /* +100 idle vs round C */
    assert_eq(monitor_model_update(&m, &c), 0);
    assert_eq(find_cpu(&m, 0)->pct.percent_10000, 0);
    monitor_model_destroy(&m);
    assert_eq(model_malloc_balance, 0);

    free_sample(&a);
    free_sample(&b);
    free_sample(&c);
}

/* ── 10. API contract: NULL safety, invalid stat round, destroy ──── */

static TEST_FUNC(test_api_contract)
{
    struct monitor_model m = { 0 };
    struct monitor_sample s = mk_sample();

    model_malloc_fail_after = -1;
    model_malloc_balance = 0;

    monitor_model_init(&m);
    assert_eq(m.nproc_rows, 0);
    assert_eq(m.total_history.count, 0);
    assert_null(m.proc_rows);

    assert_eq(monitor_model_update(NULL, &s), -EINVAL);
    assert_eq(monitor_model_update(&m, NULL), -EINVAL);

    /* a fully-invalid round updates nothing but pushes empty points */
    s.stat_valid = 0;
    s.mem_valid = 0;
    assert_eq(monitor_model_update(&m, &s), 0);
    assert_false(m.total_cpu.valid);
    assert_false(m.mem_valid);
    assert_eq(m.ncpu_rows, 0);
    assert_eq(m.nproc_rows, 0);
    assert_eq(m.total_history.count, 1);
    assert_false(m.total_history.slot[0].valid);
    assert_eq(m.mem_history.count, 1);

    /* re-init on a live model must not leak the owned arrays */
    set_proc(&s, 3, 3, 0, 0, "x");
    assert_eq(monitor_model_update(&m, &s), 0);
    assert_eq(m.nproc_rows, 1);
    assert_not_null(m.proc_rows);
    monitor_model_init(&m);
    assert_eq(model_malloc_balance, 0);
    assert_null(m.proc_rows);

    monitor_model_destroy(&m);
    assert_eq(model_malloc_balance, 0);
    /* destroy is idempotent */
    monitor_model_destroy(&m);
    assert_eq(model_malloc_balance, 0);

    free_sample(&s);
}

TEST_LIST_BEGIN
    TEST_ENTRY(test_dual_core_deltas),
    TEST_ENTRY(test_rounding),
    TEST_ENTRY(test_invalid_cases_rebuild_baseline),
    TEST_ENTRY(test_epoch_change_and_tick_hz),
    TEST_ENTRY(test_process_history),
    TEST_ENTRY(test_process_clamp_and_raw),
    TEST_ENTRY(test_sorting),
    TEST_ENTRY(test_near_uint64_max),
    TEST_ENTRY(test_history_ring),
    TEST_ENTRY(test_oom_preserves_baseline),
    TEST_ENTRY(test_api_contract),
TEST_LIST_END

int main(void)
{
    RUN_ALL_TESTS();
    return __test_stats.failed > 0 ? 1 : 0;
}
