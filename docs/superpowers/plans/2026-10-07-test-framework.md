# OS01 Lightweight Test Framework Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make OS01 test results trustworthy, reproducible, and easy to extend across host tests and both QEMU architectures.

**Architecture:** Make continues to own builds, profile gates, and image variants. Small Python standard-library modules own process lifecycle, protocol validation, and per-run evidence; existing suite scripts retain their platform assertions. C registries declare and publish selected cases, with explicit legacy adapters during migration.

**Tech Stack:** GNU Make, Python 3 standard library (`unittest`, `subprocess`, `selectors`, `codecs`, `json`), freestanding C, QEMU.

**Spec:** `docs/superpowers/specs/2026-10-05-test-framework-design.md`

## Global Constraints

- Add no third-party test runner, assertion library, YAML parser, or configuration DSL; support current Linux development machines and CI.
- Preserve profile/capability gates, controlled sub-makes, image variants, and the existing `test-host`, `test-static`, `test-qemu SUITE=...`, `test-kernel-selftest`, `test-aarch64 MODE=...`, and `test-contract` entries.
- Make alone defines build targets, profile rules, and image paths; Python receives actual QEMU, firmware, image, CPU, memory, and timeout values. A matrix driver may invoke named Make targets and `print-run-paths` for a selected variant, but must not independently construct artifacts, silently clean, or infer profile paths.
- Keep C case registration local to each layer; do not duplicate a master list or make fork/exec a mandatory test-isolation prerequisite.
- Preserve each suite's positive and negative acceptance rules, including COW input, DTB/AP/GIC/M1/M3 checks, and active termination after AArch64 sync-fault evidence.
- Ordinary migrated suites use protocol v1 and require at least one PASS, no FAIL, complete unique selected IDs, and no SKIP of required cases; legacy, static, and expected-fatal suites use explicit evidence adapters.
- Direct runner exit codes are 0=PASS, 1=FAIL/TIMEOUT, 2=configuration/environment ERROR, 130=Ctrl-C; Make need only preserve zero versus nonzero.
- Use one monotonic deadline, a 1-second post-completion observation window, and terminate-then-kill/wait within 5 seconds for owned Linux process groups.
- Archive every run under `build/<profile>/logs/tests/<suite>/<UTC-time>-<unique-id>/` with `stdout.log`, `stderr.log`, and atomically replaced `result.json`; keep success and failure runs.
- Default to serial execution with writable-image collision avoidance; do not add parallelism, retry, QMP, HTML/JUnit, or automatic log deletion.
- After kernel struct changes run `make clean`; run systest with `make OS01_SYSTEST=1 test-qemu SUITE=systest`, separate from `KERNEL_SELFTEST=1`.
- New kernel selftest public headers must follow `kernel/selftest/` ↔ `kernel/include/selftest/`; do not add UAPI for this work.

## Review Focus

- Repeated host assertion macros with side-effecting arguments evaluate operands once, even on the failure-print path (Task 1).
- A requested case whose declared/observed ID differs but count matches fails, including when a command prints a historical PASS (Tasks 4, 7).
- UTF-8 code points and protocol lines split across reads survive EOF/drain without loss or duplicate matches (Task 3).
- A full result followed by a real panic within the 1-second window fails; a diagnostic line containing the word FAIL alone does not (Tasks 4, 5).
- An expected sync fault followed by spontaneous QEMU exit fails, while a runner-owned stop after ordered evidence succeeds (Task 10).

---

## Phase A — make existing results credible

### Task 1: Host assertion and exit integrity

**Files:** Modify `hosttests/include/test_framework.h`, `hosttests/Makefile`, affected `hosttests/cases/*.c`; create `hosttests/cases/test_framework_contract.c`.

**Interfaces:** `TEST_RESULTS()` prints a snapshot without resetting `__test_stats`; `TEST_RESET()` explicitly clears it; `RUN_ALL_TESTS()` leaves failure totals available to `main`. Assertion macros evaluate each supplied expression once. `TEST_BINS` contains each binary once.

- [ ] Add a contract binary that deliberately fails one assertion, calls `TEST_RESULTS()` twice, and exits according to `__test_stats.failed`; add a side-effect assertion checking one evaluation. First run the focused binary and verify its failure survives both summaries.
- [ ] Fix the macros and any existing caller that relied on implicit reset; remove duplicate `test_libc_stdio_registry.elf`. Add a Make check that compares words in `TEST_BINS` and rejects duplicates before `run`.
- [ ] Run `make test-host` and the intentional negative binary via a command expecting nonzero; verify the negative binary is not part of the ordinary green `TEST_BINS` set.
- [ ] Commit the host integrity change.

### Task 2: Explicit framework regression entry

**Files:** Modify `qemutests/test_gfx_runner.py`, `mk/components/run.mk`, `docs/build/build.md`; add fixture-only tests to the existing runner test module if needed.

**Interfaces:** `make test-harness` runs an explicit `python3 -m unittest` module list; it never discovers arbitrary QEMU scripts. Start with `qemutests.test_gfx_runner`; append new modules in the task that creates them.

- [ ] Run `python3 -m unittest qemutests.test_gfx_runner` and record the current broken success-fixture behavior; add fixture coverage for both gfx and Tetris markers and terminal output.
- [ ] Repair the fixture to follow current `run_test.py` transport and acceptance rules, then wire `test-harness` and its help entry to explicit modules.
- [ ] Run `make test-harness`; verify no real QEMU process starts and invalid fixture output still fails.
- [ ] Commit the harness entry and fixture.

## Phase B — one dependable execution and evidence layer

### Task 3: Process session with a single deadline

**Files:** Create `qemutests/harness/__init__.py`, `qemutests/harness/process.py`, `qemutests/test_harness_process.py`.

**Interfaces:** `ProcessSession(argv: list[str], run_dir: Path, timeout_s: float, *, writable_stdin: bool = False)` owns `start()`, `send(data: bytes)`, `wait_for(predicate: Callable[[str], bool]) -> str`, `observe(seconds: float) -> str`, `stop() -> int | None`, and `close()`. It exposes `text`, `returncode`, `stopped_by_runner`, `timed_out`, and a monotonic `deadline`; each wait consumes remaining time. `stdout.log` and `stderr.log` are persistent independent streams.

- [ ] Add `unittest` cases using short Python child processes for invalid executable, partial UTF-8/line boundaries, heavy stdout/stderr, EOF drain, repeated marker after a cursor advance, stdin closure, timeout, Ctrl-C cleanup, and child process-group cleanup; run and see missing API failures.
- [ ] Implement argv-only Linux process-group startup and selector-based incremental drains with an incremental UTF-8 decoder and persistent read cursor. On stop/exception/interrupt, terminate, wait up to 5 seconds, kill if needed, then wait/reap; always close file descriptors.
- [ ] Run `python3 -m unittest qemutests.test_harness_process`; verify no child remains after timeout or interruption.
- [ ] Commit the process session.

### Task 4: Protocol, result, and archive contract

**Files:** Create `qemutests/harness/result.py`, `qemutests/test_harness_result.py`.

**Interfaces:** `parse_v1(text: str, *, suite: str, requested_case: str | None = None, allowed_ids: set[str] | None = None) -> ProtocolResult` validates anchored complete records; `ProtocolResult` carries selected/started/terminal ID sets, counts, reasons, and errors. `RunReport` carries schema version 1, run ID, git revision/dirty state, profile/suite/request, declared/observed IDs when applicable, actual argv/CPU/memory/tool versions, firmware/image paths and before/after SHA-256, UTC start/duration, runner/child exit and controlled-stop data, status/count unit/case outcomes, and log paths. `RunArchive.create(build_dir: Path, suite: str) -> RunArchive` creates the unique directory; `write(report: RunReport) -> None` atomically replaces `result.json`.

- [ ] Add table-driven fixtures for duplicate/missing START/SELECT/BEGIN/terminal/END, malformed/half lines, unknown version/ID, mismatched totals, all SKIP, missing reason, SKIP of required case, explicit CASE=A with B declared or run, same-count ID substitution, and a plain diagnostic containing `FAIL`; run and see missing API failures.
- [ ] Implement the strict state machine with IDs `[A-Za-z0-9_.-]+`, `SELECT required=0|1`, `total=passed+failed+skipped=expected`, at least one PASS, and exact set equality. Add archive tests for unique names, absent/changed input hashes, interrupted-write atomicity, dirty revision, and failure when `result.json` cannot be written.
- [ ] Run `python3 -m unittest qemutests.test_harness_result`; inspect one sample schema-v1 JSON against spec §7.2.
- [ ] Commit the protocol and archive contract.

### Task 5: Migrate x86 QEMU process boundaries without weakening gates

**Files:** Modify `qemutests/run_test.py`, `qemutests/test_resolution_switcher.py`, `qemutests/driver_matrix_run.py`, `qemutests/test_driver_model_matrix.py`, `qemutests/test_gfx_runner.py`, `mk/components/run.mk`; create `qemutests/test_run_test_harness.py`, `qemutests/test_make_qemu_failure.py`.

**Interfaces:** Existing `TestRunner`/suite functions remain callable; its QEMU process and output methods delegate to `ProcessSession`. `ResolutionSession` in `test_resolution_switcher.py` and `_run_case` in `driver_matrix_run.py` delegate their own QEMU boundaries to the same layer, preserving QMP, private image/snapshot, and per-device checks. Each actual QEMU run has an explicit legacy `suite`-unit report; no generic PASS-line shortcut. `driver_matrix_run.py` may invoke only Make's named variant build and `print-run-paths` targets, with build failure ending that case before QEMU; Make still owns the build recipe and returned paths. `SMP` is passed as effective QEMU CPU count and archived; if a caller also sets legacy `QEMU_SMP` to a different value, fail before launch. `--case` is not added to suites lacking native selection.

- [ ] Record fixture tests for each `SUITE` branch (`phase-0`, `systest`, `inittab-phase`, `network`, `gfx`, `resolution`, `driver-model`): positive completion, existing negative markers, old-PASS replay, late panic in the 1-second window, early QEMU exit, startup failure, and nonzero child status. For resolution and driver-model, pin existing QMP/image-isolation and device/case evidence, respectively.
- [ ] Add Make failure fixtures: substitute failing image-build or hash-check commands and a marker-writing runner; assert `test-qemu` exits nonzero and never starts the runner. Run red against the current recipe, where a later successful command can mask either failure.
- [ ] Replace the three QEMU process boundaries with `ProcessSession` and `RunArchive`; retain serial-stdio input, network helpers, extra QEMU arguments, QMP, private-image/snapshot rules, and driver-model's Make-target variant preparation. Add explicit `|| exit` or equivalent short-circuiting after every build/hash command in `test-qemu` and its driver-model dispatch so an old image cannot produce a green result.
- [ ] Run `python3 -m unittest qemutests.test_make_qemu_failure qemutests.test_run_test_harness qemutests.test_driver_model_matrix`, `make test-harness`, `make test-qemu SUITE=phase-0`, `make test-qemu SUITE=resolution`, and `make test-qemu SUITE=driver-model DRIVER_MODEL_DRIVER_CASE=no-nic`; verify full logs and JSON for positive and negative fixtures.
- [ ] Commit the common runner, resolution, and driver-model migrations as separately reviewable commits.

### Task 6: Migrate the x86 kernel-selftest runner

**Files:** Create `qemutests/run_kernel_selftest.py`; modify `qemutests/test_kernel_selftest_result.py`, `qemutests/check_kernel_selftest.py`, `mk/components/run.mk`.

**Interfaces:** `run_kernel_selftest.py` receives explicit firmware, image, QEMU, CPU/memory, timeout, and build directory; legacy `failures(log: str) -> list[str]` remains the semantic gate until Task 9 replaces it with v1. Success triggers controlled QEMU stop after complete boot and task markers, rather than waiting for an external 75-second timeout.

- [ ] Add fixtures where boot summary passes but task marker is absent, a task reports failure, QEMU exits early, and a panic arrives after summary; run them red against the new entry.
- [ ] Route the Make target through the new runner with `KERNEL_SELFTEST_SMP` compatibility and an actual-config field; retain the existing prohibition against combining selftest and systest and the existing task checks.
- [ ] Run `make test-harness` and `make KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=8 test-kernel-selftest`; verify QEMU is reaped and a completed run ends without the old fixed wait.
- [ ] Commit the selftest runner migration.

## Phase C — trustworthy case selection and completion

### Task 7: Host C cases and binary selection

**Files:** Modify `hosttests/include/test_framework.h`, `hosttests/Makefile`, `mk/components/run.mk`; create `qemutests/run_hosttests.py`, `qemutests/test_run_hosttests.py`.

**Interfaces:** `run_hosttests.py --build-dir DIR [--binary ID] BINARY...` receives the `TEST_BINS` list from Make, rejects unknown/duplicate/empty selection, runs each binary with its own timeout and archive, and continues after a test failure. A legacy binary requires rc=0 and nonempty suite evidence; if it prints a summary, that summary must be complete and have zero failures. Any printed nonzero failed summary with rc=0 is FAIL. Migrated binaries additionally use `parse_v1` with their selected case IDs.

- [ ] Add fixture binaries/scripts for a valid binary, assertion failure masked by rc=0, crash, hang, duplicate path, empty/unknown binary selection, and one failed binary followed by a passing one; run red.
- [ ] Emit v1 START/SELECT/BEGIN/terminal/END from the shared `RUN_ALL_TESTS()` path, retaining assertion counts separately from case counts. Label existing model-only binaries in the report; keep other host binaries on the strict legacy adapter until their own registration path is migrated.
- [ ] Change `hosttests/Makefile run` to call the runner with its authoritative `TEST_BINS` list; `test-host` remains its Make bucket. Run `make test-host`, focused host fixtures, and `make test-harness`.
- [ ] Commit the host case/selection migration.

### Task 8: Systest stable IDs and v1 result

**Files:** Modify `user/systest.c`, `qemutests/run_test.py`, `mk/components/run.mk`, `config/inittab.systest`; create `qemutests/test_systest_protocol.py` and a generated, per-run `build/<profile>/config/inittab.systest.case` when selection is requested.

**Interfaces:** The existing `tests[]` table is the sole source for `systest --list`, `systest --case <id>`, `systest --quick`, `systest --full`, and SELECT records. Each table row has a stable machine ID (`[A-Za-z0-9_.-]+`) distinct from its display name, plus `required` and `quick` flags; default is full. Per-case status derives from assertion-failure delta; no new child isolation. For `make OS01_SYSTEST=1 test-qemu SUITE=systest CASE=<id>`, Make writes a private inittab line `tty1:once:/bin/systest --case <id>` and passes that file through the existing `INITTAB_FILE` input; the host validates `parse_v1(..., suite="systest", requested_case=...)`. The default inittab also uses `once` so one guest boot emits one START/END; omitted CASE still runs the full table.

- [ ] Add table/runner tests for unique IDs, list/execution parity, quick/full subset behavior, unknown and empty case, same-count wrong ID, one selected case executing once, and `PASS` text inside a failed case; run red.
- [ ] Add list/filtering and v1 records around the existing test functions. Keep `[PASS]`/`[FAIL]` diagnostics and the `[SYS TEST] RESULT` line for transition, but make v1 the only success gate after this suite switches. Preserve the four COW TTY handshakes and reject booting a normal `/bin/terminal` image.
- [ ] Run `make test-harness`, `make OS01_SYSTEST=1 test-qemu SUITE=systest`, and a supported single-case invocation; verify unknown selection returns nonzero with an archived reason.
- [ ] Commit the systest protocol.

### Task 9: Kernel selftest final coordinator and placeholder policy

**Files:** Modify `kernel/selftest/selftest.c`, `kernel/core/main.c`, `kernel/sched/core.c`, the five scheduled `kernel/selftest/test_*.c` functions, `kernel/arch/aarch64/boot/main.c`, `kernel/Makefile`, `qemutests/run_kernel_selftest.py`, `qemutests/check_kernel_selftest.py`, `qemutests/test_kernel_selftest_result.py`; create `kernel/selftest/result.c`, `kernel/include/selftest/result.h`.

**Interfaces:** `void selftest_begin_run(unsigned int expected_late)`, `int selftest_run_all(void)`, `void selftest_record_late(const char *id, int rc)`, and `int selftest_end_run(void)` form the single publisher on x86; AArch64 calls `selftest_end_run()` after its boot-only tests. `selftest_begin_run` registers/declares the full configuration-specific selection before START; `selftest_run_all` executes its registered early subset. Convert scheduled void tests to status-returning functions, or return an equivalent explicit status via a wrapper; their existing diagnostic markers remain. Registration overflow, duplicate IDs, and an unfinished late case are failures. `pipe_basic` is removed from registration until it tests real pipe behavior, or is an explicit SKIP with a nonempty reason.

- [ ] Add parser fixtures for all early cases passing while a scheduled case hangs, duplicate late ID, failed scheduled case, and early-only AArch64 completion; run red. Add a focused C/host contract for no-op `pipe_basic` not producing PASS.
- [ ] Build the selected set before publishing START/SELECT, serialize case records from the coordinator, and issue exactly one END after the scheduled tests on x86 or boot tests on AArch64. Add `selftest/result.c` to AArch64's explicit `KERNEL_C_SOURCES` whitelist in `kernel/Makefile` (x86 wildcard already includes it); keep non-selftest builds linkable through conditional calls or a stable no-op interface. Remove the early summary as a success gate; keep useful existing diagnostics. Make the runner require v1 with no legacy fallback.
- [ ] If shared kernel structs change, run `make clean`; then run `make test-harness`, `make KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=8 test-kernel-selftest`, `make PROFILE=aarch64-clang KERNEL_SELFTEST=1 aarch64-uefi`, and ordinary x86/AArch64 kernel builds without `KERNEL_SELFTEST`. Run an AArch64 boot-only selftest mode; confirm a late-case hang cannot produce END.
- [ ] Commit the kernel protocol.

## Phase D — architecture coverage, CI, and documentation

### Task 10: AArch64 process migration with exact semantic adapters

**Files:** Modify `qemutests/aarch64_uefi_smp.py`, `qemutests/aarch64_gic_spi.py`, `qemutests/aarch64_sync_fault.py`, `qemutests/aarch64_m3_probe.py`, `qemutests/aarch64_m1_matrix.py`, `mk/components/run.mk`; create `qemutests/test_aarch64_harness.py`.

**Interfaces:** Each script reuses `ProcessSession`/`RunArchive`, while `acceptance_evidence`, `sync_fault_evidence`, and M1/M3 variant rules remain suite-owned. Each CPU/repeat/variant invocation gets its own report. Legacy/fatal adapters emit `suite`-unit results, never fabricated guest v1 cases; sync-fault PASS also requires QEMU alive at the evidence point and stopped by runner.

- [ ] Before each script migration, save positive and negative recorded-log fixtures for DTB/AP ack, GIC SPI, M1 RAM/exhaustion, M3 probe, no-ack, and sync-fault ordering/uniqueness/register fields. Include fatal-then-spontaneous-exit and ordinary-timeout negatives; run fixtures red where lifecycle is missing.
- [ ] Replace only common process/output/archive code, keeping diagnostic DTB generation, special image selection, and Make prep/run dispatch intact. Use an explicit compatibility adapter for scripts not yet migrated; switch one MODE at a time without weakening its checks.
- [ ] Run `make test-harness` and each touched `make PROFILE=aarch64-clang test-aarch64 MODE=...` path, including a 2-core smoke and the sync-fault mode. Compare accepted/rejected fixtures before and after each switch.
- [ ] Commit the AArch64 suite migration in small per-MODE commits.

### Task 11: Repeated syscall execution through the normal shell

**Files:** Modify `qemutests/x86_64_systest_repeat.py`, `mk/components/run.mk`; create `qemutests/test_systest_repeat_harness.py`.

**Interfaces:** `make test-syscall-repeat` continues to boot a private copy of the normal image and invoke `systest` through terminal/ash, not systest-as-init. The three stages require respectively 1, 1, and 3 complete passing systest runs; completion markers are matched only after each command's output cursor. The script uses `ProcessSession` and one archived report per QEMU run, preserves `VFS: find_mount: CORRUPT`/`PF-KRN:` rejection, and records input and post-run private-image hashes.

- [ ] Add recorded-log fixtures for 1/1/3 stage success, missing or extra result, echoed completion marker, late kernel corruption, early exit, and a hanging stage; run `python3 -m unittest qemutests.test_systest_repeat_harness` red.
- [ ] Replace the script's raw `Popen`/temporary `/tmp` log lifecycle with `ProcessSession` and `RunArchive`; retain the private image copy, stage commands, marker split, and strict result counts. Keep Make's normal-image prerequisite and pass the effective QEMU/profile configuration to the runner.
- [ ] Run `make test-harness` and `make test-syscall-repeat`; inspect the JSON/logs and verify a failed stage returns nonzero with no remaining QEMU process.
- [ ] Commit the repeat-suite migration.

### Task 12: Static-audit evidence adapter

**Files:** Create `qemutests/run_static_audit.py`, `qemutests/test_harness_static.py`; modify `mk/components/run.mk` for `test-runtime`, `test-static`, `test-kernel-layout`, `test-kernel-canary-contract`, and `test-user-canary` audit invocation boundaries.

**Interfaces:** `run_static_audit.py --build-dir DIR --suite ID -- AUDIT_ARGV...` runs exactly the supplied audit command with `ProcessSession`, preserving its exit status and diagnostics. The audit program itself owns all artifact and diagnostic assertions; the adapter records one `audit`-unit PASS only if that program actually starts and exits 0. Silence is allowed when the audit's own contract permits it. A missing executable, signal, nonzero exit, or timeout is archived as ERROR/FAIL/TIMEOUT. The adapter does not invent case records or execute build steps itself; Make still prepares inputs and selects commands.

- [ ] Add fixture audit commands that silently exit 0 after checking a real temp artifact, exit 1 on invalid/missing artifact, hang, or fail to start; run `python3 -m unittest qemutests.test_harness_static` and observe missing-entry failures.
- [ ] Wrap existing Python audits and shell/Make audit boundaries with the adapter while retaining their original checks, including `validate-kernel` and user-canary's seven checks. Keep capability gates and dependencies in Make, and archive one result per executed audit command.
- [ ] Run `make test-harness`, `make test-runtime`, `make test-static`, and `make test-user-canary`; confirm each command's report names its real argv and the negative fixtures cannot produce PASS.
- [ ] Commit the static-audit adapter.

### Task 13: CI, documentation, and final acceptance

**Files:** Modify `.github/workflows/ci.yml`, `docs/build/build.md`, `docs/build/toolchain.md` only if invocation details change, and the explicit `test-harness` module list/help in `mk/components/run.mk`.

**Interfaces:** PR jobs run `test-harness`, host/static checks, x86 phase-0 and systest at 1/2 CPUs, x86 kernel selftest at 8 CPUs, and an AArch64 2-CPU smoke. CI uploads `build/*/logs/tests/**` on failure; scheduled/manual coverage retains 1/2/4/8 CPU and RAM/fault modes. The docs distinguish binary/case/assertion counts and actual effective configuration.

- [ ] Add CI/static checks that `test-harness` is explicit, the separate systest/selftest commands never share flags, the required CPU matrix appears, and failed jobs upload artifacts; run the focused checks red.
- [ ] Update CI and docs with measured suite durations and exact current target names. Keep `test-contract` in its isolated job and preserve variant/clean boundaries. Document runner 0/1/2/130, report schema, `CASE` availability, and the 1-second observation limit.
- [ ] Run `make test-harness`, `make test-host`, `make test-static`, x86 phase-0, separate systest/selftest commands, AArch64 2-core smoke, `make test-contract` in its required isolated checkout, and representative fatal/variant modes. Deliberately fail an assertion, timeout a child, and interrupt a runner; confirm Make nonzero, JSON/log completeness, and no owned process left.
- [ ] Commit CI/docs and final acceptance adjustments.

## Execution notes

- Phase A can land independently. Phase B introduces the two reusable Python modules before any suite depends on them; Phase C removes legacy success fallbacks only for suites that emit v1. Phase D migrates AArch64 by mode.
- Keep Python unit fixtures free of real QEMU unless the step explicitly names a Make/QEMU command. For long-running modes, record exact command, elapsed time, archive path, and any environmental limitation rather than claiming an unrun acceptance.
- A framework change is complete only when its negative fixture fails for the intended reason and its positive fixture still passes.
