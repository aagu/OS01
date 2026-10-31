# aarch64 EL1 scheduler — local merge validation

Date: 2026-10-31. Local target: `master`.
Merge commit: `fa7d51ac`; parents: master `1612dd23`, feature `919afb6c`.

The merge combines the reviewed scheduler with master's build/test
optimizations. Kernel implementation files match the reviewed feature
branch; the conflict resolutions affect build integration:

- Preserve the split host-test Makefiles and register the three new
  scheduler tests in `hosttests/mk/sched.mk`, including their original
  production-object rules.
- Preserve parallel static audits and prerequisite handling; retain
  `CASE=sched-common-compile` outside the rootfs-only audit path.
- Keep the controlled submake whitelist from master and add
  `AARCH64_SCHED_TEST`.
- Extend inventory expectations to 153 entries (152 runnable tests plus
  the expected-failure framework contract), register the three host
  scenarios, and register the new `aarch64-sched` suite in the catalog.

Both profiles were cleaned before validation. Product tests ran on the
resolved merge contents while HEAD was `1612dd23` with a dirty index;
their runner records therefore identify that revision. The merge commit
`fa7d51ac` was created during validation, without changing the resolved
build or kernel contents. The catalog corrections and this report are
recorded as a follow-up commit. Commands ran from `/home/aagu/OS01`.

| Gate | Result | Transcript |
|---|---|---|
| `make PROFILE=x86_64-clang test-host` | exit 0; 152 distinct host binaries PASS | `/tmp/os01-merge-host.log` |
| `make PROFILE=x86_64-clang test-static` | exit 0; 14 audits PASS | `/tmp/os01-merge-static.log` |
| `make PROFILE=aarch64-clang test-static CASE=sched-common-compile` | exit 0 | `/tmp/os01-merge-arm-compile.log` |
| ordinary aarch64 normal/selftest builds | exit 0 each | `/tmp/os01-merge-arm-{normal,selftest}.log` |
| `make PROFILE=aarch64-clang test-aarch64 MODE=sched` | exit 0; 16/16 boots and 7/7 symbol audits | `/tmp/os01-merge-arm-sched.log` |
| `make PROFILE=aarch64-clang test-aarch64-audit` | exit 0 | `/tmp/os01-merge-arm-audit.log` |
| ordinary aarch64 idle smoke, CPUs 1/2/4/8 | exit 0 | `/tmp/os01-merge-arm-idle.log` |
| x86 kernel-selftest, `KERNEL_SELFTEST_SMP=2/8` | exit 0 each; 41/41 | `/tmp/os01-merge-kself-cpu{2,8}.log` |
| x86 kernel-selftest, `KERNEL_SELFTEST_SMP=1` | make exit 2; known 39/41 baseline | `/tmp/os01-merge-kself-cpu1.log` |
| x86 phase-0, CPUs 1/2 | exit 0 each | `/tmp/os01-merge-phase0-cpu{1,2}.log` |
| x86 `OS01_SYSTEST=1 test-syscall`, CPUs 1/2 | exit 0 each; 99/99 each | `/tmp/os01-merge-syscall-cpu{1,2}.log` |
| `make PROFILE=x86_64-clang test-harness` | initial exit 2 (two catalog contracts); corrected modules rerun exit 0, 22/22 | `/tmp/os01-merge-harness.log` |

The first full framework run completed all 31 modules: 29 passed; the
host-inventory count and scenario mapping contracts failed because the
new tests were absent from master's catalog expectations. After correcting
those metadata contracts and adding the scheduler suite catalog entry,
`python3 -m unittest qemutests.test_host_build_contract qemutests.test_scenario_catalog qemutests.test_suite_catalog`
passed all 22 tests (exit 0; `/tmp/os01-merge-catalog-green.log`). The newly
required suite ID was also demonstrated RED before its catalog entry was
added (`/tmp/os01-merge-catalog-red.log`). The other 28 modules' inputs
were unchanged, so the long full harness was not duplicated. No kernel
code changed during this correction.

Phase-0 and syscall commands set both `QEMU_SMP=N` in the environment
and `SMP=N` on make's command line. The controller checked actual
`-smp` and `cpu_count` values in the runner archives. UP failed exactly
`fd_refcount` and `x86_intr`; `sched_boot_probe` passed at 1/2/8.
The established baseline exception remains unchanged.

Runner archives are under `build/<profile>/logs/tests/`; a compact
CPU/run-ID check was saved in `/tmp/os01-merge-evidence.json`. Temporary
transcripts are local evidence, not repository artifacts. Historical
feature review is in [the implementation review](2026-10-31-aarch64-el1-sched-final-review.md).

The merge is local. No remote was updated. The original feature
worktree is retained to preserve its ignored review and validation
artifacts.
