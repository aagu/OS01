# Scheduler complexity assessment & extension guide

Status: assessment / decision record — Aug 2026, after commit
`f58d1a1` ("fix(sched): never schedule one task on two CPUs") and
`3b28b3d`.  Validated baseline: **36/36 gtk+stdio runs clean**
(systest 126 passed, 2 CPUs).

> Note: `docs/scheduler.md` still describes the pre-EEVDF round-robin
> design (global task list + per-CPU affinity).  The code has moved to
> per-CPU rbtree runqueues with EEVDF (`pick_eevdf`, vruntime/deadline)
> plus `sched_balance()` work stealing.  This document reflects the
> current code, not the old doc.

## 1. Current scheduler composition (`kernel/sched/task.c`, 1947 lines)

| Component | ~Lines | Complexity |
|-----------|--------|------------|
| EEVDF core (`update_curr` / `cmp_deadline` / `enqueue_task` / `dequeue_task` / `pick_eevdf`) | 61 | vruntime + deadline + rbtree (Linux 6.6-style) |
| `task_wake` (on_rq/on_cpu re-check, wakeup boost, cpu retry) | 63 | lock ordering + atomic visibility |
| `sched_balance` (double-lock address-ordered work stealing + timed batch pull) | 99 | dual rq_lock, rbtree_last, vruntime normalization |
| `schedule()` (in_schedule guard, on_cpu, IRQ save, reaper, balance, pick, switch, resume) | 116 | every stage has a race guard |
| on_cpu / in_schedule / atomic ACQUIRE-RELEASE synchronization | ~50 | mandatory for preemptive SMP |
| zombie reaper + `deferred_free` (df-reaper kthread, async queue) | ~80 | async free, double-free guards |
| do_exit / do_waitpid / do_fork / task_init | 540 | functionality |
| blocker framework (`blocker_wait` / condition callbacks) | ~100 | blocking semantics |

## 2. Verdict

The scheduler is **more complex than the workload requires**, but the
excess is concentrated in three *optional* components, not in
preemptive SMP itself:

- **EEVDF** is overkill for 2 CPUs / single-digit task counts.  A
  simple vruntime (min-vruntime pick) or plain round-robin behaves
  almost identically at low load.
- **Work stealing** (`sched_balance`) has no payoff on 2 CPUs.
  Creation-time round-robin placement (skill doc "smp-creating-claim",
  option A, ~5 lines) already balances.  The 99 lines + dual locks are
  pure redundancy at this scale.
- **Zombie reaper + deferred_free** is the largest bug surface — the
  Round-5 double-book race (commit `f58d1a1`) was fixed at 4 layers,
  all around this async-reclaim design.  The classic alternative
  (per-CPU fixed exit stack + self-kfree in do_exit) removes the
  reaper scan, the df-kthread, the deferred queue, and most of the
  on_cpu machinery.

Mandatory complexity (do NOT remove): per-CPU rq locks, `on_cpu`,
`in_schedule`, atomic ACQUIRE/RELEASE visibility — these are the cost
of choosing tick preemption + SMP.  Removing them means dropping
preemption or SMP.

## 3. Impact on planned features

### 🟢 Unaffected (~70% of the roadmap)

- Userspace: dynamic linker (ld.so), TLS, /etc/rc scripts — no
  scheduler involvement.
- FS / drivers / networking (lwIP worktree) — independent subsystems.
- `setsid`/`setpgid` / job control: add fields to `task_t` +
  syscall-layer logic; touches `do_fork` initialization only, not the
  scheduler core.
- Signal / PTY / existing FS enhancements.

### 🟡 Blocked but workable (moderate)

- **More CPUs (`-smp 4/8`)**: the design (address-ordered dual locks,
  EEVDF) scales logically, but only 2 CPUs have ever been validated —
  the cost is *re-validating the races*, not writing code.
- **epoll / multi-fd waiting**: the blocker model is "one task, one
  condition callback".  epoll needs "one task, many events (OR)" —
  extend `blocker_t` or add a waitqueue.  Independent of the scheduler
  core but lives in task.c.

### 🔴 Genuinely at risk (high blast radius)

1. **Priorities / real-time scheduling**: EEVDF is a pure fairness
   scheduler; adding RT priority means reworking the core (61-line
   EEVDF + pick).  A simple vruntime or RR core is *easier* to extend
   with priorities (cf. Linux O(1) priority arrays).  The algorithm
   choice is a liability for this feature.
2. **Anything touching task lifetime** (`exit_group`, richer
   parent/child relationships, exec-failure cleanup): the
   reaper + deferred_free area is the minefield — Round 5's four-layer
   fix lives entirely around it.  Changing it re-walks every race we
   already fixed.

## 4. Risk map / known minefields

| Area | Risk | Before touching, read |
|------|------|----------------------|
| do_exit / zombie reaper / deferred_free | async reclaim races (double-book, on_cpu, double-free) | Hermes skill `os01-dev` → `references/smp-scheduler-races-5-doublebook.md` |
| pick→switch_to window | task_wake/sched_balance re-enqueue → same task on two CPUs | same reference |
| on_rq/on_cpu | non-atomic access = lost update across CPUs | all accesses must be `__atomic_load_n(ACQUIRE)` / `__atomic_store_n(RELEASE)` |
| resume path | diagnostics must not use rbp-relative addressing (rbp may be clobbered) | `cr2 == rbp - 0x270` is the tell-tale |

## 5. When a refactor is worth it (triggers)

Do **not** refactor now — the current code is validated and stable.
Refactor only when one of these actually happens:

1. Adding RT/priorities → first downgrade EEVDF to simple vruntime
   (behaviorally ~identical at low load), then add priorities on the
   simple core.
2. Frequently changing task exit / parent-child semantics → first
   replace reaper/deferred_free with a per-CPU exit stack + self-kfree.
3. Userspace/FS/network features only → do nothing to the scheduler.

Estimated savings if 1+2 are done: task.c ~1947 → ~1500 lines, and
roughly a third of the race-guard code disappears with the reaper.

## 6. Historical validation log

| Build | Change | Result |
|-------|--------|--------|
| v6 | SWITCH-RING diag baseline | crash (RIP=user page) |
| v7 | + on_cpu=1 at pick, task_wake on_cpu check | crash (stale t->cpu) |
| v8 | + next->cpu sync at pick | crash (visibility) |
| v9 | + atomic ACQUIRE/RELEASE on on_rq/on_cpu | 12/12 clean |
| v10 | same as v9, second run | crash in run 9 (sched_balance hole) |
| v11 | + sched_balance skips on_cpu | 12/12 clean |
| v12 | same, second run | 12/12 clean |
| v13 | diagnostics stripped, fixes kept | 12/12 clean |
