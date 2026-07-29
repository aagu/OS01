# SMP Load Balancing Design

**Date:** 2026-07-29
**Status:** Draft
**Review:** [review](file:///tmp/opencode/smp-load-balance-review.md)

## Overview

Currently AP cores boot successfully and spin on idle tasks, but never
receive any real work — all new tasks are created with `cpu = 0` and
enqueued on the BSP's runqueue.  This design adds:

1. **At-creation CPU selection** — pick the least-loaded CPU for new tasks.
2. **Per-schedule() pull** — every `schedule()` compares its local
   `nr_running` against the busiest CPU; if there's a meaningful
   imbalance, steal half of the busiest queue's tail.
3. **Idle-steal fallback** — when the local runqueue is empty,
   unconditionally take half from the busiest queue.

The mechanism uses **`nr_running` as the primary load metric** with
`min_vruntime` as a tiebreaker when multiple CPUs have the same task
count.  No separate `load_avg` or weight tracking.

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
        │     ├── find busiest online CPU (max nr_running,
        │     │     tiebreak: max min_vruntime)
        │     ├── gate: (rq empty) OR (src.nr_running > rq.nr_running)
        │     ├── count = src.nr_running / 2
        │     ├── double-lock rq_locks (addr-ordered, single IRQ save)
        │     ├── walk rbtree_last → rbtree_prev, collect count tasks
        │     ├── normalize vruntime: max(t->vruntime, rq->min_vruntime)
        │     ├── insert into local rbtree
        │     └── if src empty: src.min_vruntime = 0
        │
        ├── pick_eevdf(rq)            (unchanged)
        ├── idle fallback             (unchanged)
        └── switch_to                 (unchanged)
```

**Why take from the tail (largest deadline):**
These tasks just finished their time slice and won't be scheduled again
soon on the source CPU.  Migrating them to an idle CPU naturally
balances the system without ping-pong — the source CPU keeps its
"about to run" tasks (small deadline) with hot caches.

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
- **`pick_eevdf()` also calls `dequeue_task()`** (`schedule()` step 4,
  `task.c:382-383`).  This means `nr_running` is recomputed every
  schedule round — always consistent with the rbtree.
- Read lockless by `sched_balance()` and `sched_pick_cpu()` — a transient
  stale value is harmless (at worst we skip one balance round or take
  ±1 task).

`task_t` needs no new fields — `cpu`, `on_rq`, and `rb_node` already
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
 1. Find busiest online CPU:
      - max nr_running (primary metric)
      - tiebreak: max min_vruntime
      - skip self, skip CPUs with nr_running == 0
      - return if none found

 2. Gate:
      a) If local nr_running == 0 → proceed (idle steal).
      b) Else if src.nr_running <= local.nr_running → return.
         (no real imbalance — pull would overshoot)
      c) Else proceed.

 3. count = src.nr_running / 2
    (integer division; with (b) this is always ≥ 1)

 4. Double-lock both rq_locks, ordered by address to avoid deadlock.
    Use a single IRQ save/restore around the entire critical section
    (spin_lock_irqsave/lock pair nesting is not safe — re-enabling
    interrupts partway through schedule() would be incorrect):

      uint64_t flags = arch_local_irq_save();
      spinlock_T *lo = (uintptr_t)&src->rq_lock < (uintptr_t)&rq->rq_lock
                       ? &src->rq_lock : &rq->rq_lock;
      spinlock_T *hi = (lo == &src->rq_lock) ? &rq->rq_lock : &src->rq_lock;
      spin_lock(lo);
      spin_lock(hi);

      // ... critical section ...

      spin_unlock(hi);
      spin_unlock(lo);
      arch_local_irq_restore(flags);

 5. Walk src rbtree from rbtree_last() backwards via rbtree_prev(),
    collecting up to `count` tasks.  For each:
      - rbtree_erase from src
      - t->on_rq = false
      - t->cpu = rq->cpu_id
      - src.nr_running--

 6. Insert each collected task into local rbtree.
    Before inserting, normalize vruntime to the target CPU's timeline
    (see rationale § "Wakeup boost interaction"):
      - t->vruntime = max(t->vruntime, rq->min_vruntime)
      - t->deadline = t->vruntime + EEVDF_MIN_SLICE
      - t->on_rq = true
      - rbtree_insert
      - rq->nr_running++

 7. If src runqueue is now empty:
      src.min_vruntime = 0
    (Fresh start — next tasks enqueued here won't inherit a stale
     baseline.  Do NOT decrease min_vruntime when non-empty — see
     rationale § "min_vruntime monotonicity".)

 8. Unlock both, restore IRQs.
```

---

## Integration points

| Location | Current | Change |
|---|---|---|
| `spawn_user_task()` | `tsk->cpu = 0` | → `sched_pick_cpu()` |
| `kernel_thread()` | `tsk->cpu = 0` | → `sched_pick_cpu()` |
| `do_fork()` child | copied from parent? | → `sched_pick_cpu()` |
| `schedule()` step 3.5 | (absent) | → `sched_balance(rq)` |
| `enqueue_task()` | — | → `rq->nr_running++` |
| `dequeue_task()` | — | → `rq->nr_running--` |

`task_wake()` **needs no changes** — it already enqueues to `t->cpu`'s
runqueue.  If a task was migrated by `sched_balance`, `t->cpu` already
points to the new CPU, and the wakeup boost is computed from the correct
(target) runqueue's `min_vruntime`.

---

## Design rationale

### Why `nr_running` and not `min_vruntime` as the primary load metric

`min_vruntime` has restricted monotonic semantics in this codebase: it
is only ever increased by `pick_eevdf()` and `schedule()`.  It
correlates with cumulative work but **not** with current load.

Counterexample:

| CPU | Tasks | min_vruntime | nr_running |
|-----|-------|-------------|------------|
| 0   | 10 interactive tasks (frequent sleep → wakeup boost keeps vruntime low) | 100 | 10 |
| 1   | 1 CPU-bound task (vruntime +1/tick, no sleep) | 500 | 1 |

A `min_vruntime` gap check sees CPU 1 as "busiest" and steals its
single task.  After stealing, CPU 1 goes idle and CPU 0 has 11 tasks —
the opposite of correct behavior.

Using `nr_running` correctly identifies CPU 0 as the busiest CPU.
`min_vruntime` is retained only as a tiebreaker (when two CPUs have the
same task count, prefer the one with higher `min_vruntime` — its tasks
have run more and deserve relief sooner).

### `min_vruntime` monotonicity

After removing tail (high-vruntime) tasks, `rbtree_first` returns a
task with a **lower** vruntime than before.  If we updated
`src.min_vruntime` from `rbtree_first`, we'd decrease it, breaking the
EEVDF invariant that `min_vruntime` only increases.

Consequence: `task_wake()`'s wakeup boost ceiling depends on
`min_vruntime` (`task.c:134-137`).  A lower `min_vruntime` means a
lower boost ceiling, degrading wakeup latency on the source CPU for
interactive tasks.

**Therefore:** do not update `src.min_vruntime` after migration.  The
existing code in `pick_eevdf()` and `schedule()` will advance it
naturally on the next scheduling round.  The only exception is when the
runqueue becomes empty — set to 0 to signal "fresh start."

### Wakeup boost interaction

`task_wake()` boosts vruntime based on the woken task's home CPU's
`min_vruntime` (`task.c:134-137`):

```c
uint64_t wake_vruntime = rq->min_vruntime > EEVDF_LATENCY
    ? rq->min_vruntime - EEVDF_LATENCY : 0;
if (t->vruntime < wake_vruntime)
    t->vruntime = wake_vruntime;
```

If a task woke on an overloaded CPU (high `min_vruntime`), its
vruntime was boosted upward.  After migration to a less-loaded CPU
(lower `min_vruntime`), this boosted vruntime would place it far right
in the target rbtree — it appears to have "already run a lot" and
would be unfairly starved for up to `EEVDF_LATENCY` ticks.

**Mitigation:** on migration, normalize vruntime:

```c
t->vruntime = max(t->vruntime, rq->min_vruntime);
```

This is a cap, not a boost — the task gets at worst "fair" placement
(aligned with the target CPU's timeline) and at best a head-start if
its vruntime was already behind.

---

## Edge cases

- **Only one online CPU:** step 1 finds no source, returns immediately.
- **Source queue drained between lockless read and lock acquire:**
  `rbtree_last()` returns NULL, `taken == 0`, no side effects.
- **`nr_running` transiently stale:** lockless read may be ±1 off.
  Worst case: pull one fewer/more task than ideal.  Next
  `sched_balance()` corrects.
- **`min_vruntime` tiebreak reads:** lockless, may see stale value.
  At worst the tiebreaker is wrong for one round.
- **Overshoot on small queues:** `src.nr_running = 2, local.nr_running = 1`,
  count = 1.  Stealing the only task overshoots to (1:2).  The next
  round on the other CPU's `schedule()` will steal back.  Self-correcting.
- **AP online window (`smp.c:123-133`):** `cpu->online = 1` is set
  before `scheduler_ok = 1`.  `sched_pick_cpu` may select an AP with
  `nr_running = 0` during this window — this is correct behavior
  (least-loaded CPU wins).  `task_wake` holds `rq_lock` so the enqueue
  is safe.
- **Migration while task is in `task_wake` path:** `task_wake` holds
  `rq_lock` during enqueue.  `sched_balance` also acquires `rq_lock`
  (of source).  Ordered correctly — no race.
- **Idle CPUs hammering the busiest lock:** all idle APs enter
  `sched_balance` at 100 Hz (LAPIC timer).  With multiple idle cores,
  all attempt to steal from the same busy CPU simultaneously.  The
  double-lock serializes them — only one succeeds.  For a first
  implementation this is acceptable; future optimization could add an
  exponential backoff in the idle loop.

---

## Testing strategy

1. **Unit test `sched_pick_cpu()`** — mock percpu_data, verify
   least-loaded CPU chosen.
2. **Unit test `sched_balance` gate** — verify steal triggers on
   empty local queue, triggers on `src.nr_running > local.nr_running`,
   and skips when `src.nr_running <= local.nr_running`.
3. **Integration: `systest` on multi-core** — existing 70 tests must
   pass with `num_cpus > 1`.
4. **Load distribution smoke test** — spawn `num_cpus` busy-loop
   processes, verify via debug log that each lands on a different CPU
   and `schedule_count` grows on all cores.
5. **`rbtree_last` / `rbtree_prev` correctness** — extend existing
   rbtree unit tests in systest to cover the new functions.

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
