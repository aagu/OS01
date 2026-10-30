# Phase 4 — Cooperative yield, blocker/wake semantics, and the aarch64 scheduler selftest

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make cooperative scheduling actually work on aarch64 — `kthread_yield()`, correct blocker wake ordering, and the kernel-thread exit/reclaim path — and prove it with an in-kernel selftest that runs on the production code paths.

**Architecture:** A public `kthread_yield()` wraps `schedule()` with explicit IRQ-state save and restore, giving callers a defined way to hand the CPU over without scattering unconditional IRQ enables. The existing blocker framework (`blocker_wait`, `blocker_wake`, `task_wake`, the scan fallback) is reused as-is rather than duplicated. `kernel/selftest/test_aarch64_scheduler.c` exercises all of it through the production create/switch/exit paths and reports through the existing protocol-v1 coordinator.

**Tech Stack:** C, AArch64 assembly (the register/stack probe), Clang/LLD, kernel protocol-v1 selftests, QEMU `virt`.

**Spec:** [2026-10-30-aarch64-el1-kthread-scheduler-design.md](../specs/2026-10-30-aarch64-el1-kthread-scheduler-design.md) §5.3 (IRQ state), §8.1 (creation), §8.2 (blocker and wake), §8.3 (exit and reclaim), §10.2 (the aarch64 QEMU case table this phase supplies the kernel side of). Series index: [2026-10-30-aarch64-el1-sched-series.md](2026-10-30-aarch64-el1-sched-series.md).

## Global Constraints

All series constraints from the index apply. Additionally, in this phase:

- **`kthread_yield()` is a kernel API, distinct from libc's `sched_yield()`.** The libc `sched_yield()` that returns 0 is not modified and not linked as the kernel's implementation.
- **The yield wrapper saves the caller's IRQ state and restores it after `schedule()` returns.** `schedule()` leaves IRQ masked on the resume path (the existing x86 contract); the wrapper is what restores the caller's state. No caller may scatter an unconditional `arch_local_irq_enable()`.
- **`schedule()`'s existing IRQ-state table does not change.** The spec's table (§5.3) lists four paths with four distinct outcomes; the wrapper is an addition on top, not a rewrite of `schedule()`.
- **Yield is legal with IRQ enabled OR masked, with no self-held spinlock. Block requires IRQ enabled.** `blocker_wait()`'s precondition is unchanged; the IRQ-off yield cases must be executable, not rejected by a contradictory precondition. The spec forbids yielding or blocking from a GIC handler, a lock-holding region, or an SGI/TLBI callback.
- **`signal_can_wake` stays `false` for the new blocker uses**; the existing x86 signal semantics are untouched.
- **The selftest must use the production code.** Creating a thread, switching, blocking, waking, exiting, and reclaiming must go through `kernel_thread()` / `create_kthread()` / `schedule()` / `kthread_exit()` — never through a test-local reimplementation.
- **The selftest reports through the existing protocol-v1 coordinator.** The nine IDs in the index are registered as *late* cases, executed explicitly by `test_aarch64_scheduler_run()`, and recorded before exactly one coordinator `END`.
- **The aarch64 selftest's END position is a compatibility question.** Today `kernel/arch/aarch64/boot/main.c` emits the coordinator's `END` right after the early tests, before the late selftests run. Since the scheduler case runs *after* the scheduler is live, the aarch64 `END` must move later — and the spec (§10.2) requires that all existing pre/post-SMP case records be preserved and the old parser regression be updated. Plan this explicitly; see Task 4.

---

## Review Focus

1. **The yield wrapper must restore the caller's IRQ state, not the scheduler's.** A caller that yields with IRQ enabled must find IRQ enabled on return; a caller that yields with IRQ masked must find it masked. Getting this backwards either spins forever (never re-enabled) or races (re-enabled when it should not be).
2. **A no-switch yield must also restore IRQ state.** `schedule()` has early-return paths that restore the saved `sched_flags`; the wrapper's post-`schedule()` restore must compose correctly with them, and the *same* IRQ state before and after must hold on both the switch and no-switch paths.
3. **The four blocker timings must all be exercised.** Condition already true; woken after actually switching out; woken between the condition check and the switch-out; and woken only by the scan fallback. "The thread reached line two" is not proof of blocking — the test must observe the switch-out.
4. **The wake must not double-enqueue.** After a blocker wake, the task must be on the runqueue exactly once and its blocker must be cleared; the test must assert the runqueue count and the blocker state, not just that the thread continued.
5. **Exit must reclaim, not leak.** After the reclaim case, the idle thread-table count, the runqueue count, and a dedicated allocation ledger for stack/thread/FPU objects must all return to baseline. The spec (§10.2) is explicit that "the test stopped crashing" is not evidence of reclamation.

---

## File Responsibilities

Tasks 3–4 are a single integration unit. Keep their task/step structure, but defer their product-link gates and commits until all late IDs, explicit execution and END movement are wired together.


| File | Change | Responsibility after this phase |
|---|---|---|
| `kernel/sched/kthread.c` | modify | Add `kthread_yield()`; own the exit/reclaim path |
| `kernel/include/sched/task.h` | modify | Declare `kthread_yield()` next to the other kernel-thread APIs |
| `kernel/sched/core.c` | modify | Adjust the blocker framework only if the spec's four timings require it |
| `kernel/selftest/test_aarch64_scheduler.c` | create | The BSP scheduler selftest, using the production create/switch/exit paths |
| `kernel/selftest/selftest.c` | modify | Register the index's nine `aarch64_sched_*` late IDs on aarch64 |
| `kernel/arch/aarch64/boot/main.c` | modify | Move the coordinator's `END` so it covers the scheduler case, preserving every existing case record |
| `kernel/arch/aarch64/cpu/switch.S` or a new `kernel/arch/aarch64/cpu/probe.S` | create/modify | The register/stack probe the selftest needs |

---

### Task 1: Implement `kthread_yield()`

**Files:**
- Modify: `kernel/sched/kthread.c`, `kernel/include/sched/task.h`

**Interfaces:**
- Consumes: `schedule()` (Phase 2), `arch_local_irq_save()`/`arch_local_irq_restore()`.
- Produces: `void kthread_yield(void);` — declared in `kernel/include/sched/task.h` next to `kernel_thread()`.

- [ ] **Step 1: Write the failing host test**

Add a case to `hosttests/cases/` (or extend the Phase 2 kthread-constructor test file) that calls `kthread_yield()` with a mocked `schedule()` and asserts:

- the IRQ state captured before the call is the one restored after it;
- the restore happens *after* `schedule()` returns, not before;
- `schedule()` was called exactly once.

The mock `schedule()` returns without switching, so this exercises the no-switch path; the switch path is exercised on real hardware in Task 3.

- [ ] **Step 2: Run the host test to verify it fails**

Run: `make PROFILE=x86_64-clang test-host`.

Expected: FAIL — `kthread_yield()` does not exist.

- [ ] **Step 3: Implement the wrapper**

```c
/* Hands the CPU to another runnable task, restoring the caller's IRQ
 * state on return. Distinct from libc sched_yield(), which is not
 * linked as this implementation. */
void kthread_yield(void)
{
    arch_irq_state_t flags = arch_local_irq_save();
    schedule();
    arch_local_irq_restore(flags);
}
```

`arch_irq_state_t` is `uint64_t` and comes from `kernel/include/arch/irq.h`.

- [ ] **Step 4: Run the host test to verify it passes**

Run: `make PROFILE=x86_64-clang test-host`.

Expected: PASS.

- [ ] **Step 5: Commit**

```bash
git add kernel/sched/kthread.c kernel/include/sched/task.h hosttests/
git commit -m "feat(sched): add cooperative kthread_yield with IRQ state restore"
```

### Task 2: Verify and, if needed, adjust the blocker/wake path

**Files:**
- Modify: `kernel/sched/core.c` (only if a change is required)

**Interfaces:**
- Consumes: `blocker_wait()`, `blocker_wake()`, `task_wake()`, `sched_unblock_blocked()` — all already in `kernel/sched/core.c` from Phase 2.
- Produces: the four spec timings (§8.2) behaving correctly, verified by Task 3.

- [ ] **Step 1: Walk the four timings against the current implementation**

The spec (§8.2) requires four distinct timings to work:

1. the condition is already true — the caller must not sleep;
2. the task actually switches out, then is explicitly woken;
3. the wake arrives between the condition check and the switch-out;
4. the task is recovered only by the scan fallback.

Read `blocker_wait()` in `kernel/sched/core.c` and trace each timing against it. `blocker_wait()` checks the condition first, installs the blocker, double-checks, then calls `schedule()`. `blocker_wake()` checks the condition and calls `task_wake()`, then clears the blocker. `sched_unblock_blocked()` is the scan fallback run from `schedule()`.

Write down, per timing, whether the current code satisfies it and which line proves it. If a timing is *not* satisfied, the fix goes in `core.c` and must preserve the x86 signal semantics (`signal_can_wake` handling is unchanged).

- [ ] **Step 2: Fix only what Step 1 found broken**

For each unsatisfied timing, make the minimal change. The specific failure modes the spec names:

- a lost wakeup between the condition check and the switch-out — the double-check in `blocker_wait()` exists for this; verify it is actually sufficient given the task was already enqueued;
- a double-enqueue — `task_wake()` already re-checks `on_rq` and `on_cpu` under the runqueue lock; verify the blocker-clear ordering cannot leave the task both blocked and queued;
- a wake that finds the condition false — `blocker_wake()` returns without waking; verify the blocker is left intact in that case (the spec requires the blocker to be cleared only on an actual wake).

If Step 1 finds everything already correct, make no code change and say so in the report — that is a valid outcome, not a skipped step.

- [ ] **Step 3: Commit (only if there were changes)**

```bash
git add kernel/sched/core.c
git commit -m "fix(sched): satisfy the four blocker wake timings"
```

### Task 3: Write the aarch64 scheduler selftest

**Files:**
- Create: `kernel/selftest/test_aarch64_scheduler.c`, plus the assembly probe it needs
- Modify: `kernel/selftest/selftest.c`, `kernel/Makefile` (aarch64 source whitelist)

**Interfaces:**
- Consumes: `kernel_thread()`, `create_kthread()`, `kthread_yield()`, `schedule()`, `kthread_exit()`, `blocker_wait()`, `task_wake()`.
- Produces: exported `void test_aarch64_scheduler_run(void);` called explicitly by the post-handoff C continuation. Registration does not dispatch or execute tests.

- [ ] **Step 1: Define the case list against the spec's QEMU table**

The spec (§10.2) lists the behaviours the aarch64 acceptance must observe. The kernel-side selftest supplies the deterministic subset it can observe in-kernel:

| # | Case | What the kernel must observe |
|---|---|---|
| 1 | two-thread alternation | each of two threads performs at least 10,000 yields; both counters reach the target; real cross-task switches occurred |
| 2 | register/stack probe | an assembly probe checks `x19–x29` and SP around a yield; per-thread stack sentinels survive; two threads use different patterns |
| 3 | first entry | `fn(arg)` ran exactly once; ABI stack alignment held; IRQ enabled; `current` correct; `in_schedule == 0`; `on_cpu == 1` |
| 4 | IRQ state | a yield entered with IRQ on and with IRQ off restores the same state; both the switch and the no-switch paths are covered; first entry does not inherit the parent's masked state |
| 5 | blocker | the four timings of §8.2; the blocker is cleared at the end; no double enqueue; a timeout fails the case |
| 6 | reclaim | at least 1,000 threads created and exited in batches; the idle thread-table count and runqueue count return to baseline; a dedicated allocation ledger for stack/thread/FPU objects nets to zero |
| 7 | idle resume | a test worker blocks, BSP idle is actually observed with idle outside the tree, and timer/condition recovery resumes the worker within 10 ticks |
| 8 | timer/IRQ pressure | at least 3 real CNTP ticks during the thread probe, IRQ frame roundtrip intact, no nested schedule and no IRQ-originated switch |
| 9 | AP isolation | real CPU count, all workers on CPU0; each AP scheduler_ok=0 and nr_running=0; repeat real AP work/shootdown after publication (>=2 CPUs) |

Map rows 1–9 to the exact ordered IDs and `[sched-evidence]` fields in the index. All state checks (idle tree, wake ticks, AP runqueues, next-stack reclamation, IRQ frame) happen inside the kernel; Phase 5 validates those complete observations and protocol outcomes. A PASS alone is insufficient evidence. Emit each evidence record immediately before its corresponding `selftest_record_late(id, rc)`.

- [ ] **Step 2: Implement the register/stack probe**

The probe must be assembly: it records `x19–x29` and SP, calls the yield, and compares. Write it as a small `.S` file with a C-callable entry, using the aarch64 `thread_t` offset constants from Phase 1 where relevant.

The two threads must use *different* sentinel patterns, so a stack mix-up is detectable. The C-callable probe first saves its caller's x19–x29/x30 on a 16-byte aligned frame, then installs sentinels, calls kthread_yield repeatedly, checks all registers/SP and stack canaries, and finally restores the caller's original callee-saved registers before returning its status. Do not overwrite the caller ABI while testing the scheduler ABI.

- [ ] **Step 3: Implement the case bodies**

Each case is a function returning 0 on success and non-zero on failure, with a diagnostic identifying the failing case. Use the `log_*` functions for diagnostics; the aarch64 log backend cannot format `%d`.

Use OS01_SELFTEST-only observational hooks at the real production events: context commitment, first-entry pre-C checks, blocker installation/pre-switch, allocation/free, idle entry, and timer entry/exit. Hooks may record counters or trigger the deterministic test condition, but never replace creation, wake, scheduling, IRQ dispatch or freeing. Non-selftest builds compile out all hooks.

For blocker case 5, control four separate subruns: condition true before wait; a peer observes waiter `on_cpu=0` before explicit wake; a one-shot pre-schedule hook sets the condition and invokes the production wake after blocker installation but before switching; scan-only recovery changes the condition WITHOUT explicit wake. Check actual switch counts, blocker clearing, and exactly one runqueue membership. Give each subrun a finite jiffies deadline.

For reclaim case 6, tag the cohort and record real stack/thread/FPU allocation and free events (including rollback) in the common constructor/tail; increments from the test's own calls alone are insufficient. The tail records `actual_prev != current` and current SP outside actual_prev's union BEFORE free, yielding `next_stack=1`. Wait until every created task's final tail/free is observed before sampling net-zero ledger/list/rq deltas. Use stable test-owned completion counters, never dereference a task pointer after publication/free. Slab totals are supplemental evidence.

For idle case 7, the driver itself is idle, outside the runqueue: create a worker that blocks, observe actual idle-loop entry with the worker blocked, then let a timer-driven condition/scan wake it. Measure <=10 CNTP ticks from condition availability to resumed worker; calling schedule repeatedly without seeing idle is insufficient. Return to the test driver via a bounded idle pump using the production idle step, not a test-only scheduler.

For timer case 8, a test-only IRQ observer snapshots registers at the existing assembly save/restore boundary, checks saved/restored frame fields using the existing IRQ probe technique, and observes >=3 CNTP ticks while the sentinel workers yield. A counter around the real switch commitment plus ISR-depth records proves zero IRQ switches and no nested schedule. Do not invoke the IRQ handler directly to manufacture evidence.

For AP case 9, snapshot each real AP scheduler_ok/runqueue state through safe existing per-CPU access; after CPU0 publication rerun the existing real AP work/TLB shootdown probe and report its observed completion per AP. Never read `current` on an AP work stack. At one CPU omit AP records and retain only the existing shootdown SKIP; all nine scheduler cases still PASS.



- [ ] **Step 4: Register the late case**

In the aarch64 branch of `selftest_register_builtin_tests()`, declare all nine index IDs with `selftest_register_late()` in manifest order. Implement `test_aarch64_scheduler_run()` to execute each real body and call `selftest_record_late(id, rc)` once, even when a prior case failed safely; fatal errors halt. A declaration does not call the body. Preserve all x86 registrations and expected counts.

Tasks 3 and 4 form one integration change: registration, expected-late=9, driver call and END movement must land together; do not commit a declared-but-unrun image between them.



- [ ] **Step 5: Add the source to the aarch64 whitelist**

`kernel/selftest/test_aarch64_scheduler.c` and its assembly probe go into the aarch64 branch of `KERNEL_C_SOURCES` in `kernel/Makefile`. The assembly file must be picked up by the arch assembly wildcard — check how `kernel/arch/aarch64/**/*.S` files are collected (the wildcard covers `$(ARCHDIR)/$(dir)/*.S` for each dir in `ARCH_SOURCE_DIRS`) and place the probe where it is collected, or extend the collection.

- [ ] **Step 6: Build aarch64**

```bash
make PROFILE=aarch64-clang clean && make PROFILE=aarch64-clang KERNEL_SELFTEST=1 aarch64-uefi-kernel
```

Expected: compiles and links.

- [ ] **Step 7: Commit**

```bash
git add kernel/selftest/test_aarch64_scheduler.c kernel/selftest/selftest.c \
        kernel/Makefile kernel/arch/aarch64/
git commit -m "test(aarch64): add scheduler selftest covering yield, blocker and reclaim"
```

### Task 4: Move the aarch64 coordinator END so it covers the scheduler case

**Files:**
- Modify: `kernel/arch/aarch64/boot/main.c`
- Modify: the aarch64 harness parser(s) and their unit tests, if the trace shape changes

**Interfaces:**
- Consumes: Task 3's `test_aarch64_scheduler_run()`.
- Produces: a protocol-v1 trace where all nine late IDs are executed and recorded before the single END.

- [ ] **Step 1: Determine where the coordinator's END currently lands**

`kernel/arch/aarch64/boot/main.c` calls `selftest_begin_run(0)` / `selftest_run_all()` / `selftest_end_run()` early, before the scheduler is live. The scheduler case runs later, after the handoff, so the END must move to after it.

The spec (§10.2) is explicit: the case must be registered and recorded *before* END, END must occur exactly once, and **no second self-declared-success protocol may be added after the boot main has already emitted END**. Adjusting the existing END position is allowed; adding a parallel protocol is not.

- [ ] **Step 2: Restructure the aarch64 selftest flow**

Set `selftest_begin_run(9)` before the unchanged early test run. Remove the old early END. In `arch_aarch64_bsp_handoff()` (task_arch.c), AFTER returning sched_boot_start and enabling IRQ, explicitly call `test_aarch64_scheduler_run()` under OS01_SELFTEST, then call `selftest_end_run()` exactly once, then enter idle. Add the real declaration in the matching `kernel/include/selftest/` header. Ordinary non-selftest boot enters idle directly. Update Task 3/4 file lists and stage task_arch.c/header in their joint commit.

Every existing pre-SMP and post-SMP case record must be preserved — same ids, same order, same count. The spec requires this explicitly.

- [ ] **Step 3: Check the old parser regression**

The aarch64 harnesses parse the protocol trace. If the trace shape changed (the END moved later, the late count changed), the parsers and their unit tests must be updated — the spec requires the old parser regression be updated, not skipped.

Find the parser and its tests:

```bash
rg -n "selftest|END|late" qemutests/aarch64_*.py qemutests/test_aarch64_harness.py
```

Update the expectations to the new trace shape, and add a case that fails if a second END appears or if any of the nine scheduler IDs is missing.

- [ ] **Step 4: Build and run the aarch64 selftest image**

```bash
make PROFILE=aarch64-clang clean && make PROFILE=aarch64-clang KERNEL_SELFTEST=1 aarch64-uefi
make PROFILE=aarch64-clang test-aarch64 MODE=smp
```

Expected: the harness accepts the new trace shape; all nine scheduler cases are reported.

- [ ] **Step 5: Commit**

```bash
git add kernel/arch/aarch64/boot/main.c qemutests/
git commit -m "test(aarch64): move selftest END to cover the scheduler case"
```

### Task 5: Two-arch regression gate

**Files:** none (verification only)

**Interfaces:**
- Consumes: Tasks 1-4.
- Produces: the gate evidence Phase 5 needs.

- [ ] **Step 1: Run the x86 gates**

```bash
make PROFILE=x86_64-clang test-host
make PROFILE=x86_64-clang test-static
make PROFILE=x86_64-clang KERNEL_SELFTEST=1 test-kernel-selftest
```

Expected: green. `kthread_yield()` is new but unused on x86 unless a caller was added — if a caller was added, the x86 selftest suite must still pass.

- [ ] **Step 2: Run the aarch64 selftest at several CPU counts**

```bash
make PROFILE=aarch64-clang test-aarch64 MODE=smp
```

Expected: PASS, and all nine scheduler cases reported as PASS exactly once.

- [ ] **Step 3: Run the harness regression**

```bash
make test-harness
```

Expected: PASS, including the updated aarch64 parser tests.

- [ ] **Step 4: Write the phase report**

Record per command: exact command line, exit code, CPU count, log path. Record the §8.2 timing analysis from Task 2 — including the cases that were already correct. List anything not run.

---

## Self-Review

- **Spec coverage**: §5.3 (yield wrapper and IRQ state — Task 1), §8.1 (creation via the production path — Task 3 uses `kernel_thread`/`create_kthread`), §8.2 (blocker and wake — Tasks 2 and 3 case 5), §8.3 (exit and reclaim — Task 3 case 6), §10.2 (the kernel-side half of the case table — Task 3; the harness half is Phase 5). §6 (BSP handoff) and §7 (AP isolation) are Phase 3.
- **Step scan**: each step names one edit or one command with an expected outcome. Task 2 Step 2 explicitly permits "no change needed" as a valid result with a required written analysis, so the step is not a disguised "fix the bug".
- **Type consistency**: `kthread_yield`, `kthread_exit`, `test_aarch64_scheduler_run`, the index's nine `aarch64_sched_*` IDs are spelled identically here and in the index.
- **Link check**: the Spec line in this plan links the series index as `2026-10-30-aarch64-el1-sched-series.md` — verified to resolve.
- **Proportion**: this plan implements §5.3's yield half, §8.2, §8.3, and the kernel side of §10.2's case table — roughly the size of those sections. The QEMU harness, the fault-injection variants, and the static audits are Phase 5.

## Execution Handoff

Phase 4 is complete when Task 5's gate is green and the report is written. The next plan is:

`docs/superpowers/plans/2026-10-30-aarch64-el1-sched-p5-acceptance.md`
