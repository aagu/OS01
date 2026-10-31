# aarch64 EL1 scheduler — final implementation review

Date: 2026-10-31. Branch: `feature/aarch64-el1-sched`.
Series base: `05620330`; reviewed implementation: `a6e7c3212e4be4c49c2d975381559a94d6be99ae`.
Validation addendum: `edd031cf` plus the controller artifact checks in
[the validation report](2026-10-30-aarch64-el1-sched-validation.md).

**Verdict: APPROVE for the specified EL1 BSP cooperative scheduler scope.**
P1–P5 implementation and review are complete. No Critical or Important
finding remains after the scoped re-review. This approval retains the
explicit x86 UP baseline exception below; it does not mean every gate
exited zero.

## Review and corrections

The controller reviewed constructor/publication/reclamation ownership,
CPU admission, x86 startup/resource hooks, ARM context and handoff,
selftest observations, and whole-series seams. The single Luna P5 agent
reviewed P5 acceptance and deferred findings, then implemented one
consolidated fix wave. The controller independently checked the committed
fix range `58cdd063..a6e7c321` and its regression evidence.

| Finding | Correction and evidence |
|---|---|
| Complete trace could pass after QEMU exited during the drain | Require the child to survive observation and be stopped by the runner. Lifecycle tests reject positive/fault natural exits with status 0 or 1, incomplete evidence, late fatal and timeout. |
| A published self-reaping kthread could disappear before notification read its CPU | Notify using the captured CPU value after unlock. Host test poisons the published task CPU at final unlock and proves no task-based notifier read occurs. |
| Locked placement revalidation failure leaked prepared FPU storage | Release and null FPU state, then drop files and allocations. Host injection proves zero residue and successful retry. |
| ARM tasks retained all-CPU affinity | Enforce CPU0 and mask 1 before publication. Live worker checks mask 1; an unpublished CPU1 task directly exercises backend rejection. |

Focused tests established RED on the old behavior and GREEN after the
fixes: lifecycle/parser 43 tests, constructor 161 assertions, and the
live ARM CPU0-affinity observation. The controller separately reran the
four lifecycle tests and inspected all nine changed source/test files.

## Validation disposition

The validation report records commands, CPU counts, revisions, logs and
artifact hashes, including these final-source results:

- AArch64 scheduler: 16/16 positive/fault boots, ordinary idle smoke at
  1/2/4/8 CPUs, and 7/7 injection-symbol audits. The controller rebuilt
  the ordinary-selftest ELF before the final symbol audit.
- x86 host: 152 distinct binaries passed, verified from their runner
  archives; static audits passed. Full harness: 855 tests, exit 0,
  one skip.
- x86 phase-0 and syscall systest: actual 1/2 CPUs passed; systest
  reported 99/99. Earlier `SMP=2`-only invocations actually used one
  CPU; `QEMU_SMP=2` reruns close the gap and the old table is corrected.
- x86 kernel-selftest: actual 2/8 CPUs passed 41/41. Actual 1 CPU
  reported 39/41, with exactly `fd_refcount` and `x86_intr` failing.
  `sched_boot_probe` passed at all three CPU counts.

The two UP failures are pre-existing structural guards requiring a peer
CPU. Their source files have no diff across the entire series; the
controller verified the exact failure set in fresh runner records.
They were neither suppressed nor converted to SKIP. Earlier M1,
sync-fault and other unrelated gate evidence remains historical;
these final fixes do not change those implementations.

## Remaining boundaries

The documented diagnostic-DTB workaround is still required with this
host's firmware/QEMU combination. No firmware redesign, EL0/user MM,
AP scheduling, IRQ preemption or FPU-state port is included.

Non-blocking prior observations remain bounded follow-ups: reclaim
ledger capacity if its 1000-thread cohort is enlarged, minor selftest
pointer/dead-store cleanup, and pre-existing GIC-SPI archive plumbing.
The current cohort is fully covered; no demonstrated production defect
is deferred by this approval.

The feature branch and worktree are preserved for integration. The
master checkout was not changed; no merge or push was performed.
