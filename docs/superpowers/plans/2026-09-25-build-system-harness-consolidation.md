# Build System Harness Consolidation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Consolidate the Makefile `test-*` and `run*` targets, document the build-system harness design, and fix help-output bugs so newcomers see 6 test buckets + 3 run variants instead of 17 test-* + 3 run* + 6 undocumented targets.

**Architecture:** Two-layer strategy:
1. **Code consolidation** — add bucket targets (`test-qemu SUITE=...`, `test-host`, `test-static`, `test-aarch64 MODE=...`, `test-contract PROFILE=...`). Parameterized old names forward to the matching bucket; focused legacy checks retain their own recipe so their scope does not broaden. Alias deletion is tracked in a followup plan, not in this one.
2. **Documentation capture** — write `docs/build-system-harness.md` describing the taxonomy (build / run / debug / validate / test / contract / maintenance × capability gate × variant scope). Update `AGENTS.md` Quick start, `docs/build.md` 用户入口, and `docs/build-run-debug.md` test recipes to reference the new buckets.

**Tech Stack:** GNU Make, Python 3 (qemutests harness), shell (CI scripts), Markdown.

**Spec:** This plan IS the spec for the refactor; the harness-design decisions are captured in the resulting `docs/build-system-harness.md` (Task 9 deliverable).

## Global Constraints

- Capability gates (`require_capability`) must remain parse-time and capability-aware, matching the existing pattern in `mk/components/run.mk`.
- **Compatibility behavior preservation**: parameterized old names forward to a bucket only when its selected variant reproduces their behavior; focused checks keep their original recipe (e.g. `test-runtime` runs 4 Python audits, while `test-static` runs 6).
- **Recipe-line discipline**: any recipe that contains `$(MAKE)` must put the sub-make invocation on its own recipe line, separate from the python/sh harness invocation. GNU Make executes lines containing `$(MAKE)` even under `-n`. This applies to:
  - `$(MAKE)` inside a shell `case ... $(MAKE) ...; python3 ...;;` block — still executes under `-n`.
  - `$(MAKE)` inside a shell `if ... then $(MAKE) ...; fi` block on one recipe line — still executes under `-n`.
  - `+env -i ... $(MAKE) ...` inside a shell `case` branch — `+` becomes a command name, fails with `+env: not found`; the line also has `$(MAKE)` so `-n` executes it.
  - The fix is always to split into separate Make targets with one operation per recipe line.
- **Make variables, not shell variables, for cross-recipe-line state.** Anything set in one `@`-prefixed recipe line is empty in the next (each `@` line is a fresh `/bin/sh -c`). Use `$(TEST_AARCH64_EXTRA_$(MODE))` etc., not `extra=...; ... $$extra`.
- **No image-path prerequisites.** A variant image path (e.g. `$(TEST_SYSTEST_IMAGE)`) has no direct build rule — the build chain goes through `image` (phony) → `$(DISK_IMG)` (variable) → rule in image.mk. Listing the raw path as a prereq fails with `No rule to make target`. Trigger the variant build via `$(MAKE) <flavor> image` on a recipe line.
- **No `define`/function needed for SUITE→image lookup.** A per-SUITE Make variable (`TEST_QEMU_IMG_$(SUITE)`) is enough. Avoid the `$(word 2, ...)` anti-pattern (returns empty for one-word filter match).
- **QEMU command lines**: preserve the original QEMU argument order and values at run.mk lines 65-72, 77-85, 90-97, 102-110. In particular, `-accel kvm` precedes `-netdev`, NIC selection precedes the disk drive, and only `run`/`run-kvm` end with `-no-reboot`.
- **Bucket build prerequisites**: `test-contract PROFILE=x86_64-clang` must depend on `disk.img`; `PROFILE=aarch64-clang` must depend on `aarch64-uefi`. Dropping these breaks the CI contract job which starts with an empty workspace.
- **`make help` output**: prominently show the 6 canonical test buckets (`test-qemu`, `test-host`, `test-static`, `test-kernel-selftest`, `test-aarch64`, `test-contract`), 3 run variants, 1 debug, 4 validate, and 2 maintenance targets. Show the three distinct standalone tests and the three previously hidden focused compatibility checks in a separate section; document other legacy aliases in the harness guide rather than filling the primary help list with them.
- **Alias deletion is NOT in this plan.** Aliases are tracked for deletion in a followup plan (`docs/superpowers/plans/2026-09-25-delete-test-aliases.md`) created in Task 12 Step 2.
- Maintainer scripts (`qemutests/build_contract.sh`, `qemutests/run_test.py`) stay source-compatible: their CLI surface (subcommand names, flags) does not change in this refactor.

## Review Focus

Five failure modes not pinned by any task's tests, with the task that owns the test pinning them:
1. A capability-gated recipe that worked on `x86_64-clang` silently regresses to a missing-target error on `aarch64-clang` (or vice versa). Covered by the dry-run check in Task 10's verification step.
2. A parameterized alias failing to thread the right SUITE/MODE/PROFILE value, or a focused compatibility target silently broadening to the umbrella. Covered by the per-suite smoke test in Task 4, per-mode smoke test in Task 7, and audit count comparison in Task 6.
3. A hidden `.PHONY` target (one of the six not currently in help) is removed by alias deletion but actually depended on by an external CI script. Covered by the grep-for-deprecated-names step in Task 11; deletion is deferred to a followup plan so this risk is time-bounded, not eliminated in this PR.
4. The new `docs/build-system-harness.md` describes a target that does not exist or describes one that no longer exists. Covered by Task 12's `make help`-vs-doc diff and the bucket-count consistency grep in Task 10 Step 4.
5. `make` with no goal changes behavior, or a recipe places `$(MAKE)` on the same shell line as a harness (causing `make -n` to execute that harness), or `+env` lands inside a shell branch. Covered by Task 2's default-goal verification, the separate prep/run recipe lines in Tasks 4, 7, and 8, and Task 12's dry-run command inspection.

---

### Task 1: Fix help output bugs and document hidden targets

**Files:**
- Modify: `mk/components/run.mk:229-249,494-588` (validation gates and the `help:` recipe)
- Test: manual inspection of `make help` output

**Interfaces:**
- Consumes: existing `PROFILE_CAPABILITIES`, existing target list
- Produces: x86-only validation targets fail cleanly on AArch64; matching capability badges; six previously-undocumented targets now visible

- [ ] **Step 1: Make x86-only validation targets require rootfs**

The `validate-kernel` recipe inspects `kernel.bin` and x86 kernel symbols; `aarch64-clang` has `uefi` but not `rootfs` or the required x86 artifact/tool variables. Replace the three target headers and gate lines at lines 230-247 with:
```makefile
validate: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),validate-kernel validate-uefi validate-profile)
	$(call require_capability,rootfs)
validate-kernel: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),kernel.bin)
	$(call require_capability,rootfs)
# Keep the existing validate-kernel inspection commands here.
validate-uefi: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(UEFI_EFI))
	$(call require_capability,rootfs)
# Keep the existing validate-uefi inspection commands here.
```
Keep `validate-profile` ungated. On AArch64, all three x86 validation targets must produce the capability error before evaluating x86 prerequisites.

- [ ] **Step 2: Keep the matching rootfs badges in help**

The badges at `mk/components/run.mk` lines 546-554 already say:
```text
'validate'          '(rootfs)'     'kernel ELF + UEFI COFF + profile info';
'validate-kernel'   '(rootfs)'     'kernel ELF sanity (no undef, EM_X86_64, exports)';
'validate-uefi'     '(rootfs)'     'EFI app COFF exports parseable';
'validate-profile'  '(always)'     'Print profile / triple / sysroot / capabilities';
```
Keep them as `(rootfs)`, matching Step 1's corrected gates. Change the section heading from `Validation (x86 uefi):` to `Validation (x86 rootfs):`. The prior proposal to display `(uefi)` was misleading: AArch64 has that capability, but these recipes cannot validate its artifacts.

- [ ] **Step 3: Add the six undocumented test targets to help**

Append to the existing `Test (x86, rootfs):` block in the `help:` recipe (between line 570 and 571), preserving the `%−22s %−13s` column format:
```text
@printf '  %-22s %-13s %s\n' \
       'test-syscall-repeat'  '(rootfs)'   'QEMU exec/exit stability through normal terminal (x86_64_systest_repeat.py)'; \
@printf '  %-22s %-13s %s\n' \
       'test-user-canary'     '(rootfs)'   '7-step SSP / crt0 user-stack canary audit (spec 2026-09-17)'; \
@printf '  %-22s %-13s %s\n' \
       'test-kernel-layout'   '(rootfs)'   'x86_64 kernel.elf layout audit (post-_end reserved)'; \
@printf '  %-22s %-13s %s\n' \
       'test-kernel-canary-contract' '(rootfs)' 'Kernel canary compile-flag contract'; \
@printf '  %-22s %-13s %s\n' \
       'test-pmm-boot-reservation'    '(rootfs)' 'PMM boot-time memory reservation guard';
```
Add a new block right after the `aarch64 UEFI bring-up` block:
```text
@echo 'aarch64 subsystem tests (uefi):'
@printf '  %-22s %-13s %s\n' \
       'test-aarch64-gic-spi'  '(uefi)'     'PL011 RX → GIC SPI injection (qemutests/aarch64_gic_spi.py)';
```

- [ ] **Step 4: Move `print-run-paths` to a new "Inspection" group**

In the help recipe, remove the `print-run-paths` printf at line 531-532 from the `Run / Debug (x86, rootfs):` block. After the validation block, add a new section:
```text
@echo 'Inspection:'
@printf '  %-22s %-13s %s\n' \
       'print-run-paths'   '(rootfs)'     'Print absolute firmware + active image path';
```

- [ ] **Step 5: Verify and commit**

Run: `make help`
Expected: 6 new target lines visible, rootfs validation badges retained, `print-run-paths` relocated.
Run: `make -n PROFILE=aarch64-clang validate-kernel` and `make -n PROFILE=aarch64-clang validate-uefi` (both must fail with `lacks capability 'rootfs'`, without malformed tool commands).
Run: `make disk.img` (sanity — recipe not touched).
Run: `git diff -- mk/components/run.mk` — confirm only the three validation gates/prerequisite filters and help text changed.
Commit:
```bash
git add mk/components/run.mk
git commit -m "fix(make): gate x86 validation on rootfs and expose hidden test targets"
```

---

### Task 2: Delete the `all: disk.img` alias

**Files:**
- Modify: `Makefile:55` (the `all: disk.img` line)
- Modify: `mk/components/run.mk:510` (the help entry that advertises `all`)

**Interfaces:**
- Consumes: `.DEFAULT_GOAL := disk.img` at line 54
- Produces: bare `make` continues to build `disk.img`; help advertises only the surviving `disk.img` target

- [ ] **Step 1: Delete the alias**

In `Makefile`, delete line 55 (`all: disk.img`).
In `mk/components/run.mk` help, change the target label from `all / disk.img` to `disk.img`.

- [ ] **Step 2: Verify default goal still works**

Run: `make -n` (dry run)
Expected: output shows the `disk.img` recipe being planned (or its prerequisites); no "no rule to make target" error.
Run: `make help`
Expected: the build entry shows `disk.img`, with no `all / disk.img` label.
Run: `git diff -- Makefile mk/components/run.mk` — expect the alias deletion and help label update.
Commit:
```bash
git add Makefile mk/components/run.mk
git commit -m "refactor(makefile): drop unused 'all' alias; disk.img is the default goal"
```

---

### Task 3: Extract RUN_QEMU_BASE to DRY run/run-kvm/run-virtio recipes

**Files:**
- Modify: `mk/components/run.mk:62-110` (the four run/debug recipes)

**Interfaces:**
- Consumes: existing variables `QEMU_BIN`, `OVMF_FIRMWARE`, `NORMAL_IMAGE`, `MEMORY`, `DISPLAY`, `SMP`, `OS01_ROOT` (no change)
- Produces: new variable `RUN_QEMU_BASE` consumed by all four recipes; identical runtime behavior

- [ ] **Step 1: Add the variable definition**

Before line 62 (`.PHONY: run`), insert:
```makefile
# Shared QEMU argument groups. Variant flags are inserted between these
# groups so the final argument order matches each original recipe.
RUN_QEMU_BASE = $(QEMU_BIN) -M q35 -smp $(SMP) \
  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_FIRMWARE)
RUN_QEMU_DISK = -drive file=$(NORMAL_IMAGE),format=raw,if=none,id=disk \
  -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
  -object rng-random,filename=/dev/urandom,id=rng0 \
  -device virtio-rng-pci,rng=rng0 \
  -m $(MEMORY) -display $(DISPLAY) -serial stdio
```

- [ ] **Step 2: Save the original dry-run QEMU commands**

Before replacing any recipe, run:
```bash
for target in run run-kvm run-virtio debug; do
  make -n "$target" > "/tmp/os01-qemu-before-$target.txt"
done
```

- [ ] **Step 3: Rewrite the four recipes to use RUN_QEMU_BASE**

Replace lines 62-110 (the four `.PHONY:` + recipe blocks for `run`, `run-kvm`, `run-virtio`, `debug`) with:
```makefile
.PHONY: run run-kvm run-virtio debug
# Flags before the common network/disk section, followed by the original
# final reboot flag only on run and run-kvm.
RUN_QEMU_FLAGS_run-kvm    = -accel kvm
RUN_QEMU_FLAGS_debug      = -S -s

# Capability-gated prerequisites reproduce the original
# `$(if $(filter rootfs,...),$(NORMAL_IMAGE) $(OVMF_FIRMWARE))` pattern
# so a clean workspace still gets the disk image + firmware built first.
run:        $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(NORMAL_IMAGE) $(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	$(RUN_QEMU_BASE) -netdev user,id=net0 -device e1000e,netdev=net0 $(RUN_QEMU_DISK) -no-reboot
run-kvm:    $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(NORMAL_IMAGE) $(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	$(RUN_QEMU_BASE) $(RUN_QEMU_FLAGS_run-kvm) -netdev user,id=net0 -device e1000e,netdev=net0 $(RUN_QEMU_DISK) -no-reboot
run-virtio: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(NORMAL_IMAGE) $(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	$(RUN_QEMU_BASE) -netdev user,id=net0 -device virtio-net-pci,netdev=net0 $(RUN_QEMU_DISK)
debug:      $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(NORMAL_IMAGE) $(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	$(RUN_QEMU_BASE) $(RUN_QEMU_FLAGS_debug) -netdev user,id=net0 -device e1000e,netdev=net0 $(RUN_QEMU_DISK)
```

- [ ] **Step 4: Verify all four targets work end-to-end**

Run: `make -n run` (dry run, expect the QEMU command line with e1000e, no kvm).
Run: `make -n run-kvm` (expect `-accel kvm -device e1000e`).
Run: `make -n run-virtio` (expect `-device virtio-net-pci`, no `-no-reboot`).
Run: `make -n debug` (expect `-S -s`, no `-no-reboot`).

Compare the complete ordered QEMU argv for each target against the baseline
captured in Step 2. The extractor joins all continued lines before splitting
shell words; comparing only the first line would miss the NIC and final flags.
```bash
for target in run run-kvm run-virtio debug; do
  make -n "$target" > "/tmp/os01-qemu-after-$target.txt"
done
python3 - <<'PY'
from pathlib import Path
import shlex

def qemu_argv(path):
    lines = Path(path).read_text().splitlines()
    start = next(i for i, line in enumerate(lines) if ' -M q35 -smp ' in line)
    command = []
    for line in lines[start:]:
        continued = line.rstrip().endswith('\\')
        command.append(line.rstrip().removesuffix('\\'))
        if not continued:
            break
    return shlex.split(' '.join(command))

for target in ('run', 'run-kvm', 'run-virtio', 'debug'):
    before = qemu_argv(f'/tmp/os01-qemu-before-{target}.txt')
    after = qemu_argv(f'/tmp/os01-qemu-after-{target}.txt')
    assert before == after, (target, before, after)
    print(f'{target}: ordered QEMU argv unchanged')
PY
```
Then run a 30s real QEMU session for one of them (`timeout 5 make run` — expect QEMU to start; kill after 5s).
Commit:
```bash
git add mk/components/run.mk
git commit -m "refactor(run): DRY QEMU command lines via RUN_QEMU_BASE / _FLAGS_<target>"
```

---

### Task 4: Implement `test-qemu` bucket with SUITE= flag (covers 4 E2E targets)

**Files:**
- Modify: `mk/components/run.mk:280-360` (the `test`, `test-phase-0`, `test-syscall`, `test-inittab`, `test-network`, `test-syscall-repeat` recipes)
- Modify: `qemutests/run_test.py:371-395` (no change required — already supports `phase-0 | systest | inittab-phase | network` via `test_name` arg)

**Interfaces:**
- Consumes: `run_test.py` `test_name` positional (already there)
- Produces: `test-qemu SUITE=<name>` that picks the right variant image, builds it if missing, and runs `run_test.py <name>`. Old targets (`test-phase-0`, `test-syscall`, `test-inittab`, `test-network`) become one-line aliases forwarding to `test-qemu` with the right SUITE.

- [ ] **Step 1: Define the SUITE→image lookup (Make variables, not shell vars)**

After line 273 (after the `TEST_*_IMAGE` definitions), insert:
```makefile
# Per-SUITE lookups, used by test-qemu to pick the right variant build
# flavor and the right image path. These are Make variables so they
# resolve at parse time and survive across recipe lines.
TEST_QEMU_FLAVOR_phase-0       =
TEST_QEMU_FLAVOR_systest       = OS01_SYSTEST=1
TEST_QEMU_FLAVOR_inittab-phase = INITTAB_FILE=config/inittab.test
TEST_QEMU_FLAVOR_network       = OS01_NETTEST=1
TEST_QEMU_IMG_phase-0       = $(NORMAL_IMAGE)
TEST_QEMU_IMG_systest       = $(TEST_SYSTEST_IMAGE)
TEST_QEMU_IMG_inittab-phase = $(TEST_INITTAB_IMAGE)
TEST_QEMU_IMG_network       = $(TEST_NETTEST_IMAGE)
```

No `define`/function is needed: a per-SUITE variable lookup
(`$(TEST_QEMU_FLAVOR_$(SUITE))`) is enough and survives Make's
expansion stages, unlike a shell variable that would be lost between
recipe lines.

- [ ] **Step 2: Implement `test-qemu` and the four aliases**

Replace the `test-phase-0`, `test-syscall`, `test-inittab`, `test-network` recipe blocks (currently lines 290-359) with:
```makefile
.PHONY: test-qemu test-phase-0 test-syscall test-inittab test-network
# Use the per-SUITE Make variables from Step 1. The image path is
# informational — it is NOT used as a prerequisite, because the
# variant image path has no direct build rule (the chain is `image` (phony)
# → `$(DISK_IMG)` (variable) → rule in mk/components/image.mk). Listing
# the raw image path as a prereq fails with "No rule to make target"
# (verified: `make -n test-qemu SUITE=systest` errors with the systest
# variant path). The variant build is triggered by a `$(MAKE) ... image`
# sub-make inside the recipe.
# Preserve the old syscall/selftest exclusion at parse time for both
# invocation forms. This check must run before any prerequisites are built.
ifneq ($(filter test-syscall,$(MAKECMDGOALS)),)
ifeq ($(KERNEL_SELFTEST),1)
$(error ERROR: syscall E2E must not be combined with KERNEL_SELFTEST=1)
endif
endif
ifneq ($(filter test-qemu,$(MAKECMDGOALS)),)
ifeq ($(SUITE),systest)
ifeq ($(KERNEL_SELFTEST),1)
$(error ERROR: syscall E2E must not be combined with KERNEL_SELFTEST=1)
endif
endif
endif

test-qemu: SUITE ?= phase-0
test-qemu: SUITE := $(SUITE)
# Firmware remains a prerequisite for every suite. No image-path prereq:
# the variant image is built by the recursive make below.
test-qemu: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	@case "$(SUITE)" in \
	  phase-0|systest|inittab-phase|network) ;; \
	  *) echo "SUITE must be phase-0|systest|inittab-phase|network, got '$(SUITE)'" >&2; exit 1;; \
	esac
	@echo "  [test-qemu] SUITE=$(SUITE) flavor=$(TEST_QEMU_FLAVOR_$(SUITE)) img=$(TEST_QEMU_IMG_$(SUITE))"
	@if [ "$(SUITE)" != "phase-0" ] && [ -f "$(NORMAL_IMAGE)" ]; then \
	  sha256sum "$(NORMAL_IMAGE)" > "$(NORMAL_IMAGE_DIR)/normal.before"; \
	fi
	$(MAKE) $(TEST_QEMU_FLAVOR_$(SUITE)) image
	@if [ "$(SUITE)" != "phase-0" ] && [ -f "$(NORMAL_IMAGE_DIR)/normal.before" ]; then \
	  sha256sum "$(NORMAL_IMAGE)" > "$(NORMAL_IMAGE_DIR)/normal.after"; \
	  cmp "$(NORMAL_IMAGE_DIR)/normal.before" "$(NORMAL_IMAGE_DIR)/normal.after"; \
	fi
	DISK_IMG="$(TEST_QEMU_IMG_$(SUITE))" \
	OVMF_FIRMWARE="$(OVMF_FIRMWARE)" \
	python3 qemutests/run_test.py $(SUITE)
test-phase-0:    ; @$(MAKE) test-qemu SUITE=phase-0
test-syscall:    ; @$(MAKE) test-qemu SUITE=systest
test-inittab:    ; @$(MAKE) test-qemu SUITE=inittab-phase
test-network:    ; @$(MAKE) test-qemu SUITE=network
```

**Why this layout (recipe-line discipline + variant build contract):**
- The variant image is built via `$(MAKE) $(TEST_QEMU_FLAVOR_$(SUITE)) image`
  on its own recipe line, matching the original `test-syscall` pattern at
  run.mk line 311. GNU Make executes any recipe line containing `$(MAKE)`
  even under `-n`; the run.mk comment at lines 263-266 documents this as
  the standard recursive-make contract. The python harness call is on the
  *next* recipe line and is therefore NOT executed under `-n`.
- The flavor and image lookups are **Make variables**, not shell
  variables. A shell variable set in one `@`-prefixed recipe line does
  NOT survive into the next line (each `@` line is a fresh /bin/sh
  invocation, verified: `extra=...` set in one line is empty in the next).
  Using `$(TEST_QEMU_FLAVOR_$(SUITE))` ensures the flavor is resolved at
  Make parse time and inlined into the `$(MAKE)` command.
- No image-path prerequisite. The variant path is informational only;
  listing it as a prereq fails because Make has no rule to build it
  directly (the build chain goes through the `image` phony and the
  `$(DISK_IMG)` variable in image.mk).

- [ ] **Step 3: Verify all four aliases + the new bucket work**

Run: `make -n test-qemu` (expect the firmware prerequisite, `$(MAKE) image`, then a printed `run_test.py phase-0` line).
Run: `make -n test-qemu SUITE=systest` (expect `$(MAKE) OS01_SYSTEST=1 image` line + `python3 qemutests/run_test.py systest`).
Run: `make -n test-phase-0` (expect forwarding to test-qemu SUITE=phase-0).
Run: `make -n test-syscall` (expect forwarding to test-qemu SUITE=systest).
Run: `make -n test-network` (expect OS01_NETTEST=1 + run_test.py network).
Run: `make KERNEL_SELFTEST=1 test-qemu SUITE=systest` and `make KERNEL_SELFTEST=1 test-syscall` (both must fail at parse time, before any build).

Confirm the variant image is NOT a prereq (must not error):
```bash
make -n test-qemu SUITE=systest 2>&1 | grep -c 'No rule to make target'   # expect 0
```

Confirm the dry run prints the Python harness and keeps its invocation on a
recipe line separate from the recursive Make. GNU Make strips recipe prefixes
such as `+` from printed commands, so matching `^\+` cannot distinguish
execution from printing:
```bash
make -n test-qemu SUITE=systest > /tmp/os01-test-qemu-dry.txt
grep -q '^make OS01_SYSTEST=1 image' /tmp/os01-test-qemu-dry.txt
grep -q 'python3 qemutests/run_test.py systest' /tmp/os01-test-qemu-dry.txt
```

Run a real quick smoke: `timeout 90 make test-phase-0` — should boot to shell prompt as before.
Commit:
```bash
git add mk/components/run.mk
git commit -m "refactor(test): consolidate 4 QEMU E2E targets behind test-qemu SUITE=<name>; keep old names as aliases"
```

---

### Task 5: Rename `test` to `test-host`; hide `test-pmm-boot-reservation` as internal

**Files:**
- Modify: `mk/components/run.mk:280-289`

**Interfaces:**
- Consumes: `os01_submake,hosttests,run`, `qemutests/pmm_boot_reservation_test.py`
- Produces: `test-host` is the canonical name; `test` becomes an alias for backward compat (since `make test` is in old docs); `test-pmm-boot-reservation` becomes a phony-only target with no .PHONY visibility tweak (kept as alias of test-host's pmm step).

- [ ] **Step 1: Rename the `test` target and add the alias**

Replace lines 280-289 with:
```makefile
.PHONY: test-host test test-pmm-boot-reservation
test-host:
	$(call require_capability,rootfs)
	@$(call os01_submake,hosttests,run $(OS01_SUBMAKE_ARGS))
	python3 qemutests/pmm_boot_reservation_test.py
test: test-host
test-pmm-boot-reservation:
	python3 qemutests/pmm_boot_reservation_test.py
```

- [ ] **Step 2: Verify**

Run: `make -n test-host` (expect hosttests sub-make + pmm script).
Run: `make -n test` (expect forwarding to test-host).
Run: `make -n test-pmm-boot-reservation` (expect just the pmm script line).
Run a real quick: `make test-pmm-boot-reservation` (should pass; the python script runs standalone).
Commit:
```bash
git add mk/components/run.mk
git commit -m "refactor(test): canonical name 'test-host'; 'test' becomes alias; split pmm helper"
```

---

### Task 6: Consolidate 4 static audits into `test-static`

**Files:**
- Modify: `mk/components/run.mk:364-432` (the `test-runtime`, `test-user-canary`, `test-kernel-layout`, `test-kernel-canary-contract` recipes)

**Interfaces:**
- Consumes: `KERNEL_ARTIFACT`, `KERNEL_ELF`, `KERNEL_BUILD_DIR`, `KERNEL_RUNTIME_LINK_RECEIPT`, `KERNEL_RUNTIME_INPUTS`, `LLVM_NM`, `LLVM_READOBJ`, `LLVM_READELF`, `LLVM_OBJDUMP`, `SYSROOT`, `OS01_PROFILE_FILE`, `USER_ARTIFACTS`, `USER_ARTIFACT_DIR`, `ROOTFS_MANIFEST` (no change)
- Produces: `test-static` runs all four audits sequentially; old targets become aliases.

- [ ] **Step 1: Define the `test-static` recipe and the four subset aliases**

Replace lines 364-432 (and the separate `test-kernel-layout` / `test-kernel-canary-contract` recipes at lines 485-492) with the following. **Critical rule:** each alias reproduces the **exact original recipe** of its target, not a forwarding to `test-static` — alias behavior must not change. CI currently calls these aliases individually; migrating each one to run the umbrella `test-static` would silently change behavior.

```makefile
# test-static = the umbrella: runs every static audit in one shot.
# Each alias below reproduces the recipe of the corresponding legacy target
# so that `make test-runtime` continues to run only the runtime-audit subset.
.PHONY: test-static test-runtime test-kernel-layout test-kernel-canary-contract test-user-canary

# ── test-static: all 8 audits ─────────────────────────────────
test-static: $(KERNEL_ARTIFACT)
	$(call require_capability,rootfs)
	python3 qemutests/runtime_audit.py \
	  --stage1 "$(KERNEL_BUILD_DIR)/kernel.elf.stage1" \
	  --final "$(KERNEL_ELF)" \
	  --link-receipt "$(KERNEL_RUNTIME_LINK_RECEIPT)" \
	  --runtime-input "$(KERNEL_RUNTIME_INPUTS)" \
	  --llvm-nm "$(LLVM_NM)" \
	  --llvm-readobj "$(LLVM_READOBJ)"
	python3 qemutests/stack_canary_audit.py \
	  --object "$(KERNEL_BUILD_DIR)/sched/task.o" \
	  --elf "$(KERNEL_ELF)" \
	  --llvm-readelf "$(LLVM_READELF)" \
	  --llvm-objdump "$(LLVM_OBJDUMP)"
	@$(MAKE) --no-print-directory validate-kernel
	python3 qemutests/runtime_link_order_test.py
	python3 qemutests/kernel_runtime_link_test.py \
	  --source-receipt "$(KERNEL_RUNTIME_LINK_RECEIPT)" \
	  --sysroot "$(SYSROOT)" \
	  --profile-file "$(OS01_PROFILE_FILE)" \
	  --profile "$(PROFILE)"
	python3 qemutests/x86_64_kernel_layout_test.py "$(KERNEL_BUILD_DIR)/kernel.elf" \
	  --llvm-nm "$(LLVM_NM)" --llvm-readelf "$(LLVM_READELF)"
	python3 qemutests/kernel_canary_contract_test.py
	@$(MAKE) --no-print-directory test-user-canary

# ── test-runtime: original recipe (lines 364-385 of run.mk) ──
test-runtime: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(KERNEL_ARTIFACT))
	$(call require_capability,rootfs)
	python3 qemutests/runtime_audit.py \
	  --stage1 "$(KERNEL_BUILD_DIR)/kernel.elf.stage1" \
	  --final "$(KERNEL_ELF)" \
	  --link-receipt "$(KERNEL_RUNTIME_LINK_RECEIPT)" \
	  --runtime-input "$(KERNEL_RUNTIME_INPUTS)" \
	  --llvm-nm "$(LLVM_NM)" \
	  --llvm-readobj "$(LLVM_READOBJ)"
	python3 qemutests/stack_canary_audit.py \
	  --object "$(KERNEL_BUILD_DIR)/sched/task.o" \
	  --elf "$(KERNEL_ELF)" \
	  --llvm-readelf "$(LLVM_READELF)" \
	  --llvm-objdump "$(LLVM_OBJDUMP)"
	@$(MAKE) --no-print-directory validate-kernel
	python3 qemutests/runtime_link_order_test.py
	python3 qemutests/kernel_runtime_link_test.py \
	  --source-receipt "$(KERNEL_RUNTIME_LINK_RECEIPT)" \
	  --sysroot "$(SYSROOT)" \
	  --profile-file "$(OS01_PROFILE_FILE)" \
	  --profile "$(PROFILE)"

# ── test-kernel-layout: original recipe (line 485-488) ──────
test-kernel-layout: kernel.bin
	python3 qemutests/x86_64_kernel_layout_test.py "$(KERNEL_BUILD_DIR)/kernel.elf" \
	  --llvm-nm "$(LLVM_NM)" --llvm-readelf "$(LLVM_READELF)"

# ── test-kernel-canary-contract: original recipe (line 490-492) ─
test-kernel-canary-contract:
	python3 qemutests/kernel_canary_contract_test.py

# ── test-user-canary: original recipe (lines 393-431) ───────
test-user-canary: $(if $(filter userland,$(PROFILE_CAPABILITIES)),$(USER_ARTIFACTS) $(USER_ARTIFACT_DIR)/busybox.elf $(ROOTFS_MANIFEST))
	$(call require_capability,rootfs)
	@set -e; \
	echo "[user-canary] 1/7 libc.a defines guard+fail"; \
	test -n "$$($(LLVM_NM) -P $(SYSROOT)/usr/lib/libc.a | awk '$$1=="__stack_chk_guard" && ($$2=="B" || $$2=="D")')" \
	  || { echo "ERROR: __stack_chk_guard not defined (B/D) in $(SYSROOT)/usr/lib/libc.a"; exit 1; }; \
	test -n "$$($(LLVM_NM) -P $(SYSROOT)/usr/lib/libc.a | awk '$$1=="__stack_chk_fail" && $$2=="T")" \
	  || { echo "ERROR: __stack_chk_fail not defined (T) in $(SYSROOT)/usr/lib/libc.a"; exit 1; }; \
	echo "[user-canary] 2/7 libk.a does NOT define them (kernel owns its own)"; \
	test -z "$$($(LLVM_NM) -P $(SYSROOT)/usr/lib/libk.a | awk '$$1=="__stack_chk_guard" || $$1=="__stack_chk_fail"')" \
	  || { echo "ERROR: libk.a must not carry SSP symbols"; exit 1; }; \
	echo "[user-canary] 3/7 user ELFs link SSP in (T __stack_chk_fail)"; \
	for e in systest canary_dump canary_smash; do \
	  $(LLVM_NM) -P $(USER_ARTIFACT_DIR)/$$e.elf \
	    | awk '$$1=="__stack_chk_fail" && $$2=="T" {f=1} END {exit !f}' \
	    || { echo "ERROR: $$e.elf has no resolved __stack_chk_fail"; exit 1; }; \
	done; \
	echo "[user-canary] 4/7 busybox.elf links SSP in"; \
	$(LLVM_NM) -P $(USER_ARTIFACT_DIR)/busybox.elf \
	  | awk '$$1=="__stack_chk_fail" && $$2=="T" {f=1} END {exit !f}' \
	  || { echo "ERROR: busybox.elf has no resolved __stack_chk_fail"; exit 1; }; \
	echo "[user-canary] 5/7 SSP flags in all three compile switches"; \
	for f in libc/Makefile user/Makefile config/busybox.config.in; do \
	  grep -q -- "-fstack-protector-strong" $$f \
	    || { echo "ERROR: $$f lacks -fstack-protector-strong"; exit 1; }; \
	done; \
	if grep -v "^LIBK_CFLAGS" libc/Makefile | grep -q -- "-fno-stack-protector"; then \
	  echo "ERROR: stray -fno-stack-protector in libc/Makefile (non-LIBK line)"; exit 1; \
	fi; \
	echo "[user-canary] 6/7 probe programs staged in rootfs manifest"; \
	for b in canary_dump canary_smash; do \
	  grep -q "/bin/$$b" $(ROOTFS_MANIFEST) \
	    || { echo "ERROR: /bin/$$b missing from $(ROOTFS_MANIFEST)"; exit 1; }; \
	done; \
	echo "[user-canary] 7/7 crt0 overlay byte-identity invariant"; \
	cmp -s user/crt0.S config/busybox.overlay/applets/crt0.S \
	  || { echo "ERROR: user/crt0.S and busybox overlay crt0.S diverged"; exit 1; }; \
	echo "[user-canary] audit passed"
```

Why each alias has its own recipe body (rather than `@$(MAKE) test-static`):
- The original `test-runtime` recipe is 5 audit steps. Aliasing it to
  `test-static` would make it run 8 steps (also layout, canary-contract,
  user-canary). CI calls `test-runtime` to gate the runtime-audit
  subset; broadening it changes what CI checks.
- Each subset recipe is small and self-contained — duplication is
  cheaper than a flag dispatch and matches the existing one-target-
  one-recipe shape of the surrounding file.

- [ ] **Step 2: Verify**

Run: `make -n test-static` (expect the umbrella — counts below).
Run: `make -n test-runtime` (expect the original subset — counts below).
Run: `make -n test-kernel-layout` (expect exactly 1 python script invocation).
Run: `make -n test-kernel-canary-contract` (expect exactly 1 python script invocation).
Run: `make -n test-user-canary` (expect the 7-step canary shell recipe, no python calls).

Count the python audit calls per target (must match these expected
numbers; if not, the umbrella or the alias has drifted):
```bash
# test-static umbrella = runtime_audit + stack_canary_audit + runtime_link_order
#                         + kernel_runtime_link + kernel_layout + kernel_canary_contract
#                       = 6 python calls (validate-kernel and test-user-canary
#                         are forwarded via $(MAKE), not python)
make -n test-static 2>&1 | grep -cE 'python3 qemutests/'   # expect 6

# test-runtime original subset = runtime_audit + stack_canary_audit
#                               + runtime_link_order + kernel_runtime_link
#                             = 4 python calls (validate-kernel is $(MAKE))
make -n test-runtime 2>&1 | grep -cE 'python3 qemutests/'   # expect 4

# The umbrella has strictly MORE python calls than the runtime alias:
test $(make -n test-static 2>&1 | grep -cE 'python3 qemutests/') \
     -gt $(make -n test-runtime 2>&1 | grep -cE 'python3 qemutests/')   # expect 0 (exit 0)
```

Run a smoke: `make test-user-canary` (should pass; only needs userland artifacts).
Commit:
```bash
git add mk/components/run.mk
git commit -m "refactor(test): introduce test-static umbrella; preserve 4 subset aliases with their original recipes"
```

---

### Task 7: Implement `test-aarch64` bucket with MODE= flag

**Files:**
- Modify: `mk/components/run.mk:169-219` (the `test-aarch64-uefi-smp`, `test-aarch64-uefi-smp-no-ack`, `test-aarch64-gic-spi` recipes)

**Interfaces:**
- Consumes: `AARCH64_UEFI_*`, `AARCH64_QEMU`, `AARCH64_SMP_TEST_NO_ACK_CPU`, `AARCH64_UEFI_SMP_DIAGNOSTIC_DTB`, `OS01_ROOT` (no change)
- Produces: `test-aarch64 MODE=<smp|no-ack|gic-spi>` selects the right invocation. Old targets remain aliases.

- [ ] **Step 1: Define MODE→args mapping**

Replace lines 169-219 with:
```makefile
.PHONY: test-aarch64 test-aarch64-uefi-smp test-aarch64-uefi-smp-no-ack test-aarch64-gic-spi
# The contract harness expects invalid no-ack injection to fail even under
# `make -n`. Keep this as a parse-time check for both invocation forms.
ifneq ($(filter test-aarch64-uefi-smp-no-ack,$(MAKECMDGOALS)),)
ifneq ($(AARCH64_SMP_TEST_NO_ACK_CPU),1)
$(error AARCH64_SMP_TEST_NO_ACK_CPU must be 1; clean and build the injected image first)
endif
endif
ifneq ($(filter test-aarch64,$(MAKECMDGOALS)),)
ifeq ($(MODE),no-ack)
ifneq ($(AARCH64_SMP_TEST_NO_ACK_CPU),1)
$(error AARCH64_SMP_TEST_NO_ACK_CPU must be 1; clean and build the injected image first)
endif
endif
endif
# Per-MODE lookups. The "extra" DTB flag is a Make variable (NOT a shell
# variable) so it survives across recipe lines — a shell variable set
# in one `@`-prefixed recipe line is empty in the next (each `@` line is
# a fresh /bin/sh invocation, verified).
TEST_AARCH64_EXTRA_smp     := $(if $(filter 0,$(AARCH64_UEFI_SMP_DIAGNOSTIC_DTB)),,--diagnostic-dtb=auto)
TEST_AARCH64_EXTRA_no-ack  := $(if $(filter 0,$(AARCH64_UEFI_SMP_DIAGNOSTIC_DTB)),,--diagnostic-dtb=auto)
TEST_AARCH64_EXTRA_gic-spi := --diagnostic-dtb=auto

# Pre-build helpers (one per MODE) put the $(MAKE) sub-invocation on its
# own recipe line so `make -n` honors the dry-run contract. A single
# multi-branch recipe (the v2 approach with `if [ "$(MODE)" = ... ]`)
# put $(MAKE) inside an `if/fi` block on one recipe line and would
# execute under -n.
.PHONY: _test-aarch64-prep-smp _test-aarch64-prep-no-ack _test-aarch64-prep-gic-spi
_test-aarch64-prep-smp:
	$(MAKE) KERNEL_SELFTEST=1 aarch64-uefi
_test-aarch64-prep-no-ack:
	@test "$(AARCH64_SMP_TEST_NO_ACK_CPU)" = 1 \
	  || { echo "AARCH64_SMP_TEST_NO_ACK_CPU must be 1; clean and rebuild" >&2; exit 1; }
	@test -f "$(AARCH64_UEFI_DISK)" -a -f "$(AARCH64_UEFI_FIRMWARE)" \
	  || { echo "Build the injected aarch64-uefi image first" >&2; exit 1; }
_test-aarch64-prep-gic-spi:
	$(MAKE) KERNEL_SELFTEST=1 aarch64-uefi

# Run helpers (one per MODE) keep the python harness on its own recipe
# line, after the pre-build. The python call therefore is NOT executed
# under `-n` (per the dry-run contract).
.PHONY: _test-aarch64-run-smp _test-aarch64-run-no-ack _test-aarch64-run-gic-spi
_test-aarch64-run-smp:
	python3 qemutests/aarch64_uefi_smp.py \
	  --cpus 1 2 4 --repeat 3 --timeout 90 --expect-selftest --expect-gic --expect-clk \
	  $(TEST_AARCH64_EXTRA_smp) \
	  --firmware "$(AARCH64_UEFI_SELFTEST_FIRMWARE)" \
	  --image "$(AARCH64_UEFI_SELFTEST_DISK)" \
	  --qemu "$(AARCH64_QEMU)" \
	  --log-dir "$(OS01_ROOT)/test-results/aarch64-uefi-smp/$$(date -u +%Y%m%dT%H%M%S)-normal-$$$$"
_test-aarch64-run-no-ack:
	python3 qemutests/aarch64_uefi_smp.py \
	  --cpus 2 --repeat 1 --timeout 90 --expect-no-ack 1 \
	  $(TEST_AARCH64_EXTRA_no-ack) \
	  --firmware "$(AARCH64_UEFI_FIRMWARE)" \
	  --image "$(AARCH64_UEFI_DISK)" \
	  --qemu "$(AARCH64_QEMU)" \
	  --log-dir "$(OS01_ROOT)/test-results/aarch64-uefi-smp/$$(date -u +%Y%m%dT%H%M%S)-no-ack-$$$$"
_test-aarch64-run-gic-spi:
	python3 qemutests/aarch64_gic_spi.py \
	  --diagnostic-dtb=auto \
	  --firmware "$(AARCH64_UEFI_SELFTEST_FIRMWARE)" \
	  --image "$(AARCH64_UEFI_SELFTEST_DISK)" \
	  --qemu "$(AARCH64_QEMU)" \
	  --log-dir "$(OS01_ROOT)/test-results/aarch64-gic-spi/$$(date -u +%Y%m%dT%H%M%S)-$$$$"

# test-aarch64: the umbrella. Dispatches to the per-MODE prep + run helpers.
test-aarch64: MODE ?= smp
test-aarch64: MODE := $(MODE)
# No image prerequisites: smp/gic-spi build the selftest variant in their
# prep helper, while no-ack must only consume an already injected image.
test-aarch64:
	$(call require_aarch64_uefi)
	$(call require_capability,uefi)
	@case "$(MODE)" in \
	  smp|no-ack|gic-spi) ;; \
	  *) echo "MODE must be smp|no-ack|gic-spi, got '$(MODE)'" >&2; exit 1;; \
	esac
	@echo "  [test-aarch64] MODE=$(MODE) extra=$(TEST_AARCH64_EXTRA_$(MODE))"
	$(MAKE) --no-print-directory _test-aarch64-prep-$(MODE)
	$(MAKE) --no-print-directory _test-aarch64-run-$(MODE)

test-aarch64-uefi-smp:        ; @$(MAKE) --no-print-directory test-aarch64 MODE=smp
test-aarch64-uefi-smp-no-ack: ; @$(MAKE) --no-print-directory test-aarch64 MODE=no-ack
test-aarch64-gic-spi:         ; @$(MAKE) --no-print-directory test-aarch64 MODE=gic-spi
```

**Why this layout (recipe-line discipline, mirrors Task 4):**
- The pre-build and run are split into per-MODE helper targets
  (`_test-aarch64-prep-$(MODE)`, `_test-aarch64-run-$(MODE)`) instead of
  being inlined into a shell `case ... $(MAKE) ...; python3 ...;;`
  block. Putting `$(MAKE)` inside a shell `if` branch on one recipe line
  is the v2 mistake: GNU Make still executes the line under `-n`.
- `TEST_AARCH64_EXTRA_$(MODE)` is a Make variable, not a shell variable.
  A shell variable set in one `@`-prefixed recipe line is empty in the
  next (each `@` line is a fresh `/bin/sh -c`). The Make variable is
  resolved at parse time and survives across recipe lines.
- `$(MAKE) --no-print-directory _test-aarch64-prep-$(MODE)` is on its
  own recipe line so it executes under `-n` (standard recursive-make
  contract, same as Task 4). The python call in the run helper is on its
  own recipe line and is NOT executed under `-n`.

- [ ] **Step 2: Verify**

Run: `make PROFILE=aarch64-clang -n test-aarch64` (expect smp branch).
Run: `make PROFILE=aarch64-clang -n test-aarch64 MODE=no-ack` (expect no-ack branch).
Run: `make -n PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=0 test-aarch64-uefi-smp-no-ack` and `make -n PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=0 test-aarch64 MODE=no-ack` (both must fail at parse time, matching `build_contract.sh targets`).
Inspect this dry run: it must not plan a normal AArch64 image build as a prerequisite of `MODE=no-ack`; the prep helper must check the two prebuilt files before the Python runner.
Run: `make PROFILE=aarch64-clang -n test-aarch64 MODE=gic-spi` (expect gic-spi branch).
Run: `make PROFILE=aarch64-clang -n test-aarch64-uefi-smp` (expect forwarding to MODE=smp).
On aarch64 host: `make PROFILE=aarch64-clang test-aarch64-uefi-smp` (real run; should match prior behavior).
Commit:
```bash
git add mk/components/run.mk
git commit -m "refactor(test): consolidate 3 aarch64 tests behind test-aarch64 MODE=<smp|no-ack|gic-spi>"
```

---

### Task 8: Implement `test-contract` bucket with PROFILE= flag

**Files:**
- Modify: `mk/components/run.mk:604-635` (the `test-build-contract-x86`, `test-build-contract-aarch64` recipes)

**Interfaces:**
- Consumes: `BUILD_DIR`, `PATH`, `HOME`, `TMPDIR`, `OS01_PROFILE_FILE`, `OS01_SUBMAKEFLAGS` (no change)
- Produces: `test-contract PROFILE=<name>` runs the right contract modes for that profile. Old targets become aliases.

- [ ] **Step 1: Define PROFILE→modes mapping**

Replace lines 604-635 with:
```makefile
.PHONY: test-contract test-build-contract-x86 test-build-contract-aarch64
# CI's contract job runs against a clean workspace; the bucket must still
# pre-build the artifacts it inspects. x86 contract needs disk.img;
# aarch64 contract needs aarch64-uefi. (Both were dropped in the v1
# plan; this restored version matches the original line 610 / 631.)
X86_CONTRACT_MODES := legacy-components legacy x86 sysroot firmware targets host-test
AARCH64_CONTRACT_MODES := aarch64 targets

# Pre-build helpers are split per PROFILE so the `+env` recipe prefix
# sits at Make's recipe-line position (not inside a shell `case` branch
# where `+env` would become a command name and fail with "+env: not
# found"). The cross-profile sub-make also gets `$(MAKE)` on its own
# recipe line so `-n` honors the standard recursive-make contract.
.PHONY: _test-contract-prep-x86 _test-contract-prep-aarch64
_test-contract-prep-x86:
	@echo "  [test-contract] x86: build hosttests via sub-make"
	$(call os01_submake,hosttests,all)
	@echo "  [test-contract] x86: pre-build aarch64-uefi in its own workspace"
	+env -i PATH="$(PATH)" HOME="$(HOME)" TMPDIR="$(TMPDIR)" \
	  $(MAKE) MAKEOVERRIDES= PROFILE=aarch64-clang aarch64-uefi
_test-contract-prep-aarch64:
	@true

# test-contract: the umbrella. Each PROFILE gets its own prereq and
# capability gate. The pre-build dispatch and the per-mode shell loop
# are on separate recipe lines.
test-contract: PROFILE ?= $(DEFAULT_PROFILE)
test-contract: PROFILE := $(PROFILE)
test-contract: $(if $(filter x86_64-clang,$(PROFILE)),disk.img,aarch64-uefi)
	$(call require_capability,$(if $(filter x86_64-clang,$(PROFILE)),rootfs,uefi))
	$(MAKE) --no-print-directory _test-contract-prep-$(if $(filter x86_64-clang,$(PROFILE)),x86,aarch64)
	@set -e; \
	case "$(PROFILE)" in \
	  x86_64-clang)    modes="$(X86_CONTRACT_MODES)";; \
	  aarch64-clang)   modes="$(AARCH64_CONTRACT_MODES)";; \
	  *) echo "PROFILE must be x86_64-clang or aarch64-clang, got '$(PROFILE)'" >&2; exit 1;; \
	esac; \
	for m in $$modes; do \
	  echo "  [test-contract] $(PROFILE)/$$m"; \
	  sh qemutests/build_contract.sh $(PROFILE) $$m; \
	done

test-build-contract-x86:      ; @$(MAKE) --no-print-directory test-contract PROFILE=x86_64-clang
test-build-contract-aarch64:  ; @$(MAKE) --no-print-directory test-contract PROFILE=aarch64-clang
```

**Why this layout:**
- `disk.img` (x86) and `aarch64-uefi` (aarch64) prerequisites match the
  original line 610 / 631; a CI workspace that starts empty would
  otherwise fail the contract check.
- Two separate per-PROFILE prep helpers (`_test-contract-prep-x86` and
  `_test-contract-prep-aarch64`) replace the v2 single
  `_test-contract-prep` helper. Putting the `+env ... $(MAKE) ...` line
  inside a shell `case` branch (the v2 approach) made `+env` part of the
  shell command name and triggered `+env: not found`. Verified: only the
  recipe-line position treats `+` as a prefix.
- `$(MAKE) _test-contract-prep-...` is on its own recipe line in
  `test-contract`, so it executes under `-n` (standard recursive-make
  contract). The `sh qemutests/build_contract.sh` invocations are inside
  a shell `for` on a separate recipe line and are NOT executed under
  `-n`.

- [ ] **Step 2: Verify**

Run: `make -n test-contract` (expect: disk.img prereq + `_test-contract-prep` invocation + a `for m in ...; do sh qemutests/build_contract.sh x86_64-clang $$m; done` shell loop).
Run: `make -n test-contract PROFILE=aarch64-clang` (expect: aarch64-uefi prereq + the smaller two-mode loop).
Run: `make -n test-build-contract-x86` (expect: `@$(MAKE) test-contract PROFILE=x86_64-clang`).
Run: `make -n test-build-contract-aarch64` (expect: `@$(MAKE) test-contract PROFILE=aarch64-clang`).
Confirm the dry run prints, but does not share a recipe line with, the
`build_contract.sh` loop:
```bash
make -n test-contract > /tmp/os01-test-contract-dry.txt
grep -q 'sh qemutests/build_contract.sh' /tmp/os01-test-contract-dry.txt
grep -q '^make --no-print-directory _test-contract-prep-x86' /tmp/os01-test-contract-dry.txt
```
On a real x86 build host: run one mode manually to spot-check (e.g. `sh qemutests/build_contract.sh x86_64-clang firmware`).
Commit:
```bash
git add mk/components/run.mk
git commit -m "refactor(test): consolidate 2 contract tests behind test-contract PROFILE=<name>; preserve prereqs"
```

---

### Task 9: Finalize `make help` and write `docs/build-system-harness.md`

**Files:**
- Modify: `mk/components/run.mk:494-588` (final help taxonomy)
- Create: `docs/build-system-harness.md`

**Interfaces:**
- Consumes: the target list finalized by Tasks 1-8
- Produces: a single authoritative document explaining the harness taxonomy, capability gates, alias policy, and how to add new targets

- [ ] **Step 1: Show the canonical buckets in `make help`**

After Tasks 4-8 add the bucket targets, edit the `help:` recipe as follows:

- Replace the old `Test (x86, rootfs)` entries for `test`, `test-phase-0`, `test-runtime`, `test-syscall`, `test-inittab`, and `test-network` with rows for `test-qemu`, `test-host`, `test-static`, and `test-kernel-selftest` (the last already exists). Keep `test-syscall-repeat`, `test-user-canary`, and `test-pmm-boot-reservation` as standalone rows.
- Replace the old `test-aarch64-uefi-smp` and `test-aarch64-uefi-smp-no-ack` rows with one `test-aarch64` row marked `(uefi)`. Replace the two old build-contract rows with one `test-contract` row; describe its `PROFILE=x86_64-clang|aarch64-clang` selection.
- Move the temporarily exposed `test-kernel-layout`, `test-kernel-canary-contract`, and `test-aarch64-gic-spi` rows from Task 1 to a `Focused compatibility checks` section. These remain callable and visible for focused debugging, but are clearly distinguished from the six bucket entry points.
- Add a final help sentence: `Legacy test-* aliases remain callable for one release cycle; see docs/build-system-harness.md §4.` Do not list every forwarding alias as a primary test entry.

Run `make help` and count the six exact canonical target names. Each must appear once. Confirm the three standalone and three focused compatibility rows remain visible.

- [ ] **Step 2: Write the doc**

Create `docs/build-system-harness.md` with these sections:
```markdown
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
```

- [ ] **Step 3: Cross-link and commit**

Verify links from existing docs land here:
- `AGENTS.md` "Quick start" → link to "Build System Harness" section.
- `docs/build-run-debug.md` first paragraph → link to this doc.
Commit:
```bash
git add mk/components/run.mk docs/build-system-harness.md
git commit -m "docs(build): add build-system-harness.md as authoritative target taxonomy reference"
```

---

### Task 10: Update AGENTS.md, docs/build.md, docs/build-run-debug.md

**Files:**
- Modify: `AGENTS.md` (Quick start section)
- Modify: `docs/build.md` (用户入口 section)
- Modify: `docs/build-run-debug.md` (test recipes section)

- [ ] **Step 1: AGENTS.md Quick start**

Replace the Quick start code block with:
```markdown
## Quick start

```bash
make run                 # Build + run
make debug               # Build + QEMU paused, GDB :1234
make clean               # MANDATORY after struct changes (no header deps!)
make test-qemu SUITE=phase-0    # QEMU E2E boot (also: systest, inittab-phase, network)
make test-host           # Host-side hosttests + PMM boot reservation
make test-static         # Static audits: runtime, layout, canary, link-order
make OS01_SYSTEST=1 test-qemu SUITE=systest   # 228-test syscall suite
make KERNEL_SELFTEST=1 test-kernel-selftest    # Built-in kernel selftests
```

See [`docs/build-system-harness.md`](docs/build-system-harness.md) for the full
target taxonomy and flag conventions.
```

Keep the rest of AGENTS.md (build flags, deps, etc.) unchanged.

- [ ] **Step 2: docs/build.md 用户入口**

In `docs/build.md` line ~25 ("用户入口"), replace the list of test-* entries
with the new bucket list. Insert a paragraph:

```markdown
**Test entry points** — six buckets (3 with flags, 3 fixed):

| Bucket | Purpose | Default |
| --- | --- | --- |
| `make test-qemu SUITE=phase-0\|systest\|inittab-phase\|network` | QEMU E2E against the matching variant image | `phase-0` |
| `make test-host` | hosttests + PMM boot reservation | — |
| `make test-static` | runtime / layout / canary / link-order audits (8 audits) | — |
| `make test-aarch64 MODE=smp\|no-ack\|gic-spi` | AArch64 PSCI / GIC harness | `smp` |
| `make test-contract PROFILE=x86_64-clang\|aarch64-clang` | CI build-contract check | `x86_64-clang` |
| `make test-kernel-selftest` | Boot selftest image + parse `[selftest]` markers | — |

Standalone (not bucketed): `test-syscall-repeat` (own harness),
`test-user-canary` (subset of test-static, distinct prereqs),
`test-pmm-boot-reservation` (subset of test-host). See
[`docs/build-system-harness.md`](build-system-harness.md) §3 for the full
bucket model.
```

- [ ] **Step 3: docs/build-run-debug.md test recipes**

Find the section that walks through `make test-syscall` / `make test-network`
etc. Replace each occurrence of the old target name with the new bucket call
(preserving the OS01_* flag combinations). Concretely:

| Old | New |
| --- | --- |
| `make OS01_SYSTEST=1 test-syscall` | `make OS01_SYSTEST=1 test-qemu SUITE=systest` |
| `make OS01_NETTEST=1 test-network` | `make OS01_NETTEST=1 test-qemu SUITE=network` |
| `make INITTAB_FILE=config/inittab.test test-inittab` | `make INITTAB_FILE=config/inittab.test test-qemu SUITE=inittab-phase` |
| `make KERNEL_SELFTEST=1 test-kernel-selftest` | unchanged (separate bucket) |
| `make test` | `make test-host` (note: `make test` still works as alias) |
| `make test-build-contract-x86` | `make test-contract` |
| `make PROFILE=aarch64-clang test-aarch64-uefi-smp` | `make PROFILE=aarch64-clang test-aarch64` |

- [ ] **Step 4: Verify docs**

Run:
```bash
grep -nE 'test-(syscall|network|inittab|phase-0|runtime|kernel-layout|kernel-canary-contract|user-canary|build-contract)' \
  docs/build-run-debug.md docs/build.md AGENTS.md
```
Expected: matches allowed only inside legacy-compat notes, the standalone
entries (`test-user-canary`, `test-pmm-boot-reservation`), or the
`test-kernel-canary-contract` reference in `test-static`'s recipe. All
bucket-name changes applied.

Also verify the bucket count is consistent across docs:
```bash
grep -nE '(six|five|7|seven|6|six) (test )?buckets?' \
  docs/build-run-debug.md docs/build.md AGENTS.md docs/build-system-harness.md
```
Expected: all sites say "six buckets" (or 6). No "five buckets" or
"seven buckets" leftovers.

Commit:
```bash
git add AGENTS.md docs/build.md docs/build-run-debug.md
git commit -m "docs: update Quick start / 用户入口 / test recipes to bucket targets"
```

---

### Task 11: Update CI scripts / shell references

**Files:**
- Modify: any `.sh`, `.yml`, `.yaml`, `Makefile` under `.github/`, `scripts/`, `tools/`, or top-level that references old target names

- [ ] **Step 1: Inventory references**

Run:
```bash
grep -rnE '\bmake .*\btest-(syscall|network|inittab|phase-0|runtime|kernel-layout|kernel-canary-contract|user-canary|build-contract-(x86|aarch64)|aarch64-uefi-smp|no-ack|gic-spi)\b' . \
  --include='*.sh' --include='*.yml' --include='*.yaml' --include='Makefile*' --include='*.mk' \
  --exclude-dir=thirdpart --exclude-dir=.git
```
Note every match and what it should map to (use the table from Task 10 step 3).

- [ ] **Step 2: Update each match**

For every match, replace the old invocation with the bucket equivalent.
For targets that are aliases (e.g. `test-syscall` still works as an alias
in this release), leave the match alone if the change is cosmetic-only —
but flag it in the commit message so the followup cleanup pass can find it.

- [ ] **Step 3: Verify**

Re-run the grep from Step 1. Expected: either zero hits, or only hits
that the author consciously left as alias-compat.
Commit:
```bash
git add <modified-files>
git commit -m "ci: migrate to bucket test targets (test-qemu/test-host/test-static/test-aarch64/test-contract)"
```

---

### Task 12: Final consistency verification + open followup plan for alias deletion

**Files:**
- Read only: `mk/components/run.mk`, `docs/build-system-harness.md`, `qemutests/build_contract.sh`
- Create: `docs/superpowers/plans/2026-09-25-delete-test-aliases.md` (followup plan, executed after one release cycle)

- [ ] **Step 1: Verify alias-vs-bucket consistency**

Run:
```bash
make help
```
Compare the visible target list against `docs/build-system-harness.md` §2 and §3.
Every name in help must have a corresponding row in the doc table; every row
in the doc table must appear in help (or be marked as legacy-removed).

Run the six buckets under `-n`:
```bash
make -n test-qemu SUITE=systest
make -n test-host
make -n test-static
make -n test-kernel-selftest
make -n PROFILE=aarch64-clang test-aarch64 MODE=smp
make -n test-contract PROFILE=aarch64-clang
```
All six must plan a recipe without "no rule to make target".

Confirm the dry-run recipe structure: `$(MAKE)` lines execute under `-n`
(the standard recursive-make contract); Python and shell harness calls are
on their own lines and are only printed. GNU Make strips recipe prefixes from
printed commands, so a `^\+` grep cannot prove whether a command executed.

```bash
make -n test-qemu SUITE=systest > /tmp/os01-qemu-suite-dry.txt
make -n PROFILE=aarch64-clang test-aarch64 MODE=smp > /tmp/os01-aarch64-suite-dry.txt
make -n test-contract PROFILE=x86_64-clang > /tmp/os01-contract-dry.txt
grep -q '^make OS01_SYSTEST=1 image' /tmp/os01-qemu-suite-dry.txt
grep -q 'python3 qemutests/run_test.py systest' /tmp/os01-qemu-suite-dry.txt
grep -q '^make --no-print-directory _test-aarch64-run-smp' /tmp/os01-aarch64-suite-dry.txt
grep -q 'python3 qemutests/aarch64_uefi_smp.py' /tmp/os01-aarch64-suite-dry.txt
grep -q '^env -i ' /tmp/os01-contract-dry.txt
grep -q 'sh qemutests/build_contract.sh' /tmp/os01-contract-dry.txt
```

Verify the build-contract script's old-name compatibility checks still pass:
```bash
grep -nE 'test-(phase-0|syscall|network|inittab|runtime|kernel-layout|kernel-canary-contract|build-contract)' qemutests/build_contract.sh
```
Each match must be inside a comment or a `case` branch that already
checks the legacy name; the script must keep working under the alias
compatibility window.

- [ ] **Step 2: Write the followup plan for alias deletion**

Create `docs/superpowers/plans/2026-09-25-delete-test-aliases.md` with at
minimum:

```markdown
# Delete Test Target Aliases

> **Goal:** After one release cycle with the new bucket targets, delete
> the legacy `test-*` aliases introduced by the 2026-09-25 consolidation.

> **Trigger:** No CI script under `.github/`, `scripts/`, `tools/`, or
> any top-level `*.mk` invokes a deleted alias name; `qemutests/build_contract.sh`
> has been updated to require the bucket names directly.

> **Tasks:**
> 1. Use `rg -n 'test-(phase-0|syscall|inittab|network|runtime|kernel-layout|kernel-canary-contract|aarch64-uefi-smp|aarch64-gic-spi|build-contract-)' .github scripts tools mk qemutests docs AGENTS.md` to inventory uses; preserve references in historical reports.
> 2. Migrate executable CI and shell references to buckets with explicit `SUITE`, `MODE`, or `PROFILE`, then run their affected CI commands.
> 3. Update `qemutests/build_contract.sh` target checks to require the bucket names and retain the invalid-profile and invalid-no-ack assertions.
> 4. Delete only the forwarding alias lines from `mk/components/run.mk`; retain the focused standalone checks used for debugging.
> 5. Update `docs/build-system-harness.md` §4 and run `make help`, `make -n test-qemu SUITE=systest`, `make -n PROFILE=aarch64-clang test-aarch64 MODE=smp`, and both build-contract modes.
```

This followup is **not** executed as part of this plan. The Architecture
section's promise that aliases "exist for one release cycle" is honored
by having the deletion tracked in a separate plan, not by collapsing it
into this one. Doing both at once would break CI jobs that have not yet
migrated to the bucket names.

- [ ] **Step 3: Final smoke run**

Run: `make help` (visual confirmation).
Run: `make test-host` (hosttests pass; ~minute).
Run: `make OS01_SYSTEST=1 test-qemu SUITE=systest` (228 syscall tests pass).
Run: `make KERNEL_SELFTEST=1 test-kernel-selftest` (selftest passes).
Run: `make test-static` (audits pass).
Run: `make test-contract PROFILE=x86_64-clang` (one mode manually
  first via `sh qemutests/build_contract.sh x86_64-clang firmware` to
  confirm the script still finds the artifacts).

- [ ] **Step 4: Commit followup plan and merge**

Commit:
```bash
git add docs/superpowers/plans/2026-09-25-delete-test-aliases.md
git commit -m "docs(plan): track test-alias deletion as followup to bucket consolidation"
```
Merge the bucket-introduction PR to `master` per repo policy. Push.

---

## Self-Review Checklist

Run this yourself before declaring the plan complete:

1. **Spec coverage** — does each old target have a task that absorbs it?
   - `all` → Task 2 (delete).
   - `test` → Task 5 (renamed to `test-host`, `test` becomes alias; deletion deferred to followup).
   - `validate-kernel`, `validate-uefi` → still exist (no change); capability badge fix in Task 1.
   - `run`, `run-kvm`, `run-virtio`, `debug` → Task 3 (DRY but keep names; `-accel kvm` preserved on run-kvm, no `-no-reboot` on debug).
   - `test-phase-0`, `test-syscall`, `test-inittab`, `test-network` → Task 4.
   - `test-syscall-repeat` → still exists (independent harness; not folded into test-qemu).
   - `test-runtime`, `test-kernel-layout`, `test-kernel-canary-contract`, `test-user-canary` → Task 6 (each as a separate alias with its own recipe body; test-user-canary also embedded inside test-static).
   - `test-pmm-boot-reservation` → Task 5.
   - `test-aarch64-uefi-smp`, `test-aarch64-uefi-smp-no-ack`, `test-aarch64-gic-spi` → Task 7.
   - `test-build-contract-x86`, `test-build-contract-aarch64` → Task 8 (with build prerequisites restored).
   - Hidden targets documented → Task 1.
   - Harness design captured → Task 9 (now reflects 6 buckets + 3 standalone, not 7 buckets).
   - User-facing docs updated → Task 10 (no more "five buckets" wording).
   - CI migrated → Task 11.
   - Alias deletion → followup plan written in Task 12, NOT executed here.
   All gaps covered.

2. **Placeholder scan** — verify no `TODO`, `TBD`, "implement later", "add appropriate error handling", or "similar to Task N". Tasks 4-8 repeat the recipe bodies rather than referring to "similar to N".

3. **Type consistency** — `SUITE`, `MODE`, `PROFILE` are the flag names used uniformly. Bucket names `test-qemu`, `test-host`, `test-static`, `test-aarch64`, `test-contract`, `test-kernel-selftest` are spelled identically across Tasks 4-12, the harness doc (§3), and the Quick-start / 用户入口 snippets. `RUN_QEMU_BASE` and `RUN_QEMU_DISK` appear in both Task 3 and the variable reference; the kvm/debug early flags retain their exact names. The `test_qemu_suite_image` function was replaced by per-SUITE Make variables (`TEST_QEMU_FLAVOR_<suite>`, `TEST_QEMU_IMG_<suite>`); references to it elsewhere are removed.

4. **Review Focus** — five failure modes listed in the header; the task that owns each test is named in the same list. The new failure modes surfaced during review (recipe-line discipline, alias = exact recipe, build prereq preservation, bucket-count consistency) are all pinned to verification steps in Tasks 4, 6, 8, 10, and 12.

5. **Issue coverage check** (post-review additions):
   - **v1 review**:
     - Issue 1 (word 2 empty) → fixed in Task 4 with `$(subst ...)` and removed entirely in v3 in favor of per-SUITE Make variables (no function call needed).
     - Issue 2 (QEMU suffix / prereqs) → fixed in Task 3 with explicit `RUN_QEMU_FLAGS_*` table; v3 review found `-no-reboot` still in BASE — moved out of BASE, byte-comparison verification added.
     - Issue 3 (test-contract prereqs) → fixed in Task 8 restoring `disk.img` / `aarch64-uefi`.
     - Issue 4 ($(MAKE) on same line) → addressed in Tasks 4, 7, 8 with explicit "Why this layout" subsections; v3 review found additional cases (shell var cross-line, `+env` in case branch) — all folded into the Global Constraints "Recipe-line discipline" rule.
     - Issue 5 (alias = exact recipe) → fixed in Task 6.
     - Issue 6 (alias lifecycle contradiction) → fixed by removing deletion from Task 12 and adding the followup plan.
     - Issue 7 (bucket count inconsistency) → fixed in Task 9 §3 and Task 10 Step 2; verified by the bucket-count grep in Task 10 Step 4 and Task 12 Step 1.
   - **v2 review**:
     - Issue 1 (variant image prereq fails) → fixed in Task 4 by removing the image-path prereq and using `$(MAKE) $(TEST_QEMU_FLAVOR_$(SUITE)) image` on its own recipe line, matching the original test-syscall pattern.
     - Issue 2 (`+env` in shell case fails) → fixed in Task 8 by splitting `_test-contract-prep` into per-PROFILE helpers (`_test-contract-prep-x86`, `_test-contract-prep-aarch64`) so `+env` sits at the recipe-line position.
     - Issue 3 (`-no-reboot` still in RUN_QEMU_BASE) → fixed in Task 3 Step 1; RUN_QEMU_BASE no longer contains `-no-reboot`, only `run` and `run-kvm` flags have it.
     - Issue 4 (test-aarch64 shell var + dry-run issues) → fixed in Task 7 by using `TEST_AARCH64_EXTRA_$(MODE)` Make variables and per-MODE `_test-aarch64-prep/run-$(MODE)` helpers so each `$(MAKE)` and python call sits on its own recipe line.
     - Issue 5 (grep counts wrong) → fixed in Task 6 Step 2: test-static expects 6 python calls, test-runtime expects 4 (previously claimed 7 and 5).
