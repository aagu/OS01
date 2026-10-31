# aarch64 EL1 Kernel-Thread Scheduler — Plan Series Index

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

## Implementation status — 2026-10-31

P1–P5 are implemented and reviewed **APPROVE** on
`feature/aarch64-el1-sched`; implementation source is `a6e7c321`.
See the [final review](../reports/2026-10-31-aarch64-el1-sched-final-review.md)
and [validation report](../reports/2026-10-30-aarch64-el1-sched-validation.md).
The x86 UP kernel-selftest retains its two documented pre-existing
failures; CPU2/8 pass. The phase plans below remain the implementation
record. Local master integration and its regression results are recorded
in the [merge validation](../reports/2026-10-31-aarch64-el1-sched-merge-validation.md).
No remote was updated.

**Goal:** Land the aarch64 EL1 kernel-thread scheduler as five independently reviewable, independently testable phases — from arch-context extraction through to strict QEMU acceptance — without ever asking a reviewer to hold the whole feature in their head.

**Architecture:** The series follows the spec's own dependency order. Each phase produces a working, gated artifact: Phase 1 preserves x86 behavior while adding the specified self-switch safety guard, Phase 2 splits the scheduler into arch-neutral core plus x86 system startup, Phase 3 gives aarch64 a real EL1 context switch and a BSP handoff, Phase 4 adds cooperative yield/blocker/reclaim plus the late selftest, Phase 5 adds the `MODE=sched` QEMU harness and runs the full regression gate. The interface contract between phases is frozen here (see "Frozen Interface Contract") so a phase can be implemented without reading the other phase plans.

**Tech Stack:** C (freestanding kernel), x86-64 and AArch64 assembly, Clang/LLD, profile Make builds (`mk/profiles/*.mk`), host C unit tests (`hosttests/`), kernel-internal protocol-v1 selftests (`kernel/selftest/`), Python harness + QEMU (`qemutests/`).

**Spec:** [2026-10-30-aarch64-el1-kthread-scheduler-design.md](../specs/2026-10-30-aarch64-el1-kthread-scheduler-design.md) — v2, design baseline `c6cdf45d`. Every phase below implements a numbered section of that spec; the section is cited per task.

---

## The series

| # | Plan file | Spec § | Deliverable | Gate before the next phase |
|---|---|---|---|---|
| 1 | `2026-10-30-aarch64-el1-sched-p1-arch-context.md` | §2, §4, §5.1 | `arch/context.h` facade, per-arch `thread_t`, x86 asm moved to arch-private header, common switch tail `task_switch_complete()` | x86 `test-host` + `test-static` + `phase-0` + `kernel-selftest` green, x86 `thread_t` offsets unmoved |
| 2 | `2026-10-30-aarch64-el1-sched-p2-core-split.md` | §4, §5.1, §8.1, §8.3 | `kernel/sched/core.c` becomes arch-neutral; `sched/boot.c`, `sched/kthread.c`, `sched/internal.h` split out; x86 system startup isolated; aarch64 common TUs gain a compile-only check; product whitelist changes in Phase 3 | x86 gates green **and** aarch64 `MODE=smp` / `MODE=m3-probe` still pass with the common TUs compile-checked; product link unchanged until Phase 3 |
| 3 | `2026-10-30-aarch64-el1-sched-p3-aarch64-el1.md` | §5.2, §5.3, §6, §7 | aarch64 EL1 `switch.S` / `thread_entry.S` / `task_arch.c`, BSP stack handoff, CPU0-only `scheduler_ok`, AP isolation | aarch64 boots to a BSP idle with `scheduler_ok=1`; `MODE=smp`, `MODE=m3-probe`, `MODE=gic-spi` unchanged; aarch64 audit green |
| 4 | `2026-10-30-aarch64-el1-sched-p4-cooperative.md` | §5.3, §8.1, §8.2, §8.3 | `kthread_yield()`, blocker/wake semantics, `kthread_exit()` path, `kernel/selftest/test_aarch64_scheduler.c` as a protocol-v1 late case | aarch64 late selftest records and reports exactly one PASS per case; x86 kernel-selftest unchanged |
| 5 | `2026-10-30-aarch64-el1-sched-p5-acceptance.md` | §9, §10 | `MODE=sched` QEMU runner + strict evidence parser, fault-injection variants, static/host audits, full two-arch regression gate, docs | every §10 gate command recorded with real exit codes; roadmap marks only "EL1 BSP cooperative thread scheduling" |

Each phase plan is a standalone document: an executor reads *that* plan plus this index and nothing else.

## Why five phases and not one

The spec spans four independent risk surfaces (arch context, scheduler decomposition, aarch64 EL1 machine code, acceptance harness). A single plan would force one reviewer to validate assembly offsets, x86 regression risk, allocator rollback, and QEMU evidence policy at once — the failure mode the spec's own §13 review process was designed to avoid. Splitting keeps every reviewer's scope to one surface while the frozen contract below keeps the seams honest.

---

## Global Constraints

These hold for **every** phase in the series. A phase plan may add constraints, never relax these.

- Spec baseline: `docs/superpowers/specs/2026-10-30-aarch64-el1-kthread-scheduler-design.md` (v2). When a phase plan and the spec disagree, the spec wins and the phase plan is a bug.
- Code baseline: `c6cdf45d`. Every phase lands as its own commit(s) on a worktree branch; no phase merges to `master` without the user's explicit go-ahead.
- Out of scope for the whole series (spec §3.2): EL0/SVC, user address spaces, fork/exec, user VMA/COW/uaccess, signal delivery, per-CPU clockevent, AP scheduling, load balancing, resched IPI, IRQ-return preemption, FPU/SIMD/SVE/SME context save, a unified `kernel_main`, a unified SMP boot interface, scheduler policy changes.
- aarch64 threads are **cooperative**. A compute loop that never yields or blocks monopolizes the BSP. No test may claim preemption works because a timer printed something.
- aarch64 shares one kernel address space (TTBR). No thread writes TTBR0/TTBR1/VBAR/TPIDR_EL1/SP_EL0. TPIDR_EL1 always points at the current CPU's runtime `percpu_t`.
- `num_cpus` stays the true enumerated CPU count. It is never clamped to 1 to make scheduling "work".
- aarch64 only CPU0 ever gets `scheduler_ok=1`. AP `scheduler_ok` stays 0, APs register no idle task, APs never call `schedule()`/`current`. `kernel_thread_on_cpu()` to a non-zero CPU returns `-EINVAL` even when that AP is online.
- No success-returning stubs for unported subsystems. VFS/VMA/fork are absent from the aarch64 build because the code that uses them is absent — not because a stub returns 0.
- x86 behaviour is a regression surface, not a design target. x86 `thread_t` field order and offsets must not move (Phase 1 asserts this).
- Kernel diagnostics use `log_err`/`log_warn`/`log_info`/`log_debug`. Raw output is permitted only for protocol markers. The aarch64 log backend cannot format `%d` — write numeric diagnostics with the specifiers it does support and actually print the value.
- Structure changes (any `thread_t`, `task_t`, `percpu_t`, or per-arch context layout) require `make clean` before rebuilding. `percpu_t` is not to be reordered; `ipi_ready`/`tlb_ack_gen` stay the tail fields.
- New aarch64 assembly uses no FP/SIMD registers; all reachable kernel C stays `-mgeneral-regs-only`.
- Every phase ends with a report that records the real command, the real exit code, the CPU count, and the log path. "Not run" is a valid, required entry; a silent skip is not.

---

## Review Focus (series-wide)

The five inputs/conditions most likely to bite someone using this software. Each line names the phase that owns its test; the owning phase's plan contains the actual test step.

1. **Self-switch (`prev == next`)** must not clear `on_cpu` and must not self-reap — a scheduler that reaps its own stack is a use-after-free one instruction later. Owner: Phase 1 (x86 tail extraction) and Phase 3 (aarch64 `switch.S`).
2. **Corrupt context values** — saved SP equal to `top`, SP below `usable_low`, an arbitrary low-address continuation, or a continuation in non-executable RAM — must be fatal *before* the stack switch, never after. Owner: Phase 3 (validation backend) with the negative cases executed in Phase 5.
3. **x86 boot-phase thread creation** — the `net_lwip_init()` UP startup call must still succeed while BSP `scheduler_ok` is 0, and that window must close permanently at the first BSP `scheduler_ok` publish. Owner: Phase 2.
4. **First thread entry** — `fn(arg)` runs exactly once, with IRQs enabled, `in_schedule == 0`, `on_cpu == 1`, `current` already correct before any C prologue, and never returns into a fabricated caller. Owner: Phase 3 (aarch64 entry) and Phase 4 (selftest case).
5. **Kernel-thread exit** — shared `init_mm` must not have its VMAs or page tables freed, and the reclamation must be observable as a net-zero allocation delta, not as "the test stopped crashing". Owner: Phase 2 (`kthread_exit` extraction) and Phase 4 (selftest case).

---

## Frozen Interface Contract

Names and signatures below are decided here so that no phase needs to read another phase's plan. A phase that must change one of these updates this index in the same commit and says so in its report.

### Arch facade — `kernel/include/arch/context.h`

```c
struct task_struct;   /* opaque here; task_t is defined in <sched/task.h> */

/* Current task. Valid only while running on a task stack: never on the
 * boot stack, never on an AP work stack, never on an exception stack. */
struct task_struct *arch_current_task(void);

/* Switch from prev to next. The caller has already masked interrupts.
 * It suspends prev and transfers to next. This invocation returns only
 * when prev is selected again, on prev's own stack and C continuation.
 * The scheduler skips every self-switch; a backend self-switch is a no-op. */
void arch_task_switch(struct task_struct *prev, struct task_struct *next);

/* Validate a task's saved context before it is used. Returns true when
 * the context is usable. On false, *reason points to a static string. */
bool arch_task_context_valid(struct task_struct *t, const char **reason);

/* Append the architecture-specific part of a task to a hang/watchdog
 * dump. Common code must not read thread->rip / thread->rsp /
 * thread->cr3 directly. */
void arch_dump_task_context(struct task_struct *t);
```

`kernel/include/arch/context.h` selects the per-arch header by compiler macro and includes it. Per-arch headers live at `kernel/include/arch/x86_64/context.h` and `kernel/include/arch/aarch64/context.h`; each defines that architecture's `thread_t` layout and its assembler-visible offset constants.

**x86 keeps the existing machine-level convention.** The x86 `switch_to` inline assembly stays inline assembly (a macro `arch_task_switch(prev, next)`), because x86 resumes by returning through a pushed `thread->rip` and cannot be expressed as an ordinary call without changing what `schedule()` observes. The aarch64 backend is a C wrapper in `task_arch.c` that invokes the ordinary assembly function `aarch64_switch_context()` in `cpu/switch.S`. The facade block above specifies callable semantics, not literal extern declarations: both current accessors are per-arch static inline and x86 switch is a macro. The spec (§5.1) explicitly permits the two backends to differ at the machine level; the *contract* above is what must match.

### Common scheduler tail

```c
/* kernel/include/sched/internal.h */
/* Runs on the *next* task's stack, after the architecture has installed
 * next's context. Captures the self-reap decision and the objects to
 * release BEFORE task_finish_switch() clears on_cpu, because after that
 * a waiter may free prev. Never reads prev after task_finish_switch()
 * returns. */
void task_switch_complete(task_t *actual_prev);
```

### Public kernel-thread API (unchanged signatures, spec §8.1)

```c
int   kernel_thread(uint64_t (*fn)(uint64_t), uint64_t arg, uint64_t flags);
int   kernel_thread_on_cpu(uint64_t (*fn)(uint64_t), uint64_t arg, unsigned cpu);
task_t *create_kthread(uint64_t (*fn)(uint64_t), uint64_t arg, const char *name);
void  kthread_yield(void);   /* new in Phase 4; distinct from libc sched_yield() */
```

`kernel/include/sched/task.h` keeps `switch_to(prev, next)`, `get_current_task()` and `current` as compatibility aliases for `arch_task_switch(prev, next)` and `arch_current_task()` respectively (both current forms use the latter).

### Additional frozen interfaces and phase boundaries

```c
/* context.h: declare opaque task_t before selecting ONE per-arch header.
 * x86 supplies an arch_task_switch macro, never a same-name prototype.
 * Inline current accessors have no conflicting extern declaration. */
void arch_task_prepare_first_switch(task_t *t, void (*idle_entry)(void));
void arch_task_check_prev(task_t *t); /* real fatal diagnostics, no false success */
int arch_kthread_prepare(task_t *t, uint64_t (*fn)(uint64_t), uint64_t arg);
void arch_kthread_drop_files(task_t *t);
/* prepare: real arch context + resources; errno on failure, acquired
 * files/FPU cleaned and nulled on failure. drop_files: detach/unpin files
 * only; thread/FPU/raw-stack remain owned by common rollback/switch tail. */
int kthread_alloc_and_publish(uint64_t (*fn)(uint64_t), uint64_t arg,
    const char *name, unsigned target_cpu, bool pinned,
    task_t **out_task, int *out_pid);
/* Outputs start NULL/0; capture both BEFORE publication. PID APIs use
 * out_pid without dereferencing out_task after publication. */
void kthread_exit(uint64_t code); /* noreturn, available since Phase 2 */
void sched_boot_creation_begin(void); /* x86 BSP only, once prerequisites hold */
void sched_boot_publish_idle(void); /* never reset an existing list/runqueue */
void sched_boot_start(void); /* close boot window, publish scheduler_ok, RETURN */
bool sched_boot_creation_open(void);
/* aarch64 task_arch.c C wrapper validates, then calls this assembly ABI:
 * x0=actual_prev, x1=prev_ctx, x2=next_ctx. No task_t offsets in assembly. */
void aarch64_switch_context(task_t *actual_prev, thread_t *prev_ctx,
                           thread_t *next_ctx);
void aarch64_bsp_stack_enter(uint64_t idle_sp); /* noreturn assembly, x0=top-16 */
void arch_aarch64_bsp_handoff(void); /* noreturn C continuation on idle stack */
```

`ARCH_HAS_REMOTE_RESCHED` is a compile-time constant (x86 1, aarch64 0); gate both the x86 IPI include and call with it. Reject unsupported placement BEFORE publication. The common tail first returns if `actual_prev == current`, without finish or free.

Phase 2 compiles the common TUs with the real aarch64 build flags into an unlinked compile-check directory (new focused `test-static CASE=sched-common-compile` selector); unresolved real arch externs are permitted there, with no stub and no code-eliding test macro. Product aarch64 keeps its original whitelist/boot. Phase 3 supplies all real backends/global initialisers and adds the common whitelist in one coherent integration change.

### Frozen scheduler evidence manifest (Phase 4 → Phase 5)

Register and record these **nine** late IDs, in this exact order:

1. `aarch64_sched_yield`
2. `aarch64_sched_regs`
3. `aarch64_sched_entry`
4. `aarch64_sched_irq`
5. `aarch64_sched_blocker`
6. `aarch64_sched_reclaim`
7. `aarch64_sched_idle`
8. `aarch64_sched_timer`
9. `aarch64_sched_ap`

`void test_aarch64_scheduler_run(void)` executes each case, then calls `selftest_record_late(id, rc)` exactly once. Registration only declares IDs; it does not execute them. `selftest_begin_run(9)` declares early + these late cases before START. After BSP handoff, `sched_boot_start()` returns on the idle stack, IRQ is enabled, then the selftest driver runs, the single `selftest_end_run()` follows, then idle. This applies to every scheduler-enabled aarch64 selftest image, including ordinary `MODE=smp`; Phase 3 retains the old END until Phase 4 adds registration and execution atomically.

Each case checks production state inside the kernel; the host cannot infer RAM state from a PASS line. Protocol PASS is emitted only AFTER these assertions. The ordered diagnostic evidence records are anchored `[sched-evidence] id=<id> ...` (decimal fields, one record per case; case `ap` additionally emits one per AP). Freeze fields as follows:

- yield: `a_yields>=10000 b_yields>=10000 switches>0`.
- regs: `reg_fail=0 stack_fail=0 sp_fail=0` after both sentinel patterns.
- entry: `entries=1 arg_ok=1 pre_c_sp_ok=1 irq_enabled=1 current_ok=1`.
- irq: `on_switch=1 off_switch=1 on_noswitch=1 off_noswitch=1`, all state-restoration checks true.
- blocker: `already=1 explicit=1 pre_switch=1 scan=1 duplicate_enqueue=0` with four deterministic timing controls.
- reclaim: `created>=1000 stack_live=0 thread_live=0 fpu_live=0 next_stack=1 list_delta=0 rq_delta=0`, sampled only after the last switch tail has run.
- idle: `idle_in_tree=0 idle_observed=1 wake_ticks<=10`, worker blocked and BSP idle observed before timer/condition recovery.
- timer: `tick_delta>=3 irq_frame_ok=1 nested_schedule=0 irq_switches=0`, real IRQ frame probe while threads run; counters have explicit test-only observational hooks, no fake dispatch.
- ap: `cpu_count=<actual> cpu0_only=1` plus each AP's `cpu=<id> scheduler_ok=0 nr_running=0 work_ok=1`. Re-run existing real shootdown/work probe after BSP publication at >=2 CPUs. Single CPU has zero AP lines and permits only the existing shootdown SKIP.

Normal images print exactly one `[sched] idle-ready cpu=0 scheduler_ok=1 irq=on` from the published idle continuation, then remain responsive to tick; no early boot marker substitutes for it. A Phase-3 runner `qemutests/aarch64_scheduler.py --idle-smoke --cpus 1 2 4 8 --image PATH --firmware PATH` waits for it with a fixed 120s deadline. Phase 5 extends that same runner, preserving idle smoke. This mode uses an already-built normal image, never silently rebuilds a selftest variant.

### Per-phase interface additions

| Phase | Adds |
|---|---|
| 1 | `arch_current_task()`, `arch_task_switch()`, `arch_task_context_valid()`, `arch_dump_task_context()`; `task_switch_complete()` |
| 2 | `sched_boot_publish_idle()`, returning `sched_boot_start()`, `sched_boot_creation_begin()`, `sched_boot_creation_open()`; constructor/resource hooks specified above |
| 3 | aarch64 `thread_t` layout + `AARCH64_THREAD_*` offset constants; `kernel/arch/aarch64/cpu/task_arch.c`: `arch_kthread_prepare()`, `arch_aarch64_bsp_handoff()` and `aarch64_switch_context()` as specified above |
| 4 | `kthread_yield()`, nine late scheduler IDs in the manifest above; kthread_exit already exists in Phase 2 |
| 5 | `qemutests/aarch64_scheduler.py` (`MODE=sched`), aarch64 profile variant `sched` |

---

## Plan review record — 2026-10-30

Scope: plan review and local corrections only. The five phase files retain their original Task headings and step counts; no implementation task has been marked complete. Implementation is intended for Luna after this review.

| Review scope | Review iterations | Final verdict |
|---|---|---|
| P1/P2 and common contracts | 3 | APPROVE |
| P3/P4 and AArch64 machine contracts | 2 | APPROVE |
| P4/P5 and acceptance contracts | 3 | APPROVE |
| Independent whole-series review | Initial review plus fix verification | APPROVE |

Corrections cover context/header compatibility (including direct `get_current_task()` callers), self-switch and reclaim ownership, constructor publication, boot-window/start ordering, P2 compile-only versus P3 product integration, AArch64 stack/LR/IRQ entry, explicit late execution, evidence/variant contracts and staged acceptance commands. These verdicts assess the plans; build/QEMU/host gates remain implementation work and have not been run for this documentation review.

## Execution Handoff

Implementation is complete; the following execution order is retained for
historical reference, not a request to restart Phase 1.

This index is a map, not an executable plan. Execute **Phase 1** first:

`docs/superpowers/plans/2026-10-30-aarch64-el1-sched-p1-arch-context.md`

Each phase plan ends with its own handoff section naming the next plan file. Phases run strictly in order — Phase 2 depends on Phase 1's facade, Phase 3 on Phase 2's split, and so on — but each is reviewed and merged independently.
