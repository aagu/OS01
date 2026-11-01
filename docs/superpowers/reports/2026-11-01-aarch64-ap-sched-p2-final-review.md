# aarch64 AP scheduler P2 — final review record

Date: 2026-11-01. Reviewer: independent `p2_final_review` (fresh context, read-only). Scope: completed-P1 baseline `fec562aa` through P2 T1–T4, including the takeover corrections. This record is maintained by the executor from the reviewer's returned findings.

## R1: REQUEST_CHANGES

| Severity | Finding | Resolution and evidence |
|---|---|---|
| Critical | `idle_wait.S` was ignored and absent from delivered Git tree | Explicit force-add; staged tree `acaf854313648676ccf8bcea93aad3c51d3a1d4f` contains blob `c612f55c6112250a506acef079430444e64f1a84`. A Git archive of that tree, extracted into `/tmp/os01-p2-sourcecheck-5hvjhaeg`, built ARM kernel from scratch and linked the helper. Only third-party dependency directories were supplied as source symlinks; kernel/runtime sources came from the archive. |
| Important | Final signal-resume callback ran after releasing task-list ownership | Compute interruption under task_list_lock, without rq_lock. Regression observed callback lock depth 0 (RED), then 1 (GREEN). |
| Important | Exit notification and later parent uses could dereference a reaped parent | New core operations accept the live child and resolve parent under task_list_lock; signal+blocker wake and final wake keep lifetime ownership. MM-sharing snapshot and detachment use the same ownership lock. Test reparents at lock acquisition and protects the old parent page PROT_NONE. |
| Important | Blocker state snapshot could lock the previous CPU's rq after migration | Revalidate owner under rq_lock and retry. Deterministic migration hook produces two rq acquisitions before callback; old code observed only one (RED). |
| Important | Lifetime/signal tests did not exercise the production transition | Actual scanner signal branch and clear-before-enqueue observation; post-unlock mprotect guard; two production helper calls. A deliberate dangling-read mutation causes SIGSEGV, restored source passes. Blocker host suite 71/71. |
| Important | Ordinary remote-wake could reuse the on-CPU switch handoff, and explicit copied completed | Wait for on_cpu=0 behind the worker lifetime barrier; count explicit calls at the actual verified operation independently of completions. Controlled wake-switch remains on_cpu=1. |
| Important | EOIR final value could not detect duplicate identical writes | Dedicated production-driver host build spies on MMIO writes, asserting counts and complete raw values, including spurious IRQ. Runtime EOI count now observes the store boundary. 7 assertions RED then 28/28 GREEN; duplicate-write mutation is rejected. |
| Minor | Private need-slot offset was not actually covered by the asserted definition | Removed the private literal; target SGI handler uses the real per-CPU C accessor. ARM build validates the accessor path. |
| Minor | New raw fatal logging, stale scan comments and closure docs | Operational fatal now uses log_err; scan comment corrected; closure docs finalized after gates/review. |

The executor also accepted the initial harness's CI-coverage failure: the new suite existed in the catalog without a CI workflow invocation. A real `MODE=sched-wake` full-matrix step was added; CI workflow tests pass 17/17. No coverage assertion was removed.

## R2: REQUEST_CHANGES → final APPROVE

The reviewer confirmed all R1 remedies, then found that the newly rq-protected zombie transition evaluated `this_cpu()` before masking IRQs. On x86 a timer-driven migration in that window could lock and unlock different runqueues. The executor extracted the actual production transition into `task_exit_zombie()`, masks IRQs first, caches one per-CPU pointer, and locks/unlocks through that pointer. A host test injects migration exactly at IRQ masking and records the lock address: unsafe implementation 74/76 (two failures), corrected implementation 76/76 PASS. Final ELF disassembly confirms `cli` precedes the sole `%gs:0` read and both lock operations use `0x50(%rcx)` (`/tmp/os01-p2-zombie-disasm.log`).

The same independent reviewer inspected the final change and returned **APPROVE**: “P2 整体代码审查通过，R1、R2 的阻塞项均已关闭。” Final product/harness evidence is recorded in the [validation report](2026-10-31-aarch64-ap-sched-p2-validation.md).


## Final-gate follow-up: x86 IST regression

After the initial APPROVE, final systest exposed an existing x86 nested-IRQ hazard during the user divide-by-zero case. The failed UP run is retained at `build/x86_64-clang/logs/tests/systest/20261101T090233Z-91c07fc74dce487ba42c61917d8ff7c3/`. A maskable IRQ returned to scheduling on IST1 before the fault handler reached `do_exit`; RSP masking produced an invalid task and null thread pointer. IRQ accounting also uses `current`, so guarding only the return path would leave memory corruption possible.

The reviewer reopened the completion gate and requested two companion corrections: restore-only returns for IST faults (including safe-MSR #GP), and saved-IF restoration only after the fatal-user task-stack pivot. The executor changed IST synchronous exceptions to interrupt gates while preserving DPL and IST; #PF and syscall remain unchanged. The common return selector reads the saved FUNC slot, permits exactly `do_page_fault` and `do_system_call` to use `ret_from_intr`, and routes all others to `ret_from_exception`. Fatal user cleanup restores only saved IF after switching stacks.

Source/ELF descriptor expectations and the actual runtime IDT selftest now require IRQ-masking gates for IST exceptions. The audit proves both allowed functions, the exact slot and conditional branches, and the fallback address; negative mutations cover each. Gate expectations first failed against the old source, and the selector audit rejected the old ELF. After correction, 87/87 audit tests and the actual ELF audit passed (`/tmp/os01-p2-ist-*.log`). The same reviewer inspected the rebuilt ELF and returned **APPROVE** again, confirming masking, both task-stack return paths, restore-only #GP, and conditional IF restoration after pivot. All six affected x86 gates passed, including all 99 syscall tests and SMP2/8 selftests. Final full-harness revalidation returned 0 (`ALL MODULES PASSED`, 489.27 seconds), as recorded in the validation report.

## Scope boundaries

The reviewer set aside physical WFI residency, AP timer preemption and EL0 behavior. The executor accepts these boundaries because P2 is explicitly cooperative EL1 kernel-thread scheduling, and WFI counters establish traversal of the wait path rather than physical power saving. These remain future work, not unreported failed features.

No meaningful findings were declined. No merge or push is part of this completion. The user's later instruction authorizes local Task4 history squashing only after completion, with a recovery ref and identical-tree verification.
