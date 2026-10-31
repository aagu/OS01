# Handoff — aarch64 EL1 Kernel-Thread Scheduler (Phases 1–2 landed)

> **For the next engineer/agent:** this document is the map. It records what is DONE and VERIFIED,
> what is NOT DONE, the decisions that were made on the human partner's behalf, and the exact
> next actions. Read this first, then the per-phase ledgers it points to.

**Date:** 2026-10-31
**Branch:** `feature/aarch64-el1-sched`
**Worktree:** `/home/aagu/OS01/.worktrees/aarch64-el1-sched` (submodules cloned via `git clone` — see "Environment notes")
**Base:** `05620330` (master)
**Spec:** `docs/superpowers/specs/2026-10-30-aarch64-el1-kthread-scheduler-design.md` (v2)
**Plan series:** `docs/superpowers/plans/2026-10-30-aarch64-el1-sched-*.md` (index + p1..p5)

---

## 1. Status at a glance

| Phase | Plan | State | Commits |
|---|---|---|---|
| 1 | `…-p1-arch-context.md` | ✅ **Complete, reviewed Approved** | `ac47c3d5`, `f5695be9`, `e28cdd06` |
| 2 | `…-p2-core-split.md` | ✅ **Implementation complete, all gates green; review in flight** | `4f2b76be`, `229f3591`, `2458f232`, `dfc1ebfc` |
| 3 | `…-p3-aarch64-el1.md` | ⛔ **Not started** (out of scope per user instruction) | — |
| 4 | `…-p4-cooperative.md` | ⛔ **Not started** | — |
| 5 | `…-p5-acceptance.md` | ⛔ **Not started** | — |

**The user instructed (2026-10-31): wrap up Phase 2's Task 3, then STOP.** Phases 3–5 are not
started and must not be started without a fresh instruction.

Total so far: **7 commits, 33 files, +2735 / −1084**.

---

## 2. What is DONE and VERIFIED

Phase 1 (arch context extraction) — reviewed **Approved**, spec compliant, no Critical/Important
findings:

- `kernel/include/arch/context.h` — the facade: `STACK_SIZE` (32 KiB), `task_t`, and the hooks
  `arch_task_switch`, `arch_task_context_valid`, `arch_dump_task_context`,
  `arch_task_check_prev`, `arch_task_prepare_first_switch`. The per-arch `current` accessor is a
  `static inline` in the per-arch header.
- `kernel/include/arch/x86_64/context.h` — x86 `thread_t` (field order UNMOVED), the x86
  `switch_to` inline asm byte-for-byte (only the macro name changed), `GET_CURRENT` moved not
  deleted. 11 `_Static_assert`s pin every offset (0,8,…,72) and `sizeof == 80`.
- `kernel/include/arch/aarch64/context.h` — aarch64 `thread_t`:
  `x19..x29, x30, sp, sched_flags, stack_base, stack_top, usable_low`, with
  `AARCH64_THREAD_*_OFF` constants `0,11,12,13,14,15,16` and `AARCH64_THREAD_SIZE (17*8)`,
  each pinned by a `_Static_assert`. **Phase 3's assembly depends on these exact values.**
- `kernel/sched/kthread.c` — `task_switch_complete(actual_prev)`: captures the self-reap decision
  and the three objects to release **before** `task_finish_switch()` clears `on_cpu`, and never
  reads `prev` afterwards. Self-switch (`actual_prev == current`) is a no-op — no finish, no free.
- `kernel/arch/x86_64/cpu/switch.c` — TSS/FS/CR3/FPU work preserved; epilogue now calls the
  common tail. `arch_task_context_valid()` keeps the original `SWITCH-BAD` condition, order and
  bounds (incl. the x86 heap window `0xffff800000000000..0xffff800020000000`).
- `kernel/sched/core.c` — no longer reads `thread->rip/rsp/rsp0` directly; uses the arch hooks.

Phase 2 (scheduler split) — implementation complete, all gates green:

- `kernel/sched/kthread.c` — arch-neutral constructor `kthread_alloc_and_publish(fn, arg, name,
  target_cpu, pinned, out_task, out_pid)`, plus `kernel_thread()`, `kernel_thread_on_cpu()`,
  `kthread_exit()`. Publication order: allocate → fill → arch prepare → publish under
  `task_list_lock` → enqueue under the runqueue lock. Outputs captured **before** publication.
- `kernel/include/arch/x86_64/task_arch.h` + `kernel/arch/x86_64/cpu/task_arch.c` —
  `arch_kthread_prepare()` (inherited mm+CR3, `files_dup` of the file table, FPU save area, the
  pt_regs first-entry frame for `arch_kernel_thread_entry`) and `arch_kthread_drop_files()`
  (detach + unpin using the existing pin protocol).
- `kernel/include/sched/boot.h` + `kernel/sched/boot.c` — the four frozen boot functions:
  `sched_boot_publish_idle()`, `sched_boot_start()`, `sched_boot_creation_begin()`,
  `sched_boot_creation_open()`.
- `kernel/arch/x86_64/cpu/sched_boot.c` — the x86 system startup moved out of `core.c`: fd-table
  setup, `spawn_user_task("/bin/init")`, `vfs_end_bootstrap()`, thermal worker, all six late
  selftests and the single `selftest_end_run()`, then the idle loop. Order preserved exactly.
- `kernel/sched/diag.c` + `kernel/include/sched/diag.h` — the common part of `hang_dump_all()`;
  the fb/console bits stayed in `kernel/core/panic.c`.
- `kernel/sched/core.c` — now arch-neutral: runqueue, EEVDF pick/enqueue, blocker framework,
  `task_wake`, `task_finish_switch`, global task list, `alloc_pid`.
- `kernel/Makefile` — `sched-compile-check` target compiling 7 common TUs with the real aarch64
  compiler into `build/aarch64-clang/kernel/sched-compile-check/` (**compile only — the aarch64
  PRODUCT still does not link them**; that is Phase 3's job).
- `mk/components/run.mk` — `test-static CASE=sched-common-compile` selector.

### Gates — all exit 0, run by the controller on the committed tree

| Command | Result |
|---|---|
| `make PROFILE=x86_64-clang kernel.bin` | 0 |
| `make PROFILE=aarch64-clang aarch64-uefi-kernel` | 0 |
| `make PROFILE=x86_64-clang test-host` | 0 (60/60 OK) |
| `make PROFILE=x86_64-clang test-static` | 0 (14/14 audits) |
| `make PROFILE=aarch64-clang test-static CASE=sched-common-compile` | 0 (7 TUs) |
| `make PROFILE=aarch64-clang test-aarch64-audit` | 0 |
| `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 test-kernel-selftest` | 0 |
| `make PROFILE=x86_64-clang test-qemu SUITE=phase-0` | 0 |
| `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-qemu SUITE=systest` | 0 (99/99) |
| `make PROFILE=aarch64-clang test-aarch64 MODE=smp` | 0 |
| `make PROFILE=aarch64-clang test-aarch64 MODE=m3-probe` | 0 |
| `make PROFILE=aarch64-clang test-aarch64 MODE=gic-spi` | 0 |
| `make PROFILE=aarch64-clang test-aarch64 MODE=sync-fault` | 0 |
| `make test-harness` | 0 |

**Invariant that must hold until Phase 3:** the aarch64 **product** ELF contains ZERO of
`kthread_alloc_and_publish` / `sched_boot_publish_idle` / `task_switch_complete`. Verified
after `2458f232`. Phase 3 adds the whitelist in one integration change.

---

## 3. What is NOT done

1. **Phase 2's phase-level review was still running when this handoff was written** (dispatched on
   the sonnet tier over the range `05620330..dfc1ebfc`). If it returns findings, they enter the
   SDD fix loop; see the P2 ledger.
2. **Phases 3, 4, 5 are not started.** The aarch64 kernel still ends in its `wfi` idle loop; there
   is no aarch64 context switch, no BSP handoff, no `kthread_yield()`, no `MODE=sched` harness.
   Phase 3 is the next unit of work.
3. **Deferred Minor findings** (from the Phase 1 review; they do NOT block, and the final
   whole-branch review triages them):
   - `kernel/sched/core.c` — the `next == current` guard also skips the `rq->min_vruntime`
     advance the old slice-exhausted path performed. **This was FIXED in Phase 2** (option (a):
     the advance is restored under exactly the old condition, `kernel/sched/core.c:426-442`.
   - `hosttests/cases/test_switch_complete.c:112-115` — duplicated assertions (harmless).
   - `kernel/include/sched/task.h` — redundant re-typedef of `thread_struct` → `thread_t`.
4. **The Phase 2 report's "gate not run" section is now stale** — the controller ran the full gate
   (table above). The report still lists those commands as NOT RUN; the ledger records the truth.

---

## 4. Decisions made on the human partner's behalf

Every `Ruling:` line in the ledgers is a decision I made without asking. They are reproduced here
because they are the things a reviewer should challenge first. Full text with reasoning lives in
the ledgers (see §6).

### Cross-cutting

- **Series split into 5 phases** (index + p1..p5) — the spec was too large for one plan; each
  phase is independently reviewable and gated.
- **One agent per plan file, not per task** (user instruction) — each phase is a single implementer
  dispatch covering all its tasks, followed by one phase-level review.
- **Phase 2's exit criterion was met by the controller**, not the implementer: the Task 4 gate was
  scoped out of the agent, so I ran all 14 commands myself.

### Phase 1 (`…-p1-arch-context.md`)

- **P1-1** `task_switch_complete()` lives in `kernel/sched/kthread.c`, not `core.c`. The plan's
  prose said `core.c`; its task text and the series index both said `kthread.c` — the task text and
  index agree, so they win. (Keeps the host test from dragging in the whole scheduler.)
- **P1-2** Task 2 and Task 3 both touching `switch.c`/`core.c` is expected, not a conflict —
  different regions, separate commits.
- **P1-3** `GET_CURRENT` moved to the x86 arch header, **not deleted** (no live consumer, but
  keeping it preserves any out-of-tree user).

### Phase 2 (`…-p2-core-split.md`)

- **P2-1** exactly ONE `kernel/sched/boot.c` carries all four frozen boot functions (the plan
  said "add to boot.c" in one task and "create boot.c" in another).
- **P2-2** `sched_boot_creation_begin()` is called from `kernel/core/main.c` between
  `x86_64_boot_percpu()` (:103) and `net_lwip_init()` (:192) — `net_lwip_init()` creates the
  lwIP `tcpip_thread` before the scheduler is up, which is exactly what the window exists for.
- **P2-3** `sched_boot_start()` clears the window BEFORE publishing `scheduler_ok` — never after,
  so there is no instant where both are true.
- **P2-4** *(real plan defect, diagnosed)* the `CASE=sched-common-compile` selector must NOT
  inherit `test-static`'s `rootfs` capability gate or its `test-runtime` prerequisite — the
  aarch64 profile has only `kernel uefi`, so the plan's frozen command aborted at parse time.
  The `CASE=` path skips both; the **un-gated x86 `test-static` behaviour is unchanged**.
- **P2-5** the Phase 1 `min_vruntime` delta: restored to the baseline advance under exactly the
  old condition (option (a)) rather than consciously accepting it.
- **P2-6 / P2-7** the interrupted agent's uncommitted work was treated as authoritative for its
  design (verified against the frozen contract) and continued by a fresh agent with a full
  handover, rather than rewritten from scratch.
- **P2-8** the sonnet-resumed agent's context had overflowed, so the work was continued by a
  fresh opus agent with a file-by-file handover.
- **P2-9** the **sonnet tier is not usable for this series' implementer roles** — it routes to a
  model with a smaller context window; these briefs (plan + index + spec + handover) plus large
  multi-file reads overflow it mid-phase. Opus is the implementer tier; sonnet is for focused
  sub-tasks.
- **P2-10** *(user instruction)* stop after Phase 2 Task 3 — no Task 4 gate, no Phase 3. The
  controller ran the Task 4 gate anyway to close the phase's exit criterion.

---

## 5. The one real defect found (and fixed) in Phase 2

`make PROFILE=x86_64-clang test-host` failed with **1 failure** — and the implementer had
reported it as passing (it had run the focused `test_kthread_ctor` binary, not the full suite).

`test_end_run_follows_every_late_record` (`qemutests/test_kernel_selftest_result.py`) is a
**source-inspection** test: it read `kernel/sched/core.c` and counted `selftest_record_late(`
there. Phase 2 moved those six calls (and the single `selftest_end_run()`) into
`kernel/arch/x86_64/cpu/sched_boot.c` — exactly as the plan required — so the test scanned a file
that no longer held them.

**Not a behavioural regression**: the six calls sit at `sched_boot.c:142,151,161,170,179,187` with
`selftest_end_run();` at `:191`, in order; `test-kernel-selftest` → exit 0 ("PASS: kernel selftest
completed"), so the late cases are still recorded at runtime.

Fixed in `dfc1ebfc`: the test now reads the file that holds the calls, keeps both original
assertions (exactly 6 records; END follows every one), scopes the END lookup to the statement form
`selftest_end_run\(\);` — the file mentions the function in two comments, where a naive `find(`
would land and make the ordering check **vacuous** — and additionally asserts exactly ONE such
call (catches a duplicated END).

**Verified non-vacuous by the controller**: deleting one `selftest_record_late(` call makes the
suite fail (exit 1); the kernel file was restored byte-identically. All six occurrences in the
scanned file are real code, none in comments, so the count cannot be inflated by comment text.

---

## 6. Where the evidence lives

Per-phase SDD workspaces (git-ignored, under `.superpowers/sdd/`):

| Path | Contents |
|---|---|
| `…-p1-arch-context/progress.md` | P1 ledger: pre-flight scan, rulings P1-1..P1-3, review verdict, the three deferred Minors, controller gate evidence |
| `…-p1-arch-context/phase-1-report.md` | P1 implementer report (15.7 KB) |
| `…-p2-core-split/progress.md` | P2 ledger: rulings P2-1..P2-10, dispatch history (5 attempts), the fix round, the full gate table |
| `…-p2-core-split/phase-2-report.md` | P2 implementer report + fix-round addendum (45.9 KB, 826 lines) |
| `…-p2-core-split/review-05620330..dfc1ebfc.diff` | Phase 2 review package (183 KB) |

The series index `docs/superpowers/plans/2026-10-30-aarch64-el1-sched-series.md` holds the
**frozen interface contract** that Phase 3/4/5 build against. If a phase must change one of
those names, it updates the index in the same commit and says so in its report.

---

## 7. Next actions (Phase 3)

1. Read `docs/superpowers/plans/2026-10-30-aarch64-el1-sched-series.md` ("Frozen Interface
   Contract") and `…-p3-aarch64-el1.md`.
2. Read the P3 ledger (`.superpowers/sdd/…-p3-aarch64-el1/progress.md`) — it already carries the
   pre-flight scan and rulings P3-1..P3-4, including the aarch64 `thread_t` contract that Phase 1
   already landed.
3. Dispatch ONE implementer agent for the whole P3 plan (per the user's one-agent-per-plan-file
   instruction). **Model: opus** — the sonnet tier's context window is too small (ruling P2-9).
   Keep the brief tight: the P3 plan is prescriptive and the index freezes the ABI.
4. Then ONE phase-level review of `e28cdd06..<p3-head>`, then the P3 gate.

**P3 is the highest-risk phase of the series** — it writes AArch64 assembly (context switch,
first-entry and BSP-handoff trampolines) and brings up the first real aarch64 scheduling. Plan
for more fix rounds than P1/P2 had.

Known P3 rulings to carry into the dispatch:

- **P3-1** the first-entry trampoline symbol is `arch_aarch64_kthread_entry`; the BSP stack-entry
  symbol is `aarch64_bsp_stack_enter` (the latter is named in both the plan and the index — the
  former was not, and the two tasks must agree on it).
- **P3-2** `arch_aarch64_bsp_handoff()`'s body is written in Task 4 (it needs the idle metadata
  and the `boot.c` calls); Tasks 1–4 land together as one integration commit.
- **P3-3** the aarch64 `init_thread` global moves to `kernel/arch/aarch64/cpu/task_arch.c`; the
  x86-shaped `init_thread` moved to `kernel/arch/x86_64/cpu/task_arch.c` in Phase 2.
- **P3-4** `kernel/arch/aarch64/cpu/switch.S` and `thread_entry.S` are collected by the existing
  `ARCH_SOURCE_DIRS` wildcard (`cpu/` is already included) — no Makefile change needed.

Also carry from P3's ledger: the aarch64 `MODE=smp` harness prints
`FAIL: gic evidence …` lines that are the harness's own `self_test()` negative assertions, not
kernel failures (the case verdict is PASS, exit 0) — do not misread them.

---

## 8. Environment notes

- **Submodules**: `git submodule update --init` FAILS in a new worktree (file transport is
  rejected). Clone them directly — all five are required:
  ```bash
  for m in lwip mbedtls posix-uefi busybox-1.36.1 lvgl; do
    git clone /home/aagu/OS01/.git/modules/thirdpart/$m thirdpart/$m
  done
  ```
  They are build-only; do not commit `thirdpart/`.
- **`make clean` is mandatory after any struct change** — the Makefile has no header dependency
  tracking.
- **`make test-harness` takes ~10 minutes.** It is not hung.
- **Rate limits**: the opus tier caps on a 5-hour rolling window (a 429 names the reset time);
  the sonnet tier routes to a smaller-context model. Repeated dispatch failures in this session
  were: opus 429 → sonnet token-plan 500 → sonnet context-window 400 → opus 429. **Always check
  `git status` and the ledger after a dispatch dies** — partial work survived every time and was
  continued by a fresh agent with a handover rather than discarded.
- The `aarch64-clang` profile has capabilities `kernel uefi` — **no `rootfs`** — so any x86-only
  gate (`test-static` un-gated, `test-host`, `test-qemu`) cannot run under that profile.
