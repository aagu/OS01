# SMP Load Balancing Design

**Date:** 2026-07-29
**Status:** Draft

## Overview

Currently AP cores boot successfully and spin on idle tasks, but never
receive any real work — all new tasks are created with `cpu = 0` and
enqueued on the BSP's runqueue.  This design adds:

1. **At-creation CPU selection** — pick the least-loaded CPU for new tasks.
2. **Per-schedule() pull** — every `schedule()` compares its local
   `min_vruntime` against the busiest CPU; if the gap exceeds
   `EEVDF_LATENCY`, steal half of the busiest queue.
3. **Idle-steal fallback** — when the local runqueue is empty (even if the
   vruntime gap is small), unconditionally take half from the busiest
   queue.

The mechanism uses the existing **per-CPU rbtree** (EEVDF) and
`min_vruntime` as the sole load metric — no separate `load_avg` or
weight tracking.

### Non-goals

- NUMA awareness — all memory is uniform in this system.
- Per-task CPU affinity / pinning — can be added later.
- Idle-core power management (C-states, etc.).
- Gang scheduling or co-scheduling.

---

## Architecture

```
fork / spawn / kthread
  │
  └── sched_pick_cpu() ──→ choose CPU with fewest nr_running
        │
        ▼
  task_wake() ──→ enqueue on chosen CPU's runqueue
        │
        ▼
  CPU N: schedule() runs on next timer tick
        │
        ├── update_curr / dequeue / zombie reap (unchanged)
        │
        ├── sched_balance(rq)          ◀── NEW
        │     │
        │     ├── find busiest online CPU (max min_vruntime)
        │     ├── gate: (rq empty) OR (gap > EEVDF_LATENCY)
        │     ├── double-lock rq_locks (addr-ordered)
        │     ├── walk rbtree_last → rbtree_prev, take nr_running/2 tasks
        │     ├── update cpu field, insert into local rbtree
        │     └── re-check busiest CPU's min_vruntime
        │
        ├── pick_eevdf(rq)            (unchanged)
        ├── idle fallback             (unchanged)
        └── switch_to                 (unchanged)
```

**Key invariant:** a task's `vruntime` is **never modified** during
migration.  The EEVDF natural ordering will schedule migrated tasks
fairly — they get to run sooner on the idle CPU (which has a lower
`min_vruntime`), while the source CPU's `min_vruntime` drops because
its most-deadline tasks (the tail) were removed.

---

## Data structures

### `percpu_t` — one new field

```c
// kernel/include/kernel/percpu.h
typedef struct percpu {
    // ... all existing fields ...
    uint32_t nr_running;   // count of tasks on runqueue (excl. idle)
} percpu_t;
```

- Incremented in `enqueue_task()`, decremented in `dequeue_task()`.
- Read lockless by `sched_balance()` — a transient stale value is harmless
  (at worst we skip one balance round or take ±1 task).

No new fields needed in `task_t` — `cpu`, `on_rq`, and `rb_node` already
support per-CPU runqueues.

### rbtree — two new functions

```c
// libc/rbtree/rbtree.c  (new)
rbtree_node_t *rbtree_last(rbtree_root_t *root);
rbtree_node_t *rbtree_prev(rbtree_node_t *node);
```

Symmetrical to the existing `rbtree_first` / `rbtree_next`.  Both O(log n).

---

## New functions

### `sched_pick_cpu()`

```c
// Returns CPU with minimum nr_running; ties → current CPU.
// Called from spawn_user_task(), kernel_thread(), do_fork().
static uint32_t sched_pick_cpu(void)
{
    uint32_t me = cpu_id();
    uint32_t best = me;
    uint32_t min_nr = percpu_data[me].nr_running;

    for (uint32_t i = 0; i < num_cpus; i++) {
        if (!percpu_data[i].online) continue;
        if (percpu_data[i].nr_running < min_nr) {
            min_nr = percpu_data[i].nr_running;
            best = i;
        }
    }
    return best;
}
```

### `sched_balance(percpu_t *rq)`

Single entry point for both pull and idle-steal.  Called from `schedule()`
after zombie reaping, before `pick_eevdf()`.

**Pseudocode:**

```
1. Find busiest online CPU (max min_vruntime, nr_running > 0).
   Skip self.  Return if none found.

2. Gate:
   a) If local nr_running == 0  →  steal unconditionally.
   b) Else if (max_vr - rq->min_vruntime) < EEVDF_LATENCY  →  return.

3. count = max(1, src.nr_running / 2)

4. Double-lock both rq_locks (ordered by address to avoid deadlock).

5. Walk src rbtree from rbtree_last() backwards via rbtree_prev(),
   collecting up to `count` tasks.  For each:
     - rbtree_erase from src
     - t->on_rq = false
     - t->cpu = rq->cpu_id
     - src.nr_running--

6. Insert each collected task into local rbtree:
     - t->deadline = t->vruntime + EEVDF_MIN_SLICE
     - t->on_rq = true
     - rbtree_insert
     - rq->nr_running++

7. If src runqueue still non-empty, update src.min_vruntime from
   rbtree_first.

8. Unlock both.
```

**Why take from the tail (largest deadline):**
These tasks just finished their time slice and won't be scheduled again
soon on the source CPU.  Migrating them to an idle CPU naturally
balances the system without ping-pong — the source CPU keeps its
"about to run" tasks (small deadline) with hot caches.

---

## Integration points

| Location | Current | Change |
|---|---|---|
| `spawn_user_task()` | `tsk->cpu = 0` | → `sched_pick_cpu()` |
| `kernel_thread()` | `tsk->cpu = 0` | → `sched_pick_cpu()` |
| `do_fork()` child | inherits parent? | → `sched_pick_cpu()` |
| `schedule()` step 3.5 | (absent) | → `sched_balance(rq)` |
| `enqueue_task()` | — | → `rq->nr_running++` |
| `dequeue_task()` | — | → `rq->nr_running--` |

`task_wake()` **needs no changes** — it already uses `t->cpu` to
determine the target runqueue.

---

## Edge cases

- **Only one online CPU:** step 1 finds no source, returns immediately.
- **Source queue drained between lockless read and lock acquire:**
  `rbtree_last()` returns NULL, `taken == 0`, no side effects.
- **`nr_running == 1` on source:** `count = 1/2 = 0` → bumped to 1,
  stealing the only task.  Source CPU goes idle — correct.
- **`nr_running` transiently stale:** lockless read may be ±1 off.
  Worst case: pull one fewer/more task than ideal.  Balances next round.
- **min_vruntime reads:** lockless, may see stale value.  At worst the
  gap check is slightly too strict/lenient for one round.
- **Migration while task is in `task_wake` path:** `task_wake` holds
  `rq_lock` during enqueue.  `sched_balance` also acquires `rq_lock`
  (of source).  Ordered correctly — no race.

---

## Testing strategy

1. **Unit test `sched_pick_cpu()`** — mock percpu_data, verify
   least-loaded CPU chosen.
2. **Unit test `sched_balance` gate** — verify steal triggers on
   empty queue and on gap > LANTENCY, but not on small gap.
3. **Integration: `systest` on multi-core** — existing 70 tests must
   pass with `num_cpus > 1`.
4. **Smoke test: shell on AP** — verify user processes can run on
   non-BSP cores via `taskset`-like manual pinning (future).
5. **Load distribution smoke test** — spawn `num_cpus` busy-loop
   processes, verify via debug log that each lands on a different CPU
   and `schedule_count` grows on all cores.

---

## Files changed

| File | Change |
|---|---|
| `kernel/include/kernel/percpu.h` | +`uint32_t nr_running` |
| `kernel/sched/task.c` | +`sched_pick_cpu()`, +`sched_balance()`, update `enqueue_task`/`dequeue_task`, integrate call sites |
| `libc/include/rbtree.h` | +`rbtree_last`, +`rbtree_prev` declarations |
| `libc/rbtree/rbtree.c` | +`rbtree_last()`, +`rbtree_prev()` implementations |
| `test/include/kernel/task.h` | sync `nr_running` field |
| `test/include/kernel/percpu.h` | sync `nr_running` field |
