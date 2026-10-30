# Phase 5 — Strict aarch64 QEMU acceptance harness, negative variants, and the full gate

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the `MODE=sched` QEMU harness with strict evidence checking, the fault-injection variants that prove the negative paths, the static/host audits, and run the full two-architecture regression gate — so the feature can be claimed done on evidence rather than on intent.

**Architecture:** A new `qemutests/aarch64_scheduler.py` drives a dedicated `sched` kernel/image variant under QEMU and parses the protocol-v1 trace strictly: every expected scheduler case must appear exactly once as PASS, with no FAIL, no SKIP, and no fatal. Separate fault-injection variants inject a corrupt context so the fatal path is observed to fire *before* the stack switch. Static and host-side audits cover the build-level and allocation-level properties that a QEMU run cannot observe.

**Tech Stack:** Python (harness, unittest), QEMU `virt`, Make profile variants, host C unit tests, kernel protocol-v1 selftests.

**Spec:** [2026-10-30-aarch64-el1-kthread-scheduler-design.md](../specs/2026-10-30-aarch64-el1-kthread-scheduler-design.md) §10 (acceptance and evidence), §9 (diagnostics). Series index: [2026-10-30-aarch64-el1-sched-series.md](2026-10-30-aarch64-el1-sched-series.md).

## Global Constraints

All series constraints from the index apply. Additionally, in this phase:

- **`MODE=sched` is a new mode inside the existing `test-aarch64` bucket — not a new alias target.** The spec (§10.2) says "沿用现有 `test-aarch64` bucket，不增加别名". The mode selector must reject conflicts with `sync-fault` / M1 / fault / systest **at parse time**, not at run time.
- **The `sched` mode builds its own `KERNEL_SELFTEST` kernel/image variant.** The ordinary image must not contain the fault-injection symbols.
- **Timeouts never count as success.** If the expected evidence never appears, the harness exits non-zero even when QEMU terminated cleanly.
- **A `SKIP` is a failure for any scheduler case.** The 1-CPU shootdown SKIP that exists today stays legitimate; a scheduler-case SKIP does not.
- **The harness parses the existing report schema** (`qemutests/harness/result.py`) rather than inventing a new one, and the failure exit code is non-zero.
- **Evidence is per-case and per-CPU-count**, with the per-case QEMU log, the CPU count, the build variant, the test list, and the failure reason retained.
- **Every acceptance command must be recorded with its real exit code.** A gate that was skipped is recorded as skipped, with the reason.
- **The roadmap marks only "EL1 BSP cooperative thread scheduling" as done.** M4, SMP scheduling, IRQ-return preemption, and a unified `kernel_main` stay open.

---

## Review Focus

1. **A missing case must fail, not pass silently.** If the trace omits an expected case, or duplicates one, or reports them out of order, or reports a partial PASS, the harness must fail. A parser that only checks "some PASS appeared" would let a broken scheduler pass.
2. **The fault-injection variants must fail *before* the switch, not after.** A corrupt-SP variant that crashes *after* switching stacks has proven nothing about the validation. The negative case must show the fatal diagnostic and **no** PASS marker afterwards.
3. **The ordinary image must not contain the injection symbols.** An image that always injects a fault would make the positive modes meaningless. This is a build-level property — check the symbol table, not just the runtime behaviour.
4. **The parser must reject a truncated or partially-flushed trace.** A QEMU that died mid-run can leave a trace with a START and some BEGINs but no terminal record. That must be a failure, not a partial pass.
5. **The CPU-count matrix must actually run.** The spec requires every normal case at `-smp 1/2/4/8` at least once, with the stack/register, blocker, and reclaim stress cases repeated three times at 1 and 2 CPUs. A harness that silently skips a CPU count is a false pass.

---

## File Responsibilities

| File | Change | Responsibility after this phase |
|---|---|---|
| `qemutests/aarch64_scheduler.py` | create | The `MODE=sched` runner: build variant, QEMU invocation, strict protocol parsing, per-case evidence, report writing |
| `qemutests/test_aarch64_harness.py` | modify | Unit tests for the new parser: missing/duplicate/out-of-order/partial PASS, FAIL/SKIP, crash, timeout |
| `mk/components/run.mk` | modify | `MODE=sched` dispatch: prep + run helpers, mode conflict rejection, help text, test-list entry |
| `mk/profiles/aarch64-clang.mk` | modify | The `sched` kernel/image variant and the fault-injection variant selection |
| `kernel/` (fault-injection hooks) | modify | Compile-time-gated context corruption for the negative variants |
| `hosttests/cases/` | create/modify | Host-side audits: allocation fault injection at every failure point, placement coverage, layout assertions |
| `docs/build/build.md` | modify | Document the new mode, the variant, and the gate commands |
| `docs/roadmap.md` | modify | Mark only the EL1 BSP cooperative thread scheduling item done |

---

### Task 1: Add the `sched` build variant and the mode selector

**Files:**
- Modify: `mk/profiles/aarch64-clang.mk`, `mk/components/run.mk`, `mk/project.mk`, `mk/targets/aarch64.mk`, `kernel/Makefile`

**Interfaces:**
- Consumes: the aarch64 profile's existing variant mechanism (`KERNEL_VARIANT` and the `AARCH64_UEFI_*` image paths).
- Produces: `MODE=sched` accepted by `test-aarch64`, with a `sched` kernel/image variant and parse-time conflict rejection.

- [ ] **Step 1: Write the failing parse-time check**

The spec (§10.2) requires the mode selector to reject conflicts *at parse time*. Verify by running:

```bash
make PROFILE=aarch64-clang test-aarch64 MODE=sched AARCH64_SYNC_FAULT_TEST=1 -n
```

Expected: a parse-time error naming the conflict, not a runtime failure several minutes later.

- [ ] **Step 2: Add the variant selection to the profile**

Freeze the selector as `AARCH64_SCHED_TEST` with values empty (default), `positive`, `sp-top`, `sp-low`, `pc-low`, `pc-data`. Reject any other value. Nonempty requires KERNEL_SELFTEST=1. Positive selects KERNEL_VARIANT=sched; each fault selects sched-fault-<value>. Map the four faults to explicit numeric compile defines in kernel/Makefile; positive/normal/selftest have no injection define. Add AARCH64_SCHED_TEST to mk/project.mk's OS01_SUBMAKE_ALLOWED so controlled env-i kernel submakes receive it.

Each variant owns `$(BUILD_DIR)/kernel/<variant>/kernel.elf` and `$(BUILD_DIR)/image/<variant>/{aarch64-uefi.img,QEMU_EFI.fd}`. Add outer-make `AARCH64_UEFI_SCHED_*` paths in mk/targets/aarch64.mk, using the same selector-to-variant mapping (outer makes cannot infer a recursive make's KERNEL_VARIANT). Never run the ELF/artifacts symlink from another variant; record ELF/image hashes.

At parse time, both MODE=sched and a nonempty selector reject sync-fault/M1/forced-no-RNDRRS/canary/OS01_SYSTEST, and nonzero AARCH64_SMP_TEST_NO_ACK_CPU. Do this even when the outer MODE=sched invocation has no selector yet. The prep recipe explicitly builds `KERNEL_SELFTEST=1 AARCH64_SCHED_TEST=positive aarch64-uefi`; selected fault prep uses the corresponding value. Normal and ordinary selftest builds keep their existing paths and never inherit this selector implicitly.



- [ ] **Step 3: Add the mode dispatch to `run.mk`**

In `mk/components/run.mk`, `test-aarch64` dispatches on `MODE` to a `_test-aarch64-prep-<mode>` and a `_test-aarch64-run-<mode>` pair. Add the `sched` pair, following the `sync-fault` pair's structure (prep on its own recipe line so `make -n` honours the dry-run contract; the python invocation on its own line so it is not executed under `-n`).

Add `sched` to the mode case statement. Freeze optional `SCHED_CASE=positive|sp-top|sp-low|pc-low|pc-data|all` (default all), handled by MODE=sched prep/run only. `all` prepares the positive and four fault images with sequential controlled recursive makes, then passes explicit image/firmware/ELF path maps to one runner; no live-mode implicit rebuild. Reject invalid SCHED_CASE at parse time. Normal idle-smoke remains a separate runner mode from Phase 3.

- [ ] **Step 4: Add the help text and the test-list entry**

`mk/components/run.mk` has a help block listing the `test-aarch64` modes (`smp|no-ack|gic-spi|sync-fault|m3-probe|m1-*`) in at least two places — the `help` target and the test-bucket table. Add `sched` to both.

- [ ] **Step 5: Verify the parse-time rejection and the dry run**

```bash
make PROFILE=aarch64-clang test-aarch64 MODE=sched AARCH64_SYNC_FAULT_TEST=1 -n
make PROFILE=aarch64-clang test-aarch64 MODE=sched -n
```

Expected: first errors at parse time; second prints all prep/run recipes without executing QEMU or mutating outputs. Add harness tests for all selector values/conflicts, missing KERNEL_SELFTEST in direct variant builds, submake propagation, isolated resolved paths, and MODE=sched -n. Test these through make subprocesses with checkable exit status, not text-only fixtures.

- [ ] **Step 6: Commit**

```bash
git add mk/profiles/aarch64-clang.mk mk/components/run.mk mk/project.mk \
        mk/targets/aarch64.mk kernel/Makefile
git commit -m "build(aarch64): add sched image variant and MODE=sched dispatch"
```

### Task 2: Write the `MODE=sched` runner with strict evidence parsing

**Files:**
- Extend: `qemutests/aarch64_scheduler.py` (Phase 3 idle-smoke skeleton)
- Modify: `qemutests/test_aarch64_harness.py` (add the new test classes)

**Interfaces:**
- Consumes: `qemutests/harness/process.py` (`ProcessSession`), `qemutests/harness/result.py` (`RunArchive`, `RunReport`, `parse_v1`), the `sched` variant paths from Task 1.
- Produces: a runner with a `--self-test` mode (pure, no QEMU) and a live mode.

- [ ] **Step 1: Write the parser unit tests first**

Add test classes to `qemutests/test_aarch64_harness.py` covering exactly the cases the spec (§10.1, item 5) names:

- a missing expected case → rejected;
- a duplicated case → rejected;
- cases out of order → rejected;
- a partial PASS (the case is recorded but the terminal says fewer than expected) → rejected;
- a FAIL record → rejected;
- a SKIP record → rejected;
- a trace that ends without a terminal record (crash) → rejected;
- a timeout (no evidence at all) → rejected;
- a complete, correct trace → accepted.

Build the fixtures as synthetic protocol-v1 traces in the same style the existing aarch64 harness tests use (`valid_log`, `valid_log_alt_regs` in `qemutests/aarch64_uefi_smp.py`).

- [ ] **Step 2: Run the tests to verify they fail**

Run: `python3 -m unittest qemutests.test_aarch64_harness -v`.

Expected: FAIL — the parser does not exist yet.

- [ ] **Step 3: Implement the parser**

Use `parse_v1(text, suite="kernel-selftest", allowed_ids=EXPECTED_IDS)` for protocol shape, then require `.ok`, failed=0, skipped=0, and exact equality of selected/started/terminal ID sets to the frozen manifest, not just allowed_ids membership. Freeze early IDs in registration order: `slab_alloc_free`, `slab_many_sizes`, `slab_16_caches`, `aarch64_pt_vmm`, `spinlock_basic`; append the nine late IDs from the index, total=passed=14. This is independent of what START/END claim; a self-consistent reduced manifest fails. Require every terminal status PASS, including early cases. The existing one-CPU shootdown SKIP is a diagnostic outside this protocol, not permission to skip a scheduler/early case.

`parse_v1` does not enforce cross-case order: build an ordered event list from anchored [TEST] records, require SELECT order to match the manifest, and BEGIN/terminal pairs in manifest order with exactly one START/END. Reject duplicated/unknown IDs, reordered complete case blocks, missing terminal/END, extra END, coordinator ERROR diagnostics, any fatal/PANIC/structured arch failure anywhere. Scheduler evidence records must match every exact index field and predicate, appear once per case in order before its terminal, and include exactly cpu_count-1 unique AP observations (zero at one CPU). Missing evidence, duplicate fields, partial records or mismatched CPU count fail.

Extend negative fixtures with a structurally valid early FAIL, valid reduced manifest/count, swapped complete case blocks, unknown late record/coordinator ERROR, missing next-stack/timer/AP observation, and a fatal arriving after END. A valid protocol alone is insufficient.



- [ ] **Step 4: Implement the runner**

`qemutests/aarch64_scheduler.py` follows the shape of `qemutests/aarch64_uefi_smp.py`:

- `--self-test` runs the pure parser checks and exits;
- the live mode consumes the explicit paths prepared by make, boots QEMU with the existing per-CPU diagnostic DTB generator (`generate_diagnostic_dtb`), drains serial, applies the parser, and writes the existing RunReport schema via RunArchive; it never rebuilds a different variant;
- on any failure it exits non-zero and retains the per-case log, the CPU count, the build variant, the test list, and the failure reason;
- freeze a 120s absolute deadline per boot (ticks never reset it), plus the existing one-second post-completion drain within that deadline; crash/late fatal/timeout fail, and deliberate QEMU termination only occurs after complete evidence plus the drain;
- run the complete positive suite three times at CPUs 1 and 2, once at CPUs 4 and 8. Archive each repetition independently; early failure cannot silently skip remaining matrix entries (record failed/not-run explicitly). Preserve Phase 3 --idle-smoke CLI; make live selector and paths explicit via --case plus per-variant --image/--firmware/--elf, or a make-produced JSON path map with that same fixed schema.

The QEMU invocation must use the `sched` variant's firmware and image paths — the same mechanism the `sync-fault` mode uses for its isolated paths.

- [ ] **Step 5: Run the unit tests to verify they pass**

Run: `python3 -m unittest qemutests.test_aarch64_harness -v`.

Expected: PASS, including the new classes.

- [ ] **Step 6: Run the live mode**

```bash
make PROFILE=aarch64-clang test-aarch64 MODE=sched SCHED_CASE=positive
```

Expected: PASS, with a report written under the build's log directory.

- [ ] **Step 7: Commit**

```bash
git add qemutests/aarch64_scheduler.py qemutests/test_aarch64_harness.py
git commit -m "test(aarch64): add strict MODE=sched acceptance runner"
```

### Task 3: Fault-injection variants for the negative context cases

**Files:**
- Modify: the aarch64 profile variant selection, `kernel/arch/aarch64/cpu/task_arch.c` (the injection hook), `qemutests/aarch64_scheduler.py`

**Interfaces:**
- Consumes: the `arch_task_context_valid()` backend from Phase 3 Task 3.
- Produces: variants that inject a corrupt SP or a corrupt continuation, and harness cases that assert the fatal fires before the switch.

- [ ] **Step 1: Define the injection variants**

The spec (§10.2, last row) requires an independent injection variant that corrupts the saved SP/alignment or the entry continuation, covering:

- `SP == top`;
- `SP < usable_low`;
- an arbitrary low address as the continuation;
- a continuation in non-executable RAM.

Implement these as compile-time-selected injection points (the repo already has this pattern — `ARCH9_FAULT`, `FB_RESOLUTION_TEST`, `AARCH64_M1_TEST` — each a build variant with an isolated image path). Use Task 1's exact four selector values and sched-fault-<value> directories. Do not introduce a second selection mechanism.

The injection must be **compile-time gated** so the ordinary `sched` image contains none of the symbols. Verify with `llvm-nm` on the produced ELF, comparing against the ordinary `sched` build.

- [ ] **Step 2: Implement the injection hook**

In `kernel/arch/aarch64/cpu/task_arch.c`, select one dedicated victim kthread and corrupt its saved SP/PC immediately before the production pre-switch validator: top, usable_low-16, aligned unmapped low address, or the address of a non-executable data object. Guard the entire named `aarch64_sched_fault_inject` symbol with the fault define. Maintain a real switch-commit observation counter for that victim; rejection emits exactly `[sched-fault] case=<selector> phase=pre-switch target=<pid> switches_to_target=0`, followed by the matching production fatal reason. No victim first-entry is allowed. Print this only from the rejection path before the assembly call; a marker printed before attempting a switch proves nothing.

- [ ] **Step 3: Add the negative harness cases**

For each selected fault at 1 and 2 CPUs require the exact fault record, matching target/reason and one expected fatal; require zero committed switches to victim, no victim-entry record and no scheduler completion/END. A timeout, unrelated exception, wrong reason, or later crash is FAIL. Expected pre-switch rejection makes the **negative harness case PASS**; distinguish guest fatal from harness failure in report/exit code. Freeze the target-entry/commit observers in the fault build so a synthetic pre-switch marker followed by a committed transfer cannot pass. Observe for one second after the expected fatal; continuing execution or extra records fail.



- [ ] **Step 4: Verify the ordinary image is clean**

Build normal, ordinary selftest and positive sched images separately, then run the symbol audit with `subprocess.run([llvm_nm, elf], check=True, capture_output=True, text=True)`. Do not pipe nm into a no-match grep that masks nm failure. Paths are `build/aarch64-clang/kernel/kernel.elf`, `kernel/selftest/kernel.elf`, and `kernel/sched/kernel.elf` under that build root. All must lack exact symbol `aarch64_sched_fault_inject`; each `kernel/sched-fault-<selector>/kernel.elf` must contain it. Record path/hash for every audit and error on missing ELF or nm failure. Rebuild each variant after clean as necessary; never audit a deleted/stale artifact.



- [ ] **Step 5: Run each negative variant**

Run `make PROFILE=aarch64-clang test-aarch64 MODE=sched SCHED_CASE=all`. Require each negative harness case PASS with the expected pre-switch fatal; all wrong reasons/timeouts fail the overall command.

- [ ] **Step 6: Commit**

```bash
git add mk/profiles/aarch64-clang.mk kernel/arch/aarch64/cpu/task_arch.c \
        qemutests/aarch64_scheduler.py
git commit -m "test(aarch64): add fault-injection variants for corrupt context"
```

### Task 4: Static and host-side audits

**Files:**
- Create: `hosttests/cases/test_sched_alloc_faults.c` and the other new host cases
- Modify: `hosttests/Makefile` (register the binaries in `TEST_BINS`)
- Modify: `mk/components/run.mk` (extend the aarch64 audit target if needed)

**Interfaces:**
- Consumes: the arch-neutral constructor from Phase 2.
- Produces: the §10.1 static/host evidence.

- [ ] **Step 1: Cover the allocation fault-injection points**

The spec (§10.1, item 3) requires fault injection at **each** failure point of the raw-stack allocation, the thread allocation, and the arch resource prepare — and that after each failure there is no list, runqueue, or allocation residue, and that a subsequent normal creation succeeds.

Extend the Phase 2 rollback host test to cover every failure point, asserting the residue-free state and the successful retry after each.

- [ ] **Step 2: Cover placement**

The spec (§10.1, item 4) requires coverage of: an AP that is online but not `scheduler_ok`; an empty mask; an illegal CPU; a failed pin followed by an ordinary creation; the lock order; and the `on_cpu`/`on_rq` wake handoff.

Add a host case driving the real placement/boot-window functions: ordinary CPU0 creation is accepted only inside the explicitly opened boot window with scheduler_ok=0; pinned creation is rejected; publication permanently closes the exception; failed pin leaves no residue and ordinary retry succeeds. Host mocks prove state/rollback only.

For actual execution, add an OS01_SELFTEST x86 UP boot probe after x86_64_boot_percpu/prerequisites and before scheduler publication: call the real kernel_thread with a marker function while CPU0 scheduler_ok=0, capture success, then let the normal boot continue. An existing late kernel selftest confirms that exact function ran after scheduler_ok=1 (bounded completion flag, no test scheduler), and verifies the runtime/pinned negative calls. Register any new late ID and adjust x86 expected counts plus parser fixtures atomically; preserve existing IDs. Run this with `KERNEL_SELFTEST_SMP=1`; an object mock cannot substitute for the QEMU execution evidence.

- [ ] **Step 3: Cover the layout and offset assertions**

The spec (§10.1, items 1-2) requires: both architectures' common scheduler TUs compile; aarch64 has no x86 PTE/asm/user-MM link dependency and no success stub; the x86 thread layout has not drifted; and compile-time assertions cover every context offset, the task-union alignment, and the task_t/stack margin.

The compile-time assertions were added in Phase 1. Add a host case that checks the *x86* offsets against their recorded values, so a future drift is caught by a test rather than by an audit script that might be skipped.

- [ ] **Step 4: Extend the aarch64 symbol audit**

The audit target in `mk/components/run.mk` already rejects `vma_*` / `uaccess_*` / `fork_*`. Phase 3 integrated the real common whitelist; verify the audit covers `spawn_user_task`, `files_*`, `vfs_*`, `vma_free_all`, `vmm_free_user_map`, and `do_fork`, and add whatever is missing.

- [ ] **Step 5: Run the host and static gates**

```bash
make PROFILE=x86_64-clang test-host
make PROFILE=x86_64-clang test-static
make PROFILE=aarch64-clang test-aarch64-audit
```

Expected: green.

- [ ] **Step 6: Commit**

```bash
git add hosttests/ mk/components/run.mk
git commit -m "test: add scheduler allocation, placement and layout audits"
```

### Task 5: The full two-architecture acceptance run

**Files:**
- Modify: `docs/build/build.md`, `docs/roadmap.md`
- Create: the phase report under `docs/superpowers/reports/`

**Interfaces:**
- Consumes: everything from Phases 1-4 plus Tasks 1-4.
- Produces: the evidence that closes the feature.

- [ ] **Step 1: Run the aarch64 acceptance matrix**

The spec (§10.2) requires every normal case at `-smp 1/2/4/8` at least once, with the stack/register, blocker, and reclaim stress cases repeated three times at 1 and 2 CPUs, and a finite timeout that continuous ticks do not extend.

```bash
make PROFILE=aarch64-clang test-aarch64 MODE=sched
```

Confirm from the report that each CPU count actually ran. If the harness's CPU-count list does not cover 1/2/4/8, extend it — a silently-skipped CPU count is a false pass (Review Focus 5).

- [ ] **Step 2: Run the aarch64 negative variants**

Run `make PROFILE=aarch64-clang test-aarch64 MODE=sched SCHED_CASE=all`; all four negative harness cases must PASS at CPUs 1 and 2 only for their exact expected pre-switch fatal and zero victim commitment. The positive matrix also runs. No timeout counts as expected rejection.

- [ ] **Step 3: Run the aarch64 regression matrix**

```bash
make PROFILE=aarch64-clang test-aarch64 MODE=smp
make PROFILE=aarch64-clang test-aarch64 MODE=m3-probe
make PROFILE=aarch64-clang test-aarch64 MODE=gic-spi
make PROFILE=aarch64-clang test-aarch64 MODE=sync-fault
make PROFILE=aarch64-clang test-aarch64 MODE=m1-ram
make PROFILE=aarch64-clang test-aarch64 MODE=m1-sparse
make PROFILE=aarch64-clang test-aarch64 MODE=m1-arena-exhaust
make PROFILE=aarch64-clang test-aarch64 MODE=m1-table-exhaust
make PROFILE=aarch64-clang test-aarch64 MODE=m1-ap-bad-root
make PROFILE=aarch64-clang test-aarch64-audit
make PROFILE=aarch64-clang aarch64-uefi
python3 qemutests/aarch64_scheduler.py --idle-smoke --cpus 1 2 4 8 \
  --image build/aarch64-clang/image/aarch64-uefi.img \
  --firmware build/aarch64-clang/image/QEMU_EFI.fd
```

Expected: PASS. Plus the new ordinary-image BSP-idle smoke at 1/2/4/8 CPUs that the spec (§10.3) requires.

- [ ] **Step 4: Run the x86 regression matrix**

```bash
make PROFILE=x86_64-clang test-host
make PROFILE=x86_64-clang test-static
for cpus in 1 2; do
  make PROFILE=x86_64-clang SMP="$cpus" test-qemu SUITE=phase-0 || exit 1
  make PROFILE=x86_64-clang SMP="$cpus" OS01_SYSTEST=1 test-qemu SUITE=systest || exit 1
done
for cpus in 1 2 8; do
  make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP="$cpus" test-kernel-selftest || exit 1
done
```

The kernel selftest must run at 1/2/8 CPUs, covering `kernel_mutex`, `kthread_self_reap`, `fd_refcount`, `thermal_affinity`, and the blocker cases.

- [ ] **Step 5: Run the harness regression**

```bash
make test-harness
```

Expected: PASS, including the new parser tests.

- [ ] **Step 6: Update the documentation**

`docs/build/build.md` must document: the new `MODE=sched` mode in the target taxonomy, the `sched` variant, the conflict rules, and the new gate commands.

`docs/roadmap.md` marks **only** "EL1 BSP cooperative thread scheduling" as complete. M4, SMP scheduling, IRQ-return preemption, and the unified `kernel_main` stay open. The roadmap entry must link the validation report and state the evidence.

- [ ] **Step 7: Write the validation report**

Create `docs/superpowers/reports/2026-10-30-aarch64-el1-sched-validation.md` with, per command: the exact command line, the exit code, the CPU count, and the log path. Record any failure observed on an unrelated path as a **separate baseline entry** — the spec (§9) forbids calling the whole suite green because a new case passed.

- [ ] **Step 8: Commit**

```bash
git add docs/build/build.md docs/roadmap.md docs/superpowers/reports/
git commit -m "docs: record the EL1 scheduler validation evidence and roadmap status"
```

---

## Self-Review

- **Spec coverage**: §10.1 (static/host audits — Task 4), §10.2 (the QEMU case table, the mode, the variant, the negative cases — Tasks 1-3), §10.3 (the full regression gate — Task 5), §9 (diagnostics, and the "record unrelated failures separately" rule — Task 5 Step 7). §11's "delivery must include code, test wiring, a validation report, and documentation updates" is Task 5 Steps 6-8.
- **Step scan**: each step names one edit or one command with an expected outcome. Task 3 Step 1 enumerates the four injection cases from the spec rather than saying "add fault injection".
- **Type consistency**: `MODE=sched`, `sched`, `sched-fault-<selector>`, the nine `aarch64_sched_*` IDs, `aarch64_scheduler.py`, `test_aarch64_harness.py` are spelled identically here and in Phase 4.
- **Resolved selection**: AARCH64_SCHED_TEST and SCHED_CASE values, controlled-submake propagation, isolated paths, parser manifest and fault outcome rules are frozen in Tasks 1–3; implementation does not choose a second variant mechanism.


- **Proportion**: this plan implements §10 and the documentation half of §11. It does not re-derive the kernel-side behaviour (Phases 1-4) — it observes it.

## Execution Handoff

This is the last phase of the series. When Task 5's gate is green and the report is written, the feature is complete and the series index (`docs/superpowers/plans/2026-10-30-aarch64-el1-sched-series.md`) can be annotated with the final status and merged.
