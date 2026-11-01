# aarch64 AP scheduler P2 — validation

Date: 2026-11-01 (Asia/Shanghai). Worktree: `/home/aagu/OS01/.worktrees/aarch64-ap-sched`; branch: `aarch64-ap-sched`.

Status: **P2 complete**. Final 18 product gates and full harness PASS; independent whole-P2 and IST follow-up reviews **APPROVE**.

## Scope and handoff

P1 finished before this phase (`fec562aa`, followed by final-review correction `3a06077c` before P2 T1). P2 T1–T3 were committed as `131cdb75`, `b6241b01`, `bcf648cd`; T4 was handed over at WIP `7ce8c14e` with handoff document `4f30df3e`. Codex resumed that worktree without recreating completed tasks. Product gate runs identify the dirty `4f30df3e` tree containing the final source changes; the eventual task commit preserves these changes.

The result remains cooperative EL1 kernel-thread scheduling. SGI4 wakes idle CPUs and requests scheduling; it does not preempt an uncooperative running thread. SGI3 remains TLB shootdown. AP timers, migration, EL0/user MM and IRQ-return preemption remain outside this phase.

## Defects found during takeover

- The UP fallback loop started at CPU1 and never tested CPU0. Its failing envelope caused the runner to wait for success until its deadline. The new fixture runs 100 real UP scan wakeups. A UP worker returns after completion rather than waiting on a controller-owned exit flag that the idle controller cannot set while a runnable worker repeatedly yields to itself.
- Production `task_wake()` did not invoke the architecture notification after remote enqueue. A real-core host regression failed three new assertions, then passed after using the captured target with `sched_notify_remote_cpu()` outside rq_lock.
- Blocker state snapshots lacked rq_lock despite rq-only writers. The callback/lock trace regression failed 1/44, then passed 44/44 after adding rq-protected snapshots; callbacks continue under task_list_lock without rq_lock.
- WIP wake evidence omitted inherited P1 cases, aggregated AP results, used fixed/reused work seq values and counted 100 after-wait completions for a single operation. T4 now uses per-CPU observations, one operation per counted completion, retained seq+1, real one-shot ready overlap, and all 13 late case IDs.
- The parser formerly accepted phase=ap case lines inside a wake envelope. It now enforces matching phases, all inherited cases, every AP, exact fields, valid CPU ranges, UP NA shape and one START/END.
- The wait-helper audit previously exercised only simulated disassembly. The runner now audits the prepared ELF with llvm-objdump and verifies wake-hook symbol isolation. A real tab-separated mnemonic/operand fixture caught and corrected the old parser's splitting error.

## Focused verification

- Initial real-core blocker tests: 44/44 PASS (`/tmp/os01-p2-state-green.log`); both relevant assertions were watched fail before the fixes. Review expanded this to 76/76, including signal ownership, guard-page lifetime, migration revalidation, parent reparenting and IRQ-before-percpu ordering (`/tmp/os01-p2-zombie-order-green.log`). Dangling-read and duplicate-EOI mutations were rejected before restoration.
- AP parser/audit plus boot contract: 56 unittest cases PASS (`/tmp/os01-p2-audit-green.log`).
- Initial fresh QEMU smoke at 1/2/4/8 CPUs passed the strict phase=wake parser (`/tmp/os01-wake-smoke-<N>/stdout.log`). These do not replace the clean final matrix below.
- Scenario catalog regression revealed missing P2 host mappings; all three P2 host tests are now mapped, with the unchanged inventory-completeness check kept active.

## Evidence protocol

Each boot inherits P1 ready/entry/yield/reap/work/admission/default-mask and adds six wake cases. Each AP independently proves 100 explicit wakeups with scan=0, 100 on-CPU switch-window wakeups with 100 actual finish-switch enqueues, 100 arrivals at each of three idle boundaries, one real pre-ready work plus 200 post-ready work operations, and 100 fallback scan wakeups. UP reports the four cross-CPU cases as explicit zero/NA and still performs local scan and self-targeted SGI4. SGI trace counts actual entry/EOI and observes switch attempts while active; ACK generation advances through real tlb_shootdown.

All task fixtures keep an object alive while a waker reads it, then wait for actual observer free completion before resetting the next round. The before/after-check hooks stall without locks; the wait-boundary hook returns immediately. WFI entry/return counts prove traversal of the wait path, not physical power saving in QEMU.

## Final gate matrix

Both profiles were cleaned before the initial matrix (`/tmp/os01-p2-final-gates/results.json`). Review corrections introduced no struct changes. Final ARM results come from `/tmp/os01-p2-finalpass-gates/results.json`; affected x86 results after the IST correction come from `/tmp/os01-p2-ist-gates/results.json`. Every final command below returned 0. Full transcripts are in the corresponding `<gate>.log` directories.

| Gate | Final command | Exit |
|---|---|---|
| host | `make PROFILE=x86_64-clang test-host` | 0 |
| static | `make PROFILE=x86_64-clang test-static` | 0 |
| phase-0 | `make PROFILE=x86_64-clang test-qemu SUITE=phase-0` | 0 |
| systest | `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-qemu SUITE=systest` | 0 |
| selftest-2 | `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=2 test-kernel-selftest` | 0 |
| selftest-8 | `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=8 test-kernel-selftest` | 0 |
| arm-compile | `make PROFILE=aarch64-clang test-static CASE=sched-common-compile` | 0 |
| arm-audit | `make PROFILE=aarch64-clang test-aarch64-audit` | 0 |
| arm-sched | `make PROFILE=aarch64-clang test-aarch64 MODE=sched` | 0 |
| arm-smp | `make PROFILE=aarch64-clang test-aarch64 MODE=smp` | 0 |
| arm-m3-probe | `make PROFILE=aarch64-clang test-aarch64 MODE=m3-probe` | 0 |
| arm-gic-spi | `make PROFILE=aarch64-clang test-aarch64 MODE=gic-spi` | 0 |
| arm-sync-fault | `make PROFILE=aarch64-clang test-aarch64 MODE=sync-fault` | 0 |
| arm-noack-build | `make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 aarch64-uefi` | 0 |
| arm-noack | `make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 test-aarch64 MODE=no-ack` | 0 |
| arm-ap | `make PROFILE=aarch64-clang test-aarch64 MODE=sched-ap` | 0 |
| arm-fatal | `make PROFILE=aarch64-clang test-aarch64 MODE=sched-ap AP_SCHED_CASE=fatal-owner` | 0 |
| arm-wake | `make PROFILE=aarch64-clang test-aarch64 MODE=sched-wake` | 0 |

The earlier finalpass systest returned 2 and is **not** counted as PASS. Its UP user-#DE failure led to the IST correction described in the review report. The corrected run completed all **99 syscall tests**; x86 SMP2/8 selftests and the production interrupt audit also passed. The initial harness failed because sched-wake was missing from CI; the workflow was corrected without relaxing coverage checks. The first complete corrected harness passed (`/tmp/os01-p2-fixpass-harness.rc=0`). The final full harness after the IST/audit changes also passed: `make PROFILE=x86_64-clang test-harness`, exit 0, 489.27 seconds, `ALL MODULES PASSED` (`/tmp/os01-p2-ist-harness.log`, `/tmp/os01-p2-ist-harness.rc`).

ARM sched-wake and sched-ap each passed 1/2/4/8 CPUs ×2 (8 boots); the older sched matrix passed 16 boots. The fatal-owner and remaining fault/smoke modes passed independently. Per-boot directories under `build/aarch64-clang/logs/tests/` retain the exact QEMU command, image/firmware/ELF SHA256, revision/dirty/profile, stdout/stderr and exit reason in `metadata.json` and stdout/stderr logs. The final ARM wake boot IDs are:

- CPUs=1, run=1: `20261101T090715Z-f99e0122bd6d4fc69347acfe4ae36376` — PASS
- CPUs=1, run=2: `20261101T090717Z-b0d02817352f422d973a15fdd48f5c1d` — PASS
- CPUs=2, run=1: `20261101T090719Z-7ce80050bad8449582d7fe3d0f7c8a97` — PASS
- CPUs=2, run=2: `20261101T090721Z-748fd4bc0071403faa10d17cd19c256f` — PASS
- CPUs=4, run=1: `20261101T090723Z-0cfa7456a7734950a9ee23731e062a82` — PASS
- CPUs=4, run=2: `20261101T090726Z-fdb6710f957e4e0883709a0cb278e97e` — PASS
- CPUs=8, run=1: `20261101T090728Z-13cf0017585745f89c1351afec816588` — PASS
- CPUs=8, run=2: `20261101T090732Z-5ff869c75e084e17b65dc20fcd619c8b` — PASS

All eight final wake boots used the same artifacts:

- `image_sha256`: `f45065483ea701ade6875b979f2c3f64c3acc730ee4f182438e2305c7fe29204`
- `elf_sha256`: `3d01e0efbe3dde2fb24f49ac4f4d15dfc649b088693e60dee13e884f2bd001a0`
- `firmware_sha256`: `9f1b69c2a50c5119312050c4931aa155b91e2d69539c7290c50ae58e02decc09`
- `revision`: `4f30df3e311dd379aac5e445e768ae4413d68f71`
- `dirty`: `True`

## Independent review

Independent read-only review covered the whole P2 range from `fec562aa`, including committed T1–T3. R1 and R2 returned REQUEST_CHANGES; all meaningful findings were accepted and fixed. The same reviewer initially returned **APPROVE**. The later x86 systest failure reopened the gate; after the IST correction, the reviewer inspected the actual rebuilt ELF and again returned **APPROVE**. All affected x86 product gates passed. See the [review record](2026-11-01-aarch64-ap-sched-p2-final-review.md) for each finding, resolution and negative-test evidence.

## Delivered-source and history checks

The final staged source tree `033bcb2959455fde6e0e770495b48e77d78c652f` was archived and extracted to `/tmp/os01-p2-delivery-r_nmd7me`. Cold builds of both `make PROFILE=aarch64-clang aarch64-uefi-kernel` and `make PROFILE=x86_64-clang kernel.bin` returned 0 (`/tmp/os01-p2-delivery-results.json`). Kernel/runtime sources came entirely from Git; third-party dependency directories were linked separately. This confirms the previously ignored `idle_wait.S` is delivered and the final x86 IST changes build from the recorded tree. Subsequent changes are completion documentation only.

History policy authorized by the user: fold Task4's WIP, handoff and finalization history into one formal commit with parent `bcf648cd`, preserving P1 and P2 T1–T3. Keep recovery branch `codex/backup-aarch64-ap-sched-p2-before-squash-20261101` and require identical Git trees before/after the rewrite. No merge or push is part of this completion.
