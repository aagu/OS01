# SMP Load Balancing Design

**Date:** 2026-07-29
**Status:** Draft
**Reviews:** [round 1](file:///tmp/opencode/smp-load-balance-review.md) · [round 2](file:///tmp/opencode/smp-load-balance-review.md)

## Overview

Currently AP cores boot successfully and spin on idle tasks, but never
receive any real work — all new tasks are created with `cpu = 0` and
enqueued on the BSP's runqueue.  This design adds:

1. **At-creation CPU selection** — pick the least-loaded CPU for new tasks.
2. **Remote reschedule IPI** — when a new task lands on a remote CPU,
   send IPI so it's discovered immediately, not up to 10 ms later.
3. **Per-schedule() pull** — every `schedule()` compares its local
   `nr_running` against the busiest CPU; if there's a meaningful
   imbalance, steal half of the busiest queue's tail.
4. **Idle-steal fallback** — when the local runqueue is empty,
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
  ├── sched_pick_cpu() ──→ choose CPU with fewest nr_running
  │     │
  │     └── tsk->cpu = chosen
  │
  ├── do_fork: set vruntime (fair_start from tsk->cpu's min_vruntime)
  │
  ├── enqueue_task(tsk, &percpu_data[tsk->cpu])
  │
  └── if tsk->cpu != cpu_id():
        ──→ ipi_send(tsk->cpu, IPI_VECTOR_RESCHED)    ◀── NEW
        │
        ▼
  CPU N: schedule() runs on next timer tick (or IPI wakeup)
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

**Window between sched_balance and pick_eevdf:** `sched_balance` releases
`rq_lock` after migration, then `pick_eevdf` re-acquires it.  This
window means another CPU's `sched_balance` could steal a task we just
pulled.  This is **expected and self-correcting** — the next schedule
round will rebalance.  The alternative (holding `rq_lock` across the
entire schedule path) would serialize all CPUs on every tick, which is
far worse.

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

### Declarations

New functions (`sched_pick_cpu`, `sched_balance`, `sched_notify_remote`)
are `static` in `kernel/sched/task.c` and need no header declarations.
If they become non-static in the future, they go in
**`kernel/include/kernel/task.h`** — this is where `schedule()`,
`task_wake()`, and `do_exit()` are already declared.  No new header
file is needed.

### `sched_pick_cpu()`

```c
// Returns CPU with minimum nr_running; ties → current CPU.
// Called from do_fork().
//
// Complexity: O(num_cpus).  Acceptable for NR_CPUS ≤ 8.
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

### `sched_notify_remote(task_t *tsk)`

```c
// If the task was placed on a remote CPU, wake it up.
// Without this, the remote CPU won't discover the task until
// its next LAPIC timer tick (up to 10 ms at 100 Hz).
//
// Called from do_fork() after enqueue_task().
static void sched_notify_remote(task_t *tsk)
{
    if ((int)tsk->cpu == (int)cpu_id())
        return;
    percpu_t *dst = &percpu_data[tsk->cpu];
    dst->need_resched = 1;
    __sync_synchronize();
    ipi_send(dst->arch_processor_id, IPI_VECTOR_RESCHED);
}
```

Setting `need_resched` is an optimization: if the remote CPU is already
running with interrupts enabled (not in `hlt`), it will see the flag on
the next timer tick or return-to-userspace path and schedule without
waiting for the IPI handler.  The IPI guarantees the CPU wakes from
`hlt` and enters `schedule()` promptly.

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
    Complexity: O(num_cpus).  Acceptable for NR_CPUS ≤ 8.

 2. Gate:
      a) If local nr_running == 0 → proceed (idle steal).
      b) Else if src.nr_running <= local.nr_running → return.
         (no real imbalance — pull would overshoot)
      c) Else proceed.

 3. count = src.nr_running / 2
    (integer division; gate (b) ensures this is always ≥ 1)

 4. Double-lock both rq_locks, ordered by address to avoid deadlock.
    Use a single IRQ save/restore around the entire critical section
    (re-enabling interrupts partway through schedule() would let
    an IRQ handler attempt sched_balance on the same CPU and deadlock):

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
| `do_fork()` — CPU selection | `tsk->cpu = 0` or copied from parent | → `tsk->cpu = sched_pick_cpu()` |
| `do_fork()` — fair_start | `percpu_data[cpu_id()].min_vruntime` | → `percpu_data[tsk->cpu].min_vruntime` |
| `do_fork()` — after enqueue | (nothing) | → `sched_notify_remote(tsk)` |
| `schedule()` step 3.5 | (absent) | → `sched_balance(rq)` |
| `enqueue_task()` | — | → `rq->nr_running++` |
| `dequeue_task()` | — | → `rq->nr_running--` |

### `do_fork()` fair_start: use target CPU's min_vruntime

Currently `do_fork()` (task.c:1299-1301):

```c
uint64_t fair_start = percpu_data[cpu_id()].min_vruntime;
tsk->vruntime = current->vruntime < fair_start ? current->vruntime : fair_start;
```

This uses the **current** CPU's `min_vruntime`.  After this change,
`tsk->cpu` is already set by `sched_pick_cpu()`, so use the target:

```c
uint64_t fair_start = percpu_data[tsk->cpu].min_vruntime;
tsk->vruntime = current->vruntime < fair_start ? current->vruntime : fair_start;
```

Without this, a child placed on a freshly-unloaded CPU (whose
`min_vruntime` was just reset to 0 by `sched_balance` step 7) would
inherit the parent CPU's higher vruntime and be placed unfairly far
right in the target rbtree.  With the target CPU's `min_vruntime`,
the child gets correct initial placement.

### `task_wake()` — no changes needed, reasoning

`task_wake()` (task.c:114-145) reads `rq->min_vruntime` locklessly for
the wakeup boost (task.c:134).  After `sched_balance` migration,
`t->cpu` already points to the new CPU, so `task_wake` reads the
**target** CPU's `min_vruntime` — correct, because the boost should
be relative to the CPU the task actually runs on.

The last two lines of `task_wake` (task.c:143-144):
```c
if ((int)t->cpu != (int)cpu_id())
    rq->need_resched = 1;
```
handle the case where `task_wake` is called from a different CPU than
the task's home.  After migration, this is still correct — if BSP
wakes a task on AP1, AP1's `need_resched` is set.  No IPI is sent
here because `task_wake` assumes the caller (e.g., interrupt handler)
will eventually return and the flag will be noticed on the next tick.
For the `do_fork()` case, `sched_notify_remote()` explicitly sends
the IPI.

### Cross-CPU signal delivery

`task_send_signal()` (task.c:1436) sets the signal bit and calls
`task_wake()` if the target is INTERRUPTIBLE.  `task_wake` enqueues
to `percpu_data[t->cpu]`.  If `sched_balance` migrated the task,
`t->cpu` points to the new CPU — signal delivery is correct without
changes.

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

### `min_vruntime = 0` interaction with do_fork

When a runqueue is emptied by `sched_balance`, `min_vruntime` is reset
to 0.  `do_fork()` uses `min_vruntime` as `fair_start` for child
vruntime.  With `min_vruntime = 0`, `fair_start = 0`, and the child
receives `current->vruntime` (or 0, whichever is smaller).  Since
`current->vruntime` is always ≥ 0, the child effectively inherits
`current->vruntime` — reasonable for the first task on a freshly
unloaded CPU.

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
  (least-loaded CPU wins).  `enqueue_task` in `do_fork` holds
  `rq_lock` so the enqueue is safe.  `sched_notify_remote` sends IPI
  to the AP — the AP's IDT is already loaded (`ap_entry` line 71)
  and LAPIC is initialized, so it handles the IPI correctly.
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

## Known limitations

1. **O(num_cpus) scans:** `sched_pick_cpu()` and `sched_balance()` both
   iterate all online CPUs.  Acceptable for NR_CPUS ≤ 8.  A future
   optimization could maintain a global `max_nr_running_cpu` hint.

2. **Test mock gap:** `test/include/kernel/percpu.h` diverges from the
   real `percpu_t` (uses `list_t run_queue` instead of `rbtree_root_t`,
   lacks `min_vruntime`/`rq_lock`/`watchdog_counter`).  Unit testing
   `sched_pick_cpu` and `sched_balance` gate logic requires updating
   the test mock.  This is scoped into the implementation.

---

## Testing strategy

1. **systest: `rbtree_last` / `rbtree_prev`** — extend existing rbtree
   unit tests (`user/systest.c`, lines 936-1004) with inorder-predecessor
   traversal.
2. **systest: multi-core** — existing 70 tests must pass with
   `num_cpus > 1`.  (Requires QEMU `-smp 2` or higher.)
3. **systest: load distribution** — spawn `num_cpus` busy-loop
   processes, verify `schedule_count` grows on all cores.
4. **Unit test `sched_pick_cpu()`** — requires test mock update (see
   known limitation #2).  Defer to a follow-up if mock sync is too
   extensive.
5. **Unit test `sched_balance` gate** — same mock constraint as above.

---

## Files changed

| File | Change |
|---|---|
| `kernel/include/kernel/percpu.h` | +`uint32_t nr_running` |
| `kernel/sched/task.c` | +`sched_pick_cpu()`, +`sched_balance()`, +`sched_notify_remote()`, update `enqueue_task`/`dequeue_task`, update `do_fork()` (CPU selection + fair_start + IPI) |
| `libc/include/rbtree.h` | +`rbtree_last`, +`rbtree_prev` declarations |
| `libc/rbtree/rbtree.c` | +`rbtree_last()`, +`rbtree_prev()` implementations |
| `test/include/kernel/task.h` | sync `nr_running` field |
| `test/include/kernel/percpu.h` | sync `nr_running` field |
