# Profile-only UEFI Overlay Reduction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the root Makefile the only supported OS01 build entry point, move all runtime firmware and host-test outputs under the selected profile, retain only the required posix-uefi `int8_t` patch, and remove the runtime Makefile overlay.

**Architecture:** Root `mk/` modules own public targets, artifact edges, firmware acquisition, and clean. Component Makefiles receive the selected profile only through `OS01_PROFILE_FILE` and write only profile-private paths. The UEFI adapter still creates a clean private copy of the fixed posix-uefi submodule, but supplies the former `0002` behavior through an explicitly scoped wrapper environment rather than patching upstream Makefiles.

**Tech Stack:** GNU Make, POSIX shell, Python 3, Clang/LLVM, lld, QEMU, curl or wget, patch.

**Spec:** [docs/superpowers/specs/2026-09-02-profile-only-uefi-overlay-reduction-design.md](../specs/2026-09-02-profile-only-uefi-overlay-reduction-design.md)

## Global Constraints

- Do not add CMake, Ninja, Meson, Bazel, a fork, an upstream PR, or a Termux `-lgcc` solution.
- Keep `config/posix-uefi/0001-clang-int8.patch`; never patch or mutate the posix-uefi submodule or another OS01 source tree during a build.
- Remove `config/posix-uefi/0002-runtime-make-overlay.patch` only after both x86_64 and aarch64 probes prove the environment contract.
- Preserve all public root targets and both architectures' boot ABI and UEFI entry ABI.
- All generated objects, host tests, UEFI runtime copies, EFI files, images, and downloaded x86 firmware live below `build/<profile>/`.
- `disk.img` and `kernel.bin` at the repository root remain default-profile compatibility copies only; non-default profiles must never modify them.
- Run `make PROFILE=<profile> clean` before every full build verification; do not use component `clean` targets.

---

### Task 1: Add regression checks for the final profile-only contract

**Files:**
- Modify: `tests/build_contract.sh`
- Modify: `mk/components/run.mk:120-230`
- Create: `mk/profiles/x86_64-clang-fixture.mk`

**Interfaces:** Extends `tests/build_contract.sh <profile> <mode>` with `legacy-components`, `legacy`, `firmware`, and `host-test` modes. `legacy-components` covers the four production components; `legacy` adds host-test after Task 4. Root build-contract targets invoke the complete modes only after their required artifacts exist.

- [ ] **Step 1: Add the failing static legacy-path check**

First move the current unconditional `test -d "$base"` and x86 sysroot-generation/symlink prelude into only the modes that consume produced artifacts: `x86`, `sysroot`, `firmware`, and `host-test`. `legacy-components` and `legacy` must run their static scans and parse-time invocations before, and without, any build-directory assertion.

Add a `legacy-components` case that fails if the four production components retain a standalone branch:

```sh
legacy-components)
    ! rg -n 'toolchain\.mk|build/\$\(ARCH\)|Legacy standalone|ifndef OS01_PROFILE_FILE' \
      kernel libc user boot/uefi
    for d in kernel libc user boot/uefi; do
        log=$(mktemp)
        if make -C "$d" -n >"$log" 2>&1; then
            cat "$log"; rm -f "$log"; exit 1
        fi
        grep -F 'make PROFILE=' "$log"
        rm -f "$log"
        log=$(mktemp)
        if make -C "$d" OS01_PROFILE_FILE=/nonexistent -n >"$log" 2>&1; then
            cat "$log"; rm -f "$log"; exit 1
        fi
        grep -F "OS01_PROFILE_FILE='/nonexistent' does not exist" "$log"
        rm -f "$log"
    done
    ;;
```

Add a `legacy` case for Task 4 by extending the same scan with `test/build|build/test_poll_requested\.elf` and adding `test` to both direct-invocation loops (empty and `/nonexistent` profile file). Add a committed non-default fixture profile, `mk/profiles/x86_64-clang-fixture.mk`, which includes the x86 profile settings and therefore has `rootfs`; its only purpose is to exercise aliases without using the default profile's compatibility files. The regression contract covers the two required error cases: missing `OS01_PROFILE_FILE` and a nonexistent profile path. A valid root-selected profile file is the supported root-internal component interface.

```make
# x86_64-clang-fixture — build-contract-only non-default rootfs profile.
include $(dir $(lastword $(MAKEFILE_LIST)))x86_64-clang.mk
```

- [ ] **Step 2: Run the check against the current tree**

Run: `sh tests/build_contract.sh x86_64-clang legacy-components`

Expected: fail because legacy `ifndef OS01_PROFILE_FILE` branches, `toolchain.mk`, `test/build`, and direct `make -C` entry points still exist.

- [ ] **Step 3: Add firmware, alias, and host-test assertions**

Add a `firmware` mode that creates an exact 4 KiB local fixture, invokes the fixture profile's real firmware file target, and then verifies isolation:

```sh
default_profile=x86_64-clang
fixture_profile=x86_64-clang-fixture
test -f disk.img
mkdir -p "build/$fixture_profile"
sha256sum disk.img | cut -d' ' -f1 > "build/$fixture_profile/root-disk.before"
fixture=$(mktemp)
dd if=/dev/zero of="$fixture" bs=4096 count=1 status=none
make PROFILE="$fixture_profile" OVMF_FIRMWARE_SOURCE="$fixture" \
  "$(pwd)/build/$fixture_profile/firmware/OVMF.fd"
make PROFILE="$fixture_profile" disk.img
test -f "build/$fixture_profile/firmware/OVMF.fd"
test ! -e boot/uefi/OVMF.fd
test "$(sha256sum disk.img | cut -d' ' -f1)" = "$(cat build/$fixture_profile/root-disk.before)"
test -f "build/$fixture_profile/image/disk.img"
rm -f "$fixture"
```

For the HTTPS branch, prepend a temporary fake `wget` to `PATH`; it must require the URL to start with `https://`, write a known 4 KiB output to the `-O` path, and record the URL. Remove `build/$fixture_profile/firmware/OVMF.fd`, invoke the same real target with `PROFILE="$fixture_profile"`, assert the record begins with `https://`, then remove the temporary fake directory. This tests the download branch without network access.

Add a `host-test` mode that checks `build/$profile/host-test/test_poll_requested.elf`, rejects `test/build` and `build/test_poll_requested.elf`, runs the focused binary, invokes profile clean, and confirms the host-test directory is gone. Add `legacy`, `firmware`, and `host-test` calls to the existing x86 build-contract target after its artifact prerequisites.

- [ ] **Step 4: Verify the test script syntax and commit**

Run: `sh -n tests/build_contract.sh && make PROFILE=x86_64-clang -n test-build-contract-x86`

Expected: shell syntax succeeds; the dry-run prints the new modes but current assertions still fail until later tasks.

```bash
git add tests/build_contract.sh mk/components/run.mk mk/profiles/x86_64-clang-fixture.mk
git commit -m "test(build): define profile-only cleanup contract"
```

### Task 2: Root-own x86 firmware, images, and QEMU test inputs

**Files:**
- Modify: `mk/targets/x86_64.mk`
- Modify: `mk/components/run.mk:12-210`
- Modify: `mk/components/image.mk:20-105`
- Modify: `tests/run_test.py:13-45`
- Modify: `mk/project.mk:70-95`

**Interfaces:** Produces `OVMF_FIRMWARE`, `OVMF_FIRMWARE_SOURCE`, and `print-run-paths`. `OVMF_FIRMWARE_SOURCE` accepts only an `https://` URL or an existing absolute local file. All x86 QEMU consumers receive the same readable `OVMF_FIRMWARE`; non-default `disk.img` is a phony alias to `NORMAL_IMAGE`.

- [ ] **Step 1: Make the firmware-source test fail**

In `tests/build_contract.sh`, create a small local firmware fixture with `mktemp`, then run:

```sh
make PROFILE=x86_64-clang-fixture OVMF_FIRMWARE_SOURCE="$fixture" print-run-paths
! make PROFILE=x86_64-clang-fixture OVMF_FIRMWARE_SOURCE=relative.fd \
  "$(pwd)/build/x86_64-clang-fixture/firmware/OVMF.fd"
! make PROFILE=x86_64-clang-fixture OVMF_FIRMWARE_SOURCE=/no/such/OVMF.fd \
  "$(pwd)/build/x86_64-clang-fixture/firmware/OVMF.fd"
```

Expected: current tree has no `print-run-paths` target, so the command fails before the contract exists.

- [ ] **Step 2: Define profile-private firmware and safe acquisition**

In `mk/targets/x86_64.mk`, add:

```make
OVMF_FIRMWARE ?= $(BUILD_DIR)/firmware/OVMF.fd
OVMF_FIRMWARE_SOURCE ?= https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd
```

In `run.mk`, replace `boot/uefi/OVMF.fd` with a real-file `$(OVMF_FIRMWARE)` rule. Its one shell recipe must: accept `https://*` and run `wget -q -O "$@.tmp" "$source"`; accept an absolute `/...` source only after `test -f`; reject every other value; then `mv` the complete temporary file atomically. For a local source, use `cmp -s` before `cp`. Never call `boot/uefi` for firmware.

Before finalizing that rule, perform a one-time, exact legacy-artifact cleanup: if `boot/uefi/OVMF.fd` exists, require both `git check-ignore -q boot/uefi/OVMF.fd` and failure of `git ls-files --error-unmatch boot/uefi/OVMF.fd`, then remove only that path. Root `clean` also applies those two guards before removing this obsolete ignored artifact; it must never recurse into a component. If either guard fails, print an error and preserve the file. This deliberately removes only a verified old generated file, never a tracked or user-owned source file.

- [ ] **Step 3: Route every consumer through profile paths**

Make `run`, `run-kvm`, `run-virtio`, and `debug` depend on `$(NORMAL_IMAGE) $(OVMF_FIRMWARE)` and use those two variables in QEMU arguments. Add capability-gated prerequisites on `$(OVMF_FIRMWARE)` to `test-phase-0`, `test-syscall`, `test-inittab`, and `test-network`; prefix every Python runner recipe with both `OVMF_FIRMWARE="$(OVMF_FIRMWARE)"` and its existing `DISK_IMG` when applicable.

In `tests/run_test.py`, replace the hard-coded pflash string with:

```python
OVMF_FIRMWARE = os.environ.get("OVMF_FIRMWARE")
if not OVMF_FIRMWARE or not os.path.isfile(OVMF_FIRMWARE):
    raise SystemExit("OVMF_FIRMWARE must name a readable firmware file")
```

Build the pflash argument with `OVMF_FIRMWARE`. Add a `print-run-paths` root target gated by `rootfs` that prints absolute `firmware=...` and `image=...` values.

In `image.mk`, keep the real root `disk.img` copy rule only under `ifeq ($(PROFILE),$(DEFAULT_PROFILE))`; in the other branch define `.PHONY: disk.img` and `disk.img: $(NORMAL_IMAGE)` with no copy recipe.

- [ ] **Step 4: Verify dry-run and local-source behavior**

Run: `make PROFILE=x86_64-clang OVMF_FIRMWARE_SOURCE=/tmp/ovmf-fixture run -n | rg 'build/x86_64-clang/(firmware/OVMF\.fd|image/disk\.img)'`

Run: `env -u OVMF_FIRMWARE python3 tests/run_test.py phase-0 --disk /tmp/no-disk`

Expected: the first command has no `boot/uefi/OVMF.fd` or root `disk.img`; the second fails with the explicit firmware-variable message before QEMU starts.

- [ ] **Step 5: Commit**

```bash
git add mk/targets/x86_64.mk mk/components/run.mk mk/components/image.mk mk/project.mk tests/run_test.py tests/build_contract.sh
git commit -m "refactor(build): own x86 firmware per profile"
```

### Task 3: Remove every legacy component build branch

**Files:**
- Modify: `kernel/Makefile:1-35`
- Modify: `kernel/arch/x86_64/make.config:1-14`
- Modify: `kernel/arch/aarch64/make.config:1-14`
- Modify: `libc/Makefile:1-35`
- Modify: `user/Makefile:1-45`
- Modify: `boot/uefi/Makefile:1-155`
- Modify: `mk/components/kernel.mk`
- Delete: `toolchain.mk`

**Interfaces:** Each listed component fails at parse time unless `OS01_PROFILE_FILE` names a readable profile file. Root `os01_submake` remains the only component caller. `ARCH` is supplied by the root artifact rules where architecture selection is needed.

- [ ] **Step 1: Exercise the red direct-invocation cases**

Run:

```bash
for d in kernel libc user boot/uefi; do make -C "$d" -n; done
make -C kernel OS01_PROFILE_FILE=/nonexistent -n
```

Expected: all currently parse or build; this establishes the cases Task 1 will turn red.

- [ ] **Step 2: Install the common parse-time gate**

At the top of each component Makefile, after computing its local path, use this exact contract before any build variables:

```make
ifeq ($(strip $(OS01_PROFILE_FILE)),)
$(error invoke from the repository root: make PROFILE=x86_64-clang <target>)
endif
ifeq ($(wildcard $(OS01_PROFILE_FILE)),)
$(error OS01_PROFILE_FILE='$(OS01_PROFILE_FILE)' does not exist; invoke from the repository root)
endif
include $(OS01_PROFILE_FILE)
```

Remove `ifdef OS01_PROFILE_FILE` / `else` branches, legacy `ARCH ?=` defaults, `build/$(ARCH)` assignments, and every `toolchain.mk` include. In both kernel arch configs, retain only architecture flags and consume compiler variables supplied by the profile.

In `mk/components/kernel.mk`, pass architecture explicitly on every root-controlled kernel submake: the x86 artifact invocation gets `ARCH=x86_64`; the aarch64 artifact invocation gets `ARCH=aarch64`. No component may select an architecture by default after this task.

At the first line of both `kernel/arch/x86_64/make.config` and `kernel/arch/aarch64/make.config`, add the same empty-profile guard (without a second include, because `kernel/Makefile` already loaded the profile):

```make
ifeq ($(strip $(OS01_PROFILE_FILE)),)
$(error invoke from the repository root: make PROFILE=x86_64-clang <target>)
endif
```

- [ ] **Step 3: Remove UEFI wrapper legacy behavior**

Make `boot/uefi/Makefile` use only `UEFI_BUILD_DIR`, `UEFI_RUNTIME_DIR`, and root-passed `ARCH`. Remove its copied-runtime creation, all `sed -i`, `OVMF.fd`, `clean-all`, and legacy source-tree clean logic. Its `all` target must only check that the adapter staged `$(RUNTIME_DIR)` and compile the supplied sources; its `clean` target may remove only its two profile-private directories if retained for root-internal use.

Delete `toolchain.mk` only after `rg -n 'toolchain\.mk' --glob '!docs/**' --glob '!thirdpart/**' .` has no functional references.

- [ ] **Step 4: Verify gates and supported paths**

Run: `sh tests/build_contract.sh x86_64-clang legacy-components`

Run: `make PROFILE=x86_64-clang -n kernel.bin lib user disk.img validate && make PROFILE=aarch64-clang -n aarch64-uefi`

Expected: all five direct component cases fail with the root command hint; root targets still expand successfully.

- [ ] **Step 5: Commit**

```bash
git add kernel libc user boot/uefi mk/components/kernel.mk toolchain.mk tests/build_contract.sh
git commit -m "refactor(build): require profiles in all components"
```

### Task 4: Move host tests into the profile build tree

**Files:**
- Modify: `mk/profiles/x86_64-clang.mk`
- Modify: `mk/profiles/aarch64-clang.mk`
- Modify: `test/Makefile:1-340`
- Modify: `mk/components/run.mk:135-150,245-275`
- Modify: `tests/build_contract.sh`

**Interfaces:** `HOST_TEST_BUILD_DIR := $(BUILD_DIR)/host-test` exists in both profiles. `test/Makefile` receives it through `OS01_PROFILE_FILE`, emits every object and test executable below it, and exposes only profile-private focused test paths.

- [ ] **Step 1: Run the isolation check in its red state**

Run: `rm -rf test/build build/x86_64-clang/host-test && make PROFILE=x86_64-clang test`

Expected: current tree creates `test/build`; the contract's `host-test` mode fails.

- [ ] **Step 2: Make the test component profile-only**

Apply the Task 3 parse-time gate to `test/Makefile`. Replace `TEST_BLD := $(TEST_SRC)/build` with `TEST_BLD := $(HOST_TEST_BUILD_DIR)`. Keep host compilation host-native (`HOST_CC ?= clang`) and do not inject target `--target`, sysroot, or kernel link flags. Remove the root-level `build/test_poll_requested.elf` duplicate; retain only `$(TEST_BLD)/test_poll_requested.elf`.

Add `HOST_TEST_BUILD_DIR := $(BUILD_DIR)/host-test` to both profile files so parsing each profile is self-contained.

- [ ] **Step 3: Use the controlled root adapter and clean scope**

Replace `$(MAKE) -C test run` in root `test` with:

```make
test:
	$(call require_capability,rootfs)
	@$(call os01_submake,test,run $(OS01_SUBMAKE_ARGS))
```

Remove `test/build` and legacy component recursive cleans from root `clean`; retain only the existing lock, lease check, profile build removal, and default compatibility removal.

- [ ] **Step 4: Verify host tests and commit**

Run: `make PROFILE=x86_64-clang clean && make PROFILE=x86_64-clang test && sh tests/build_contract.sh x86_64-clang host-test && sh tests/build_contract.sh x86_64-clang legacy`

Expected: all host tests run; only `build/x86_64-clang/host-test` is created and profile clean removes it.

```bash
git add mk/profiles test/Makefile mk/components/run.mk tests/build_contract.sh
git commit -m "refactor(build): isolate host tests by profile"
```

### Task 5: Replace the UEFI runtime overlay with an explicit environment contract

**Files:**
- Modify: `mk/components/uefi.mk:1-155`
- Modify: `boot/uefi/Makefile:45-140`
- Delete: `config/posix-uefi/0002-runtime-make-overlay.patch`
- Keep: `config/posix-uefi/0001-clang-int8.patch`
- Modify: `tests/build_contract.sh`

**Interfaces:** The root adapter passes `UEFI_RUNTIME_CFLAGS=-DUEFI_NO_UTF8` and `UEFI_RUNTIME_MAKE="$(MAKE) OUTDIR="` only to the boot wrapper. The wrapper starts the outer runtime Make with `env CFLAGS=... MAKE=...`; the inner upstream recipe inherits both values. Runtime receipts hash `0001`, adapter/wrapper content, and the final two contract values.

- [ ] **Step 1: Capture the required red probe evidence**

Before editing, build one clean profile-private runtime per architecture with command logging enabled:

```bash
make PROFILE=x86_64-clang clean
make PROFILE=x86_64-clang V=1 build/x86_64-clang/artifacts/uefi/BOOTX64.EFI 2>&1 | tee /tmp/uefi-x86-before.log
make PROFILE=aarch64-clang clean
make PROFILE=aarch64-clang V=1 aarch64-uefi 2>&1 | tee /tmp/uefi-aa64-before.log
```

Record the outer application compile, inner `uefi/` archive compile, object locations, and `-j1`. Do not delete `0002` until the replacement probe is ready.

- [ ] **Step 2: Define dedicated variables and receipt inputs**

In `uefi.mk`, set:

```make
UEFI_RUNTIME_CFLAGS ?= -DUEFI_NO_UTF8
UEFI_RUNTIME_MAKE ?= $(MAKE) OUTDIR=
UEFI_RUNTIME_ADAPTER_INPUT ?= $(OS01_ROOT)/mk/components/uefi.mk
UEFI_RUNTIME_WRAPPER_INPUT ?= $(OS01_ROOT)/boot/uefi/Makefile
UEFI_PATCHES := $(UEFI_PATCH_DIR)/0001-clang-int8.patch
```

Pass the first two names explicitly in the root-to-wrapper submake invocation; do not add generic `CFLAGS` to `OS01_SUBMAKE_ALLOWED`. Because `UEFI_RUNTIME_MAKE` contains a space, the root call must use shell-safe quoted assignments exactly as follows:

```make
@$(call os01_submake,boot/uefi,ARCH=$(UEFI_ARCH_FAMILY) \
  UEFI_RUNTIME_STAMP=$(UEFI_RUNTIME_STAMP) \
  UEFI_RUNTIME_CFLAGS='$(UEFI_RUNTIME_CFLAGS)' \
  UEFI_RUNTIME_MAKE='$(UEFI_RUNTIME_MAKE)' $(OS01_SUBMAKE_ARGS))
```

Use the same single-quote form in every contract fixture override. In the receipt digest, hash `thirdpart/posix-uefi.manifest`, `0001`, `$(UEFI_RUNTIME_ADAPTER_INPUT)`, `$(UEFI_RUNTIME_WRAPPER_INPUT)`, boot sources, compiler identity, and print the final `UEFI_RUNTIME_CFLAGS` and `UEFI_RUNTIME_MAKE` values. Production defaults always name the real adapter and wrapper; the two `*_INPUT` values exist only as build-contract digest fixtures and never change the Makefiles that execute. Apply only `0001` to the private copy.

- [ ] **Step 3: Start outer posix-uefi Make with the contract**

In `boot/uefi/Makefile`, preserve the x86 COFF compiler/linker arguments and serial `-j1`, but execute the runtime invocation in the wrapper as:

```make
env CFLAGS="$(UEFI_RUNTIME_CFLAGS)" MAKE="$(UEFI_RUNTIME_MAKE)" $(UEFI_MAKE)
```

The `MAKE` value applies only when upstream's `uefi/libuefi.a` recursively invokes `$(MAKE)`, clearing `OUTDIR` for runtime archive objects. The outer application invocation retains `OUTDIR=$(UEFI_BUILD_DIR)/`. Do the equivalent environment wrapping for aarch64 without adding x86 compiler arguments.

- [ ] **Step 4: Delete 0002 only for the fresh two-architecture probe**

After Steps 2–3 are complete, delete exactly `config/posix-uefi/0002-runtime-make-overlay.patch` with `apply_patch`, then immediately run both clean builds from Step 1 with fresh logs. Assert each log has an outer application compile and an inner `uefi/` compile containing `-DUEFI_NO_UTF8`; assert runtime archive objects are under `build/<profile>/uefi-runtime`, application objects under `build/<profile>/uefi`, and no non-serial upstream invocation occurs. Then verify:

```bash
test ! -e config/posix-uefi/0002-runtime-make-overlay.patch
test -z "$(git -C thirdpart/posix-uefi status --porcelain)"
```

Before deletion, capture the tracked patch's verbatim baseline with `git show HEAD:config/posix-uefi/0002-runtime-make-overlay.patch` for review. If either probe fails, restore only that file using `apply_patch` with the reviewed baseline content, retain the completed profile-only commits, record the observed propagation mismatch in `/tmp/uefi-x86-after.log` or `/tmp/uefi-aa64-after.log`, and stop this task for design review. Do not introduce `sed` or a replacement patch.

- [ ] **Step 5: Test receipt invalidation and commit**

In `tests/build_contract.sh`, copy the real adapter and wrapper to two temporary files, append distinct fixture comments to the copies, and run four real UEFI artifact builds with one changed input at a time: `UEFI_RUNTIME_CFLAGS`, `UEFI_RUNTIME_MAKE`, `UEFI_RUNTIME_ADAPTER_INPUT`, and `UEFI_RUNTIME_WRAPPER_INPUT`. Save the old receipt before each run. Each run must replace the receipt and print `runtime input changed, recopying`; no OS01 source file or submodule file is modified. The normal build immediately after the fixture must use the default real input paths again.

Run: `make PROFILE=x86_64-clang clean && make PROFILE=x86_64-clang disk.img validate`

Run: `make PROFILE=aarch64-clang clean && timeout 25 make PROFILE=aarch64-clang run-aarch64-uefi`

Expected: x86 validation succeeds; aarch64 emits `aarch64 uefi handoff ok` and `phase1 boot ok` before timeout.

```bash
git add mk/components/uefi.mk boot/uefi/Makefile config/posix-uefi tests/build_contract.sh
git commit -m "refactor(build): remove UEFI runtime overlay patch"
```

### Task 6: Migrate supported build documentation and run the release checks

**Files:**
- Modify: `docs/build/toolchain.md`
- Modify: `docs/build.md`
- Modify: `docs/build-run-debug.md`
- Modify: `docs/boot.md`
- Modify: `AGENTS.md`

**Interfaces:** Current operator documentation uses `make PROFILE=<name> <target>` and `make PROFILE=x86_64-clang print-run-paths`; historical plans, specs, and reports remain untouched.

- [ ] **Step 1: Add a failing documentation scan**

Run:

```bash
rg -n 'toolchain\.mk|make -C (boot/uefi|kernel|libc|user|test)|boot/uefi/OVMF\.fd' \
  docs/build/toolchain.md docs/build.md docs/build-run-debug.md docs/boot.md AGENTS.md
```

Expected: current operating instructions still report old standalone commands and source-tree firmware.

- [ ] **Step 2: Rewrite only current usage guidance**

Replace each component command with its equivalent root/profile command. Remove the standalone `toolchain.mk` section in `docs/build/toolchain.md`; retain only `mk/toolchains/clang.mk` and whitelist behavior. Replace firmware download instructions with `OVMF_FIRMWARE_SOURCE` examples and `print-run-paths`. In the AGENTS manual QEMU snippet, first show `make PROFILE=x86_64-clang print-run-paths`, then substitute the printed `firmware` and `image` values. Do not edit historical files below `docs/superpowers/{plans,specs}` or review reports.

- [ ] **Step 3: Run the complete acceptance suite**

Run:

```bash
sh tests/build_contract.sh x86_64-clang legacy
make PROFILE=x86_64-clang clean
make PROFILE=x86_64-clang disk.img validate test
make PROFILE=x86_64-clang test-build-contract-x86
make PROFILE=aarch64-clang clean
timeout 25 make PROFILE=aarch64-clang run-aarch64-uefi
git status --short
```

Expected: every build-contract mode succeeds; aarch64 prints both handoff markers before timeout; only intentional source changes are present and `thirdpart/posix-uefi` is clean.

- [ ] **Step 4: Commit**

```bash
git add docs/build/toolchain.md docs/build.md docs/build-run-debug.md docs/boot.md AGENTS.md tests/build_contract.sh
git commit -m "docs(build): document profile-only build workflow"
```
