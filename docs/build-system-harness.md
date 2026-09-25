# Build System Harness

This document is the authoritative reference for the Makefile-based build,
run, debug, validate, test, contract and maintenance entry points of OS01.
It is the contract that the `help` target, `AGENTS.md`, and CI scripts all
agree on. When the Makefile changes, this doc and `make help` change together.

## 1. Capability gate

Every entry point declares the capability it needs from the active profile:
`kernel`, `userland`, `rootfs`, `uefi`. The gate is enforced by
`$(call require_capability,<cap>)` at parse time, so a mis-targeted
invocation fails fast with `PROFILE='<p>' lacks capability '<cap>'`,
not at link time.

The default `x86_64-clang` profile declares `kernel userland rootfs uefi`.
The `aarch64-clang` profile declares `kernel uefi`.

## 2. Target taxonomy

| Bucket | Canonical names | Capability | Purpose |
| --- | --- | --- | --- |
| Build artifact | `disk.img`, `kernel.bin`, `lib`, `user`, `image`, `sysroot` | profile-specific | Produce a single named artifact |
| Run / Debug | `run`, `run-kvm`, `run-virtio`, `debug` | `rootfs` | Launch QEMU against an existing image |
| Bring-up | `aarch64-uefi`, `aarch64-uefi-kernel`, `run-aarch64-uefi` | `uefi` | AArch64 UEFI bring-up chain |
| Validate | `validate`, `validate-kernel`, `validate-uefi`, `validate-profile` | `rootfs` (last: `always`) | x86 artifact sanity checks and profile inspection |
| Test | `test-qemu`, `test-host`, `test-static`, `test-kernel-selftest`, `test-aarch64`, `test-contract` (6 buckets; see §3) | varies | End-to-end and audit suites |
| Inspection | `print-run-paths` | `rootfs` | Print resolved paths for external QEMU invocation |
| Maintenance | `clean`, `unlock-profile` | always | Lifecycle |

## 3. Test bucket model

There are **6 test bucket targets** — 3 with flags and 3 without:

| Bucket | Flag | Values | What it runs |
| --- | --- | --- | --- |
| `test-qemu` | `SUITE=` | `phase-0`, `systest`, `inittab-phase`, `network` | `qemutests/run_test.py <SUITE>` against the matching variant image |
| `test-aarch64` | `MODE=` | `smp`, `no-ack`, `gic-spi` | One of `qemutests/aarch64_*.py` |
| `test-contract` | `PROFILE=` | `x86_64-clang`, `aarch64-clang` | `qemutests/build_contract.sh <PROFILE> <mode>` per profile mode list |
| `test-host` | — | — | `os01_submake hosttests` + `pmm_boot_reservation_test.py` |
| `test-static` | — | — | All 8 static audits (runtime_audit, stack_canary_audit, validate-kernel, runtime_link_order, kernel_runtime_link, kernel_layout, kernel_canary_contract, test-user-canary) |
| `test-kernel-selftest` | — | — | Boots the selftest image variant (`KERNEL_SELFTEST=1`) and parses `[selftest]` markers |

**Standalone test targets** that are not folded into any bucket because
they use a distinct harness or image variant:

| Standalone | Why not bucketed |
| --- | --- |
| `test-syscall-repeat` | Uses `x86_64_systest_repeat.py` (distinct from `run_test.py`) and the normal image variant |
| `test-user-canary` | Subset of `test-static` but callable on its own; prereqs (`USER_ARTIFACTS`, busybox, rootfs manifest) differ |
| `test-pmm-boot-reservation` | Sub-step of `test-host`; callable standalone for PMM-only debugging |

**Focused compatibility checks** remain visible in `make help` during the
alias window, with their original narrow behavior:

| Target | What it checks |
| --- | --- |
| `test-kernel-layout` | x86 kernel ELF layout only |
| `test-kernel-canary-contract` | Kernel canary compile-flag contract only |
| `test-aarch64-gic-spi` | PL011 RX to GIC SPI injection only |

## 4. Alias policy

Bucket targets are the canonical names. The legacy fine-grained names
(`test-syscall`, `test-runtime`, `test-aarch64-uefi-smp`,
`test-build-contract-x86`, etc.) remain callable during the compatibility
cycle. Parameterized aliases forward to a bucket with the right flag.
Subset compatibility targets (`test-runtime`, `test-kernel-layout`,
`test-kernel-canary-contract`, `test-user-canary`) retain their original
recipes; they never call the broader `test-static`
(or any other bucket) wholesale, because CI relies on the narrower
scope of the original target. Aliases exist for one release cycle after
the bucket is introduced; deletion is tracked in a followup plan, not
in the bucket-introduction plan itself.

Adding a new alias is **not** a substitute for using the bucket target
in new code or CI. Aliases are compatibility shims.

## 5. Adding a new target

1. Decide the bucket (build / run / debug / bring-up / validate / test /
   inspection / maintenance).
2. Pick the flag value (if any) and confirm the recipe is not just a
   variant of an existing recipe — if it is, fold it into a bucket and
   add an alias instead.
3. Add the gate (`require_capability`), pick the existing Make variables
   for paths and tools, write the recipe.
4. If the recipe contains a `$(MAKE)` call (variant build, sub-make),
   put it on its own recipe line so `make -n` honors the dry-run
   contract — see the existing comment at run.mk lines 263-266.
5. Append a line to the `help` recipe in `mk/components/run.mk` matching
   the bucket's printf format.
6. Update this doc and `AGENTS.md` if the bucket gains new values.

## 6. Variable reference

| Variable | Defined in | Used by |
| --- | --- | --- |
| `RUN_QEMU_BASE` | `mk/components/run.mk` | `run`, `run-kvm`, `run-virtio`, `debug` |
| `RUN_QEMU_DISK` | `mk/components/run.mk` | common disk, RNG, memory, display and serial arguments after NIC selection |
| `RUN_QEMU_FLAGS_run-kvm` / `RUN_QEMU_FLAGS_debug` | `mk/components/run.mk` | flags inserted before the shared network/disk arguments |
| `TEST_QEMU_FLAVOR_<suite>` / `TEST_QEMU_IMG_<suite>` | `mk/components/run.mk` | `test-qemu` per-SUITE lookups (Make variables, not shell vars) |
| `X86_CONTRACT_MODES` / `AARCH64_CONTRACT_MODES` | `mk/components/run.mk` | `test-contract` |
| `_test-contract-prep-x86` / `_test-contract-prep-aarch64` (private) | `mk/components/run.mk` | per-PROFILE pre-build steps (split so `+env` sits at recipe-line position) |
| `TEST_AARCH64_EXTRA_<mode>` | `mk/components/run.mk` | `test-aarch64` per-MODE DTB flag |
| `_test-aarch64-prep-<mode>` / `_test-aarch64-run-<mode>` (private) | `mk/components/run.mk` | per-MODE pre-build + python invocation (split so `$(MAKE)` and python sit on separate recipe lines) |