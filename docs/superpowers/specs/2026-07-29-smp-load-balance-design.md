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

### Prerequisite

- **`fork_mm_copy` must use `tlb_shootdown()` instead of local `flush_tlb()`.**
  Currently `fork_mm_copy()` (`task.c:1236`) performs `flush_tlb()` on the
  local CPU only.  With load balancing, the parent process may be running
  on a different CPU after fork, and that CPU's TLB still caches writable
  mappings to COW pages — causing silent data corruption if both parent
  and child write before the TLB is evicted.  This is an *existing* bug
  that load balancing makes more likely to trigger.  The fix (replacing
  `flush_tlb()` with `tlb_shootdown()`) should be implemented before or
  alongside this design.

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
        │     ├── gate: (rq empty) OR (src.nr_running > rq.nr_running + 1)
        │     ├── count = max(1, (src.nr_running - rq.nr_running) / 2)
        │     ├── double-lock rq_locks (addr-ordered, single IRQ save)
        │     ├── dequeue_task from src, normalize vruntime, enqueue_task to local
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
  ±1 task).  On x86-64 TSO, plain `mov` loads have acquire semantics;
  however, without a compiler barrier the optimiser may hoist or merge
  reads across loop iterations.  All lockless `nr_running` reads should
  use a `READ_ONCE()`-style volatile access:
  ```c
  uint32_t nr = *(volatile uint32_t *)&percpu_data[i].nr_running;
  ```

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

 2. Gate (see rationale § "Oscillation prevention"):
      a) If local nr_running == 0 → proceed (idle steal).
      b) Else if src.nr_running <= local.nr_running + 1 → return.
         (require ≥ 2 task gap to prevent permanent 2:1↔1:2 ping-pong)
      c) Else proceed.

 3. count = max(1, (src.nr_running - local.nr_running) / 2)
    Migrate half the difference, not half of src's total.  This
    converges toward balance in O(log N) rounds instead of overshooting.

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
      - dequeue_task(t, src_rq)       // rbtree_erase + on_rq=false + src.nr_running--
      - if src rbtree now empty: src.min_vruntime = 0
      - t->cpu = rq->cpu_id
      - normalize vruntime (see rationale § "Wakeup boost interaction"):
          t->vruntime = max(t->vruntime, rq->min_vruntime)
      - enqueue_task(t, rq)           // deadline set + on_rq=true + rbtree_insert + rq->nr_running++

    Using enqueue_task / dequeue_task rather than manual rbtree
    manipulation avoids duplicating the nr_running accounting logic
    and guarantees consistency with the existing code paths.

 6. If no tasks were taken and src runqueue is now empty:
      src.min_vruntime = 0
```

---

## Integration points

| Location | Current | Change |
|---|---|---|
| `spawn_user_task()` — CPU selection | `tsk->cpu = cpu_id()` | → `tsk->cpu = sched_pick_cpu()` |
| `spawn_user_task()` — after enqueue | (nothing) | → `sched_notify_remote(tsk)` |
| `do_fork()` — CPU selection | `tsk->cpu = 0` or copied from parent | → `tsk->cpu = sched_pick_cpu()` |
| `do_fork()` — fair_start | `percpu_data[cpu_id()].min_vruntime` | → `percpu_data[tsk->cpu].min_vruntime` |
| `do_fork()` — after enqueue | (nothing) | → `sched_notify_remote(tsk)` |
| `deferred_free_spawn()` | `df->cpu = 0` | → **remove line** (do_fork already uses sched_pick_cpu) |
| `schedule()` step 3.5 | (absent) | → `sched_balance(rq)` |
| `enqueue_task()` | — | → `rq->nr_running++` |
| `dequeue_task()` | — | → `rq->nr_running--` |

### `do_fork()`: correct ordering

Currently `do_fork()` (task.c:1299-1313):

```c
uint64_t fair_start = percpu_data[cpu_id()].min_vruntime;
tsk->vruntime = current->vruntime < fair_start ? current->vruntime : fair_start;
// ... other init ...
tsk->cpu = cpu_id();
```

After this change, pick the CPU **first**, then derive `fair_start` from
the **target** CPU:

```c
uint32_t target_cpu = sched_pick_cpu();
uint64_t fair_start = percpu_data[target_cpu].min_vruntime;
tsk->vruntime = current->vruntime < fair_start ? current->vruntime : fair_start;
// ... other init ...
tsk->cpu = target_cpu;
```

Without this ordering, a child placed on a freshly-unloaded CPU (whose
`min_vruntime` was just reset to 0 by `sched_balance`) would inherit
the parent CPU's higher vruntime and be placed unfairly far right in the
target rbtree.  With the target CPU's `min_vruntime`, the child gets
correct initial placement.

### `deferred_free_spawn()`: remove `df->cpu = 0` override

### `deferred_free_spawn()`: remove `df->cpu = 0` override

`task.c:1518-1520` creates the deferred-free reaper kthread, then
overrides the CPU:

```c
task_t *df = kernel_thread(df_reaper_main, ...);
df->cpu = 0;   // ← MUST REMOVE — overrides sched_pick_cpu()
```

`kernel_thread()` already calls `do_fork()` which now uses
`sched_pick_cpu()`.  The `df->cpu = 0` override would pin the reaper
to the BSP, defeating load balancing.  Simply delete the line.

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

### Oscillation prevention

A naive `nr_running / 2` steal with gate `src.nr_running > local.nr_running`
produces permanent oscillation on small imbalances:

```
2 CPUs, 3 tasks total: (2:1) → steal 1 → (1:2) → steal back → (2:1) → …
```

Every 10 ms the tasks migrate, causing unnecessary cache misses and IPI
traffic.  The system is already *optimally* balanced at (2:1) for 3 tasks
on 2 CPUs — there's no genuine imbalance to fix.

**Two changes prevent this:**

1. **Gate requires ≥ 2 task gap:** `src.nr_running > local.nr_running + 1`.
   At (2:1), `2 > 1 + 1` is false — no steal.  Only (3:1), (4:1), etc.
   trigger.  Idle steal still works: at (1:0), `1 > 0 + 1` is false, but
   local `nr_running == 0` passes gate (a).

2. **Steal half the difference, not half of src:** count =
   `max(1, (src.nr_running - local.nr_running) / 2)`.  Examples:
   | Before | Diff | Count | After | Result |
   |--------|------|-------|-------|--------|
   | 100:98 | 2    | 1     | 99:99 | ✓ perfect balance |
   | 8:5    | 3    | 1     | 7:6   | ✓ close (vs 4:9 with src/2) |
   | 2:0    | 2    | 1     | 1:1   | ✓ idle steal works |
   | 5:1    | 4    | 2     | 3:3   | ✓ converges in one round |

   Old formula (`src.nr_running / 2`) on 100:98 would steal 50 tasks,
   overshooting to 50:148 — worse than the original imbalance.

Together these converge to balance in O(log Δ) rounds with minimal
overshoot.

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
- **Small-queue convergence:** with `count = max(1, diff/2)`, systems
  converge monotonically toward balance.  The (2:1) → (1:2) oscillation
  is prevented by the `+ 1` gate (see rationale § "Oscillation
  prevention").
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
- **sched_balance can steal `current`:** `schedule()` step 2 re-enqueues
  the calling task if it's still RUNNING.  `sched_balance` (step 3.5)
  may then steal it to another CPU.  This is **safe**: `pick_eevdf`
  simply won't find it in the local rbtree and selects a different task.
  The stolen `current` resumes on the target CPU when it's picked there.
- **Tail stealing naturally migrates CPU-bound tasks:** tasks with high
  vruntime (large deadline) are the ones that ran continuously without
  sleeping.  These are typically CPU-bound.  Interactive tasks (low
  vruntime from frequent sleep/wakeup-boost cycles) stay near the front
  of the rbtree and remain on the source CPU.  The design gets this
  behavior *for free* from the EEVDF deadline ordering — no explicit
  task classification needed.
- **Reschedule IPI may be silently dropped:** `ipi_send()` (ipi.c:54-68)
  polls ICR with a 10,000-iteration timeout, and if the previous IPI
  hasn't completed, the send is silently skipped.  For reschedule IPI,
  the consequence is mild: the target CPU discovers new tasks on its
  next LAPIC timer tick (≤10 ms) via `need_resched`.  The
  `sched_notify_remote` path sets `need_resched` as a belt-and-suspenders
  fallback precisely for this case.
- **Idle CPUs hammering the busiest lock:** all idle APs enter
  `sched_balance` at 100 Hz (LAPIC timer).  With multiple idle cores,
  all attempt to steal from the same busy CPU simultaneously.  The
  double-lock serializes them — only one succeeds.  For a first
  implementation this is acceptable; future optimization could add an
  exponential backoff in the idle loop.
- **Init may start on an AP:** `spawn_user_task("/bin/init")` now uses
  `sched_pick_cpu()`.  If the AP's runqueue is empty, init lands on the
  AP.  This is safe — `user_init_task` pointer (task.c:874-876) is set
  regardless of chosen CPU, and BSP's remaining `task_init()` work
  (mutex test, deferred free spawn) doesn't depend on init running
  first.
- **BSP PIT vs AP LAPIC timer:** BSP is driven by PIT at 100 Hz, APs by
  per-core LAPIC timer at 100 Hz.  Both trigger `schedule()` at the same
  frequency — `sched_balance` firing rate is uniform across CPUs.

---

## Known limitations

1. **O(num_cpus) scans:** `sched_pick_cpu()` and `sched_balance()` both
   iterate all online CPUs.  Acceptable for NR_CPUS ≤ 8.  A future
   optimization could maintain a global `max_nr_running_cpu` hint.

2. **`nr_running` does not distinguish task types:** 2 CPU-bound tasks
   and 2 idle-sleeping tasks both report `nr_running = 2`, but have
   completely different actual CPU demand.  Similarly, interactive
   tasks that sleep frequently cause `nr_running` to fluctuate.  For
   OS01's current workloads (syscall tests, busybox shell), this is
   adequate.  A future upgrade could add per-task load tracking (PELT
   weight) via `enqueue_task`/`dequeue_task` hooks — the call sites
   are already in place.

3. **Pull-only model:** all balancing is driven by idle or less-loaded
   CPUs pulling from busier ones.  A busy CPU never proactively pushes
   tasks.  If *all* CPUs are busy but unbalanced, convergence is slower
   (each CPU only rebalances on its own `schedule()`).  For OS01 this
   is acceptable; a future push path could use `ipi_send` to actively
   wake idle CPUs for immediate steal.

4. **No tickless/idle awareness:** every CPU wakes at 100 Hz regardless
   of load.  On real hardware this increases power consumption.  OS01
   targets QEMU where this is negligible.

5. **Test mock gap:** `test/include/kernel/percpu.h` diverges from the
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
4. **systest: oscillation resistance** — spawn 3 busy-loop processes
   on 2 CPUs.  Verify they stabilize at (2:1) or (1:2) distribution
   and don't ping-pong every tick.
5. **Manual: remote reschedule latency** — boot with `OS01_DEBUG_SCHED`,
   `fork()`, verify remote CPU's `schedule()` runs within 1-2 ticks
   of IPI instead of full 10 ms.
6. **Unit test `sched_pick_cpu()`** — requires test mock update (see
   known limitation #3).  Defer to a follow-up if mock sync is too
   extensive.

---

## Files changed

| File | Change |
|---|---|
| `kernel/include/kernel/percpu.h` | +`uint32_t nr_running` |
| `kernel/sched/task.c` | +`sched_pick_cpu()`, +`sched_balance()`, +`sched_notify_remote()`, update `enqueue_task`/`dequeue_task`, update `do_fork()` (CPU selection + fair_start + IPI), remove `df->cpu=0` override |
| `kernel/sched/task.c` (prereq) | COW fix: replace `flush_tlb()` with `tlb_shootdown()` in `fork_mm_copy()` |
| `libc/include/rbtree.h` | +`rbtree_last`, +`rbtree_prev` declarations |
| `libc/rbtree/rbtree.c` | +`rbtree_last()`, +`rbtree_prev()` implementations |
| `test/include/kernel/task.h` | sync `nr_running` field |
| `test/include/kernel/percpu.h` | sync `nr_running` field |
