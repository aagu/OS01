# aarch64 EL1 kernel-thread scheduler — validation report

Feature series: aarch64 EL1 kernel-thread scheduler (P1–P5), spec
[2026-10-30-aarch64-el1-kthread-scheduler-design.md](../specs/2026-10-30-aarch64-el1-kthread-scheduler-design.md)
(§9/§10/§11 acceptance). Branch `feature/aarch64-el1-sched`; the historical acceptance source
revision was `8e78717d47a8f2a5301a4954dce88dee87b348df` (initial P1–P5
implementation commits `711c4b19`, `d249b0b3`, `df16f183`, `8e78717d`).
See the [post-review fix-wave addendum](#post-review-fix-wave-2026-10-31)
for current source revision and corrected CPU-count evidence. All commands below ran on
2026-10-31 from the worktree root
`/home/aagu/OS01/.worktrees/aarch64-el1-sched`; per-step transcripts live
under `/tmp/p5-matrix/<step>.log` with the harness archives under each
profile's `build/<profile>/logs/tests/`.

Host environment: QEMU 11.1.3 (`qemu-system-aarch64 --version`), edk2
nightly firmware fetched 2026-10-31 (built 05:04), dtc 1.8.1,
llvm-nm at `/usr/bin/llvm-nm`.

## Environment assessment (P5-owned QEMU acceptance surface)

Reproduced: with the default FDT EFI-config-table handoff (no `-dtb`),
every aarch64 boot on this host dies at `[dtb] FATAL: UEFI handoff has no
DTB` (`/tmp/p5-env/no-dtb.log`, 2-CPU selftest image, timeout kill after
the fatal; log retained). The same image with a QEMU-generated diagnostic
DTB (`-dtb` + `acpi=off`) boots to the complete 14-case protocol
(`[TEST] END … total=14 passed=14`, `/tmp/p5-env/with-dtb.log`).

Assessment: host-side firmware/QEMU behavior change, not a repository
regression — verified across every repo acceptance surface: `MODE=smp` /
`no-ack` default to `--diagnostic-dtb=auto` (mk/components/run.mk),
`gic-spi` and `m3-probe` hardcode it, the M1 matrix passes it, the
Phase-3 idle-smoke runner and the new MODE=sched runner generate their
own DTB per CPU count, and `MODE=sync-fault` is unaffected because its
probe fires before DTB parsing (gate PASS this session, exit 0).
Decision: keep the harness-default posture (no firmware plumbing change
— out of scope), and document it in `docs/build/build.md` (MODE=sched
block) with this report as the evidence record. Nothing was papered
over: the failure mode, reproduction, and the explicit opt-out
(`AARCH64_UEFI_SMP_DIAGNOSTIC_DTB=0`) all remain documented.

## Build-variant symbol audit (spec §10.2, "普通镜像不包含故障注入符号")

Runner-audited via `subprocess.run([llvm-nm, elf], check=True, …)` on the
exact build paths (inside `make … MODE=sched SCHED_CASE=all`, exit 0;
symbol `aarch64_sched_fault_inject`, sha256 prefixes recorded by the
runner):

| ELF (under build/aarch64-clang/) | injection symbol | sha256 (first 16) |
|---|---|---|
| kernel/kernel.elf (ordinary normal) | absent (required) | 81015e82bb12abcf |
| kernel/selftest/kernel.elf (ordinary selftest) | absent (required) | 4788b56fed0d47ad |
| kernel/sched/kernel.elf (positive) | absent (required) | 9757d42ca2040860 |
| kernel/sched-fault-sp-top/kernel.elf | present (required) | 8ff4c3aef770e2cb |
| kernel/sched-fault-sp-low/kernel.elf | present (required) | e95ad5bcf7acbe6a |
| kernel/sched-fault-pc-low/kernel.elf | present (required) | c22ed1b76ecd5d47 |
| kernel/sched-fault-pc-data/kernel.elf | present (required) | dce20a47efcc3ac4 |

## §10.2 — aarch64 QEMU acceptance

### MODE=sched positive matrix (Steps 1+2; one unchanged-code run at HEAD)

Command: `make PROFILE=aarch64-clang test-aarch64 MODE=sched` (SCHED_CASE
defaults `all`; the identical command with explicit `SCHED_CASE=all`
satisfies plan Task 5 Step 2 — one run, not duplicated). Exit **0**.
Transcript `/tmp/p5-matrix/aarch64-sched-all.log`; per-boot archives
`build/aarch64-clang/logs/tests/aarch64-sched/20261031T*` (one RunReport
per boot: command, CPU count, variant, image/firmware sha, errors).
First positive live gate (earlier, same code): `… MODE=sched
SCHED_CASE=positive`, exit 0, 8/8 PASS (`/tmp/p5-t2-positive2.log`).

Matrix executed (16/16 PASS): positive full suite 3× at CPUs 1 and 2 and
once at 4 and 8 — `[(1,1..3),(2,1..3),(4,1),(8,1)]` — plus each fault at
CPUs 1 and 2. Every boot: absolute 120 s deadline (evidence window 119 s
+ 1 s post-completion drain inside the budget; ticks never extend it),
runner-owned QEMU stop only after complete evidence + drain.

Positive strict evidence per boot: START(expected=14); SELECT order ==
frozen manifest (5 early + 9 late); BEGIN→PASS pairs in manifest order,
exactly once each; END total=passed=14 failed=0 skipped=0; all
`[sched-evidence]` records present, ordered, with every frozen field
inside its predicate window; per-AP observations == cpus−1 with
cpu_count == -smp. Representative 2-CPU boot archive:
`build/aarch64-clang/logs/tests/aarch64-sched/20261031T084155Z-643a74c104494c398a363540ab57ed59/`
(records identical in shape to the P4 gate logs).

### MODE=sched negative variants (fault injection)

Each fault at CPUs 1 and 2 (same SCHED_CASE=all run, 2/2 PASS each).
Required and observed per boot — the exact frozen marker, then the
matching production fatal, then nothing (victim never entered/committed,
no scheduler completion):

```
[sched-fault] case=sp-top phase=pre-switch target=1 switches_to_target=0
[aarch64] context reject code=6 reason=saved sp outside usable stack …
```
- sp-top → reason=saved sp outside usable stack
- sp-low → reason=saved sp outside usable stack
- pc-low → reason=continuation outside executable kernel text (pc=0x2000)
- pc-data → reason=continuation outside executable kernel text (pc=.bss object)

Expected pre-switch rejection = negative harness case PASS; guest fatal
is distinguished from harness failure in the report/exit code (fault
cases exit 0 only on the expected rejection; timeouts/unrelated
exceptions/wrong reasons/anything after the fatal are FAIL).

Negative control (real QEMU, control build then restored): a build that
latches the victim but skips the corruption and prints the marker from
the pass path (committed transfer) — harness rc=1, both boots FAIL
("expected exactly one fault record, saw 10441" + deadline), proving a
synthetic marker cannot pass. Restored source verified by gate re-run
(exit 0, 2/2 PASS) and ELF sha identical to the audited
sched-fault-sp-top artifact (`8ff4c3aef770…`). Transcripts:
`/tmp/p5-t3-nc2-run.log`, `/tmp/p5-t3-restore.log`.

## §10.3 — regression gates (real command / exit / CPUs / log)

| Command | Exit | CPUs | Log |
|---|---|---|---|
| `make PROFILE=aarch64-clang test-aarch64 MODE=smp` | 0 | 1,2,4 ×3 → 9/9 PASS | /tmp/p5-matrix/aarch64-smp.log |
| `make PROFILE=aarch64-clang test-aarch64 MODE=m3-probe` | 0 | 2 | /tmp/p5-matrix/aarch64-m3-probe.log |
| `make PROFILE=aarch64-clang test-aarch64 MODE=gic-spi` | 0 | 1 | /tmp/p5-matrix/aarch64-gic-spi.log |
| `make PROFILE=aarch64-clang test-aarch64 MODE=sync-fault` | 0 | 1 | /tmp/p5-matrix/aarch64-sync-fault.log |
| `make PROFILE=aarch64-clang test-aarch64 MODE=m1-ram` | 0 | matrix | /tmp/p5-matrix/aarch64-m1-ram.log |
| `make PROFILE=aarch64-clang test-aarch64 MODE=m1-sparse` | 0 | matrix | /tmp/p5-matrix/aarch64-m1-sparse.log |
| `make PROFILE=aarch64-clang test-aarch64 MODE=m1-arena-exhaust` | 0 | matrix | /tmp/p5-matrix/aarch64-m1-arena.log |
| `make PROFILE=aarch64-clang test-aarch64 MODE=m1-table-exhaust` | 0 | matrix | /tmp/p5-matrix/aarch64-m1-table.log |
| `make PROFILE=aarch64-clang test-aarch64 MODE=m1-ap-bad-root` | 0 | matrix | /tmp/p5-matrix/aarch64-m1-apbadroot.log |
| `make PROFILE=aarch64-clang test-aarch64-audit` | 0 | host nm | /tmp/p5-matrix/aarch64-audit.log |
| `make PROFILE=aarch64-clang aarch64-uefi` (production image) | 0 | — | /tmp/p5-matrix/aarch64-normal-img.log |
| `python3 qemutests/aarch64_scheduler.py --idle-smoke --cpus 1 2 4 8 --image build/aarch64-clang/image/aarch64-uefi.img --firmware build/aarch64-clang/image/QEMU_EFI.fd` | 0 | 1,2,4,8 → 4/4 PASS | /tmp/p5-matrix/aarch64-idle-smoke.log |
| `make PROFILE=aarch64-clang test-aarch64 MODE=sched` (SCHED_CASE=all) | 0 | positive 1,2×3,4,8 + faults 1,2 → 16/16 PASS | /tmp/p5-matrix/aarch64-sched-all.log |
| `make PROFILE=x86_64-clang test-host` | 0 | host | /tmp/p5-matrix/x86-host.log |
| `make PROFILE=x86_64-clang test-static` | 0 | host | /tmp/p5-matrix/x86-static.log |
| `make PROFILE=x86_64-clang SMP=1 test-qemu SUITE=phase-0` | 0 | 1 | /tmp/p5-matrix/x86-phase0-smp1.log |
| `make PROFILE=x86_64-clang SMP=2 test-qemu SUITE=phase-0` | 0 | 2 | /tmp/p5-matrix/x86-phase0-smp2.log |
| `make PROFILE=x86_64-clang SMP=1 OS01_SYSTEST=1 test-qemu SUITE=systest` | 0 | 1 | /tmp/p5-matrix/x86-systest-smp1.log |
| `make PROFILE=x86_64-clang SMP=2 OS01_SYSTEST=1 test-qemu SUITE=systest` | 0 | 2 | /tmp/p5-matrix/x86-systest-smp2.log |
| `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=1 test-kernel-selftest` | **2 (baseline failure, see below)** | 1 | /tmp/p5-matrix/x86-kself-smp1.log |
| `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=2 test-kernel-selftest` | 0 | 2 | /tmp/p5-matrix/x86-kself-smp2.log |
| `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=8 test-kernel-selftest` | 0 | 8 | /tmp/p5-matrix/x86-kself-smp8.log |
| `make test-harness` | 0 | host | /tmp/p5-matrix/test-harness.log |

OS01_SYSTEST=1 was always set at the top level and never combined with
KERNEL_SELFTEST=1.

### Separate baseline entry (spec §9: unrelated failures recorded apart)

`KERNEL_SELFTEST_SMP=1` exits 2 with END `total=41 passed=39 failed=2`:
`fd_refcount` and `x86_intr` FAIL. **Pre-existing and structural, not a
regression of this series**: `kernel/selftest/test_fd_refcount.c`
returns −1 when `num_cpus == 1` ("never claim SMP coverage on one CPU",
line ~347) and `test_x86_intr.c` requires an online+ipi_ready peer CPU;
both files are untouched by the whole series
(`git diff e278019e --stat -- kernel/selftest/test_fd_refcount.c
kernel/selftest/test_x86_intr.c` is empty). The same command fails
identically before P5's x86 changes. The new `sched_boot_probe` case
PASSES at 1/2/8 (END totals 41; SMP 2/8 are 41/41 green). Not fixed
here: changing those cases would be a behavioural edit outside P5's
scope (and weakening them to SKIP would hide, not fix).

## §10.1 — static/host audits

- `make PROFILE=x86_64-clang test-host` exit 0, includes the extended
  Phase-2 rollback suite (`test_kthread_ctor`: raw-alloc / thread-alloc /
  FPU-alloc / arch-prepare fault points, each followed by residue
  assertions and a SUCCESSFUL normal retry; publication lock order
  task_list_lock → rq_lock with reverse release; exactly-one enqueue +
  notify handoff) and the new `test_sched_layout_offsets` (real
  `<arch/context.h>` host-compiled + x86 recorded offsets).
- `make PROFILE=x86_64-clang test-static` exit 0 (14 audits, incl. the
  Phase-2 `sched-common-compile` selector and Phase-1 offset asserts).
- `make PROFILE=aarch64-clang test-aarch64-audit` exit 0 — the gate's
  symbol filter already covers `vma_*` (matches `vma_free_all`),
  `uaccess_*`, `fork_*`, `spawn_user_task`, `files_(alloc|dup|unpin)`,
  `vfs_lookup`, `vmm_free_user_map`, `do_fork` — verified, no additions
  needed.
- x86 UP boot-window probe (real QEMU, not a mock/scan): creation inside
  the legal pre-publication window in `kernel_main` (CPU0 online,
  scheduler_ok=0) via the real `kernel_thread()`; late case
  `sched_boot_probe` (registered 3rd of 7 late IDs; x86 expected counts
  and the parser fixture moved atomically with the real registrations)
  proves the marker thread ACTUALLY executed under the scheduler after
  publication (bounded 2 s wait through production `schedule()`) and
  that runtime negative calls (pinned to `num_cpus`, to `NR_CPUS+3`,
  garbage flags) return −EINVAL. PASS at SMP 1/2/8 (logs cited above).
  The live "no eligible CPU at all" runtime state is pinned by the host
  suite against the real window state machine (window-closed case).

## Harness regression

`make test-harness` exit 0 — 23 unittest modules including
`qemutests.test_aarch64_harness` with the new strict-parser,
fault-evidence and make-contract classes (105 tests in that module;
module count unchanged, list is append-only).
`python3 qemutests/aarch64_scheduler.py --self-test` exit 0.

## Not run (explicit)

None. Every §10.3 gate command listed above was executed at the recorded
HEAD. (Historical note only: P4 had skipped `MODE=m3-probe`/`gic-spi`
re-runs; both are green here.)


## Post-review fix-wave addendum (2026-10-31)

Current reviewed source is commit `a6e7c3212e4be4c49c2d975381559a94d6be99ae`.
The focused validation gates below were run against the same source contents
while the worktree still had those changes unstaged/uncommitted at baseline
`58cdd0638f85bc5f71a74b9b755e6bdd24b520d2`; the subsequent commit contains
exactly that source/test diff. `git diff --check` passed before commit.

### Review fixes and focused evidence

- F1: the runner now requires the full observation drain with the child still
  alive and runner-controlled termination. Runner-level fixtures cover natural
  exit during drain for positive/fault traces and exit codes 0/1, pre-evidence
  exit, controlled stop, late fatal, and timeout. RED reproduced a false PASS
  when exit-in-window was paired with a valid trace; GREEN focused harness run:
  `python3 -m unittest qemutests.test_aarch64_harness.SchedulerRunnerLifecycleTests qemutests.test_aarch64_harness.SchedulerEvidenceParserTests qemutests.test_aarch64_harness.SchedulerFaultEvidenceTests`
  — 43 tests passed.
- F2/F3: post-publication notification uses the captured CPU value after unlock;
  the existing task-pointer wrapper remains for fork/exec. Revalidation rollback
  releases and nulls acquired FPU state before files/thread/raw allocations.
  Host RED using the original `58cdd063` kthread implementation reproduced
  four assertions: poison CPU was read as `0xfe`, destination was incorrect,
  FPU remained live, and allocation bytes remained. GREEN focused
  `test-kthread-ctor`: 161/161 passed, including immediate-reap poisoning,
  exact rollback residue assertions, and successful retry.
- F4: ARM admission rejects nonzero target CPU and freezes affinity to mask 1;
  live worker evidence checks that every observed worker has that mask. The
  direct backend negative case calls real `arch_kthread_prepare()` on a valid
  unpublished task targeting CPU1; it does not rely on the common constructor's
  earlier `scheduler_ok` rejection. Old-policy live 2-CPU evidence had
  `cpu0_only=0`; fixed positive matrix reports `cpu0_only=1`.

### Current gate results

All listed commands completed against the current source. Logs under `/tmp`
are session-local; durable runner records remain in `build/.../logs/tests/`.

| Gate | Result / actual CPUs | Evidence |
|---|---:|---|
| `make PROFILE=x86_64-clang test-host` | PASS, 152 host binaries | run completed in this session |
| `make PROFILE=x86_64-clang test-static kernel.bin` | PASS | run completed in this session |
| `make PROFILE=aarch64-clang KERNEL_SELFTEST=1 AARCH64_SCHED_TEST=positive aarch64-uefi` | PASS, compile | run completed in this session |
| `make PROFILE=aarch64-clang test-aarch64 MODE=sched` | PASS, 16/16 boots; positive 1/2/4/8, four fault variants at 1/2; 7/7 symbol audits | `/tmp/os01-p5-fix-sched.log` |
| ordinary aarch64 idle smoke | PASS at 1/2/4/8 CPUs | `/tmp/os01-p5-fix-idle.log`; `build/aarch64-clang/logs/tests/aarch64-idle-review/cpus-{1,2,4,8}/serial.log` |
| `make PROFILE=aarch64-clang test-aarch64-audit` | PASS | `/tmp/os01-p5-fix-audit.log` |
| `make PROFILE=x86_64-clang test-harness` | PASS, exit 0 | `/tmp/os01-p5-fix-harness.log` |
| `make PROFILE=x86_64-clang KERNEL_SELFTEST=1 KERNEL_SELFTEST_SMP=1 test-kernel-selftest` | known baseline: 39/41, only `fd_refcount` and `x86_intr` fail; `sched_boot_probe` PASS | `/tmp/os01-p5-fix-kernel-selftest-cpu1.log`; run `20261031T093932Z-acb855a8bc444104b78ea3618cf46435` |
| same target with `KERNEL_SELFTEST_SMP=2` | PASS, 41/41; `sched_boot_probe` PASS | `/tmp/os01-p5-fix-kernel-selftest-cpu2.log`; run `20261031T093939Z-3f535efd9a0a4b44ab2d4a92864e5c6e` |
| same target with `KERNEL_SELFTEST_SMP=8` | PASS, 41/41; `sched_boot_probe` PASS | `/tmp/os01-p5-fix-kernel-selftest-cpu8.log`; run `20261031T093947Z-b04b1bec5c6f4b9cbac45fe039eb9c48` |
| `make PROFILE=x86_64-clang OS01_SYSTEST=1 test-syscall` | PASS, 99/99, actual 1 CPU | `/tmp/os01-p5-fix-syscall-smp1.log`; run `20261031T093818Z-c7fb69b901124c56ab38ceacc60d9e45` |
| `QEMU_SMP=2 make PROFILE=x86_64-clang SMP=2 test-qemu SUITE=phase-0` | PASS, actual 2 CPUs | `/tmp/os01-p5-fix-phase0-qemusmp2.log`; run `20261031T094003Z-e5355482e9934092ae7f9569baa1a1b1` |
| `QEMU_SMP=2 make PROFILE=x86_64-clang SMP=2 OS01_SYSTEST=1 test-syscall` | PASS, 99/99, actual 2 CPUs | `/tmp/os01-p5-fix-syscall-qemusmp2.log`; run `20261031T094010Z-f42a1c333f9e411d9e3e43043d4173fd` |

**CPU-count correction to the initial P5 table above:** this repository's
phase-0/systest runner reads `QEMU_SMP`, not make's `SMP` variable. Therefore
initial commands that specified only `SMP=2` actually ran with one CPU; the
historical matrix rows remain preserved but their claimed 2-CPU counts are
not valid. Fresh two-CPU reruns above explicitly set `QEMU_SMP=2`, and their
runner `result.json` records `cpu_count: 2`. The initial `SMP=1` phase-0 and
systest results are valid 1-CPU runs. Likewise, kernel-selftest CPU count is
controlled by `KERNEL_SELFTEST_SMP`; the three initial `SMP=1/2/8` invocations
actually used its default 4 CPUs and are only redundant SMP4 evidence. The
corrected 1/2/8 runs above use the documented variable and runner records.

The UP failures remain separate baseline failures, not fixed or suppressed.
This run independently confirms the same exact failure set and passing
`sched_boot_probe`; SMP 2/8 are fully green. Do not interpret the earlier
“23/24” acceptance summary as a fully green matrix.
