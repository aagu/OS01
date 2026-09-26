# ── Run / Debug / Test / Validation / Clean entry points ─────────
# (spec: mk/components/run.mk — the root Makefile's user-facing aliases).
# Every QEMU, debug, test, validation, clean and compat-copy recipe lives
# here so the root Makefile stays a pure include + default-goal file.
#
# Capability gates use the same pattern as lib/user (root Makefile): the
# artifact prereqs exist only for capable profiles, and the recipe's first
# line expands to $(error ...) on incapable profiles — so both real runs and
# dry runs (-n) fail cleanly at the gate with
#   PROFILE='<p>' lacks capability '<cap>'
# instead of "No rule to make target".

# ── Firmware (x86 run targets) ─────────────────────────────────
# Root-owned, profile-private real-file rule. OVMF_FIRMWARE_SOURCE accepts
# ONLY an https:// URL (wget to "$@.tmp", then an atomic rename) or an
# existing absolute local path (content-guarded copy). Every other value —
# relative path, bad scheme, missing file — is rejected with a clear error
# BEFORE any download/copy. wget's exit status is checked via set -e, so a
# failed download never leaves a half-written file (and never reaches the
# mv). Never calls boot/uefi for firmware. Capability-gated on rootfs: the
# x86-only OVMF_FIRMWARE variable is undefined for aarch64-clang, so without
# the guard this rule expands to an empty-target rule with a recipe — a
# make-version-sensitive parse hazard.
ifeq ($(filter rootfs,$(PROFILE_CAPABILITIES)),rootfs)
$(OVMF_FIRMWARE):
	@set -e; \
	mkdir -p "$(dir $@)"; \
	case "$(OVMF_FIRMWARE_SOURCE)" in \
	https://*) \
	  wget -q -O "$@.tmp" "$(OVMF_FIRMWARE_SOURCE)"; \
	  mv "$@.tmp" "$@";; \
	/*) \
	  test -f "$(OVMF_FIRMWARE_SOURCE)" || { \
	    echo "ERROR: OVMF_FIRMWARE_SOURCE '$(OVMF_FIRMWARE_SOURCE)' is not an existing file" >&2; \
	    exit 1; }; \
	  cmp -s "$(OVMF_FIRMWARE_SOURCE)" "$@" || cp "$(OVMF_FIRMWARE_SOURCE)" "$@";; \
	*) \
	  echo "ERROR: OVMF_FIRMWARE_SOURCE '$(OVMF_FIRMWARE_SOURCE)' must be an https:// URL or an existing absolute local file path" >&2; \
	  exit 1;; \
	esac

# $(OVMF_FIRMWARE) is absolute (BUILD_DIR is profile-absolute), so the same
# on-disk file is also reachable through its relative spelling. This alias
# makes `make build/<profile>/firmware/OVMF.fd` work too; the absolute rule
# above does the actual work.
build/$(PROFILE)/firmware/OVMF.fd: $(OVMF_FIRMWARE)
endif

# ── Kernel compat copy ─────────────────────────────────────────
# Project-root kernel.bin is a one-way copy of the profile's kernel artifact
# (mk/components/kernel.mk). Only defined when the profile declares a kernel
# artifact (x86_64-clang); other profiles simply fail on `make kernel.bin`.
ifdef KERNEL_ARTIFACT
kernel.bin: $(KERNEL_ARTIFACT)
	@cmp -s $(KERNEL_ARTIFACT) $@ || cp $(KERNEL_ARTIFACT) $@
endif

# ── Run (x86, rootfs capability) ───────────────────────────────
# All x86 QEMU entry points consume the profile's NORMAL_IMAGE and
# OVMF_FIRMWARE directly — never the project-root disk.img compat copy and
# never the source-tree boot/uefi/OVMF.fd.

# Shared QEMU argument groups. Variant flags are inserted between these
# groups so the final argument order matches each original recipe.
RUN_QEMU_BASE = $(QEMU_BIN) -M q35 -smp $(SMP) \
  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_FIRMWARE)
RUN_QEMU_DISK = -drive file=$(NORMAL_IMAGE),format=raw,if=none,id=disk \
  -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
  -object rng-random,filename=/dev/urandom,id=rng0 \
  -device virtio-rng-pci,rng=rng0 \
  -m $(MEMORY) -display $(DISPLAY) -serial stdio

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

# ── aarch64 UEFI bring-up (uefi capability) ────────────
# Targets are always defined so `make aarch64-uefi` under the default x86
# profile fails at the capability gate (not "no rule to make target");
# the aarch64-only AARCH64_UEFI_* variables are empty under x86, so the
# guard inside `require_capability aarch64-only` errors out cleanly. The
# kernel artifact + image rules live in mk/components/image.mk; there is
# no `lib` dependency — aarch64 does not consume the sysroot.
define require_aarch64_uefi
$(if $(AARCH64_UEFI_DISK),,$(error ERROR: aarch64-uefi targets require the aarch64-clang profile (AARCH64_UEFI_DISK is empty)))
endef

.PHONY: aarch64-uefi
aarch64-uefi: $(AARCH64_UEFI_DISK) $(AARCH64_UEFI_FIRMWARE)
	$(call require_aarch64_uefi)
	$(call require_capability,uefi)

.PHONY: aarch64-uefi-kernel
aarch64-uefi-kernel: $(BUILD_DIR)/artifacts/kernel.elf
	$(call require_aarch64_uefi)
	$(call require_capability,uefi)

.PHONY: run-aarch64-uefi
run-aarch64-uefi: aarch64-uefi
	$(call require_aarch64_uefi)
	$(call require_capability,uefi)
	@set -e; \
	case "$(AARCH64_UEFI_SMP_DIAGNOSTIC_DTB)" in \
	0) extra_dtb= ;; \
	*) \
	  dtb_dir="$(BUILD_DIR)/logs/aarch64-uefi-dtb"; \
	  mkdir -p "$$dtb_dir"; \
	  sparse="$$dtb_dir/qemu-virt.dtb.sparse"; \
	  packed="$$dtb_dir/qemu-virt.dtb"; \
	  $(AARCH64_QEMU) -M virt,gic-version=2 -cpu cortex-a53 -smp $(SMP) \
	    -machine "dumpdtb=$$sparse" -display none -m $(MEMORY); \
	  dtc -I dtb -O dtb -o "$$packed" "$$sparse"; \
	  rm -f "$$sparse"; \
	  extra_dtb="-dtb $$packed" ;; \
	esac; \
	$(AARCH64_QEMU) -M virt,gic-version=2$${extra_dtb:+,acpi=off} -cpu cortex-a53 -smp $(SMP) -m $(MEMORY) \
	  -drive if=pflash,format=raw,readonly=on,file=$(AARCH64_UEFI_FIRMWARE) \
	  -drive if=none,file=$(AARCH64_UEFI_DISK),format=raw,readonly=on,id=disk \
	  -device virtio-blk-device,drive=disk \
	  $$extra_dtb \
	  -serial stdio -display none -no-reboot

# The AArch64 PSCI SMP acceptance suite intentionally runs the production
# firmware/image with multiple vCPU counts.  Its Python parser is host-only
# and asserts structured kernel diagnostics; it does not mistake a UEFI
# banner or arbitrary firmware "FAIL" text for kernel test results.
#
# The prebuilt edk2 firmware for QEMU virt does not expose the device tree
# through the EFI configuration table, so the kernel prints
# `[dtb] FATAL: UEFI handoff has no DTB` and never reaches SMP. The harness
# accepts --diagnostic-dtb=auto to materialize a packed QEMU-generated DTB
# per case and pass it via `-dtb` (with `acpi=off`). Set
# AARCH64_UEFI_SMP_DIAGNOSTIC_DTB=0 to require the production firmware path.
#
# This target only consumes an already injected image. Switching the
# compiled value requires the following profile-clean sequence:
# make PROFILE=aarch64-clang clean
# make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 aarch64-uefi
# make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 test-aarch64 MODE=no-ack
# make PROFILE=aarch64-clang clean
# make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=0 aarch64-uefi
# Then run a normal two-core recovery case with the rebuilt image paths.
#
# PL011 RX -> GIC SPI injection test (spec §7.4, Task 2.3b). Reuses the
# SMP suite's firmware / image / DTB mechanism; harness injects one byte
# into the PL011 socket and expects the kernel-side "handled count=1"
# marker once the RX handler fires.
.PHONY: test-aarch64
# The contract harness expects invalid no-ack injection to fail even under
# `make -n`. Keep this as a parse-time check.
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

# ── Validation ─────────────────────────────────────────────
# validate keeps the x86 kernel + UEFI artifact checks (kernel ELF has no
# undefined symbols / INTERP / DYNAMIC, is EM_X86_64, exports _start /
# kernel_main / _text; the EFI app has a parseable COFF export table) and
# prints the selected profile's identity. x86-only: gated on the `rootfs`
# capability (the recipes inspect the x86 kernel.bin artifact and x86 kernel
# symbols, which aarch64-clang does not produce even though it has `uefi`)
# so an incapable profile gets the clean capability error instead of cryptic
# empty-LLVM_* failures.
.PHONY: validate validate-kernel validate-uefi validate-profile
validate: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),validate-kernel validate-uefi validate-profile)
	$(call require_capability,rootfs)
validate-kernel: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),kernel.bin)
	$(call require_capability,rootfs)
	@echo "  [validate] kernel.elf has no undefined symbols"
	@test -z "$$($(LLVM_NM) --undefined-only $(KERNEL_ELF))"
	@echo "  [validate] kernel.elf has no INTERP/DYNAMIC program headers"
	@! $(LLVM_READELF) -Wl $(KERNEL_ELF) | grep -E 'INTERP|DYNAMIC'
	@echo "  [validate] kernel.elf machine is $(RUNTIME_MACHINE_kernel)"
	@$(LLVM_READOBJ) --file-headers $(KERNEL_ELF) | grep -F '$(RUNTIME_MACHINE_kernel)'
	@echo "  [validate] GLOBAL _start present"
	@$(LLVM_READELF) -Ws $(KERNEL_ELF) | grep -E 'GLOBAL.*\b_start\b'
	@echo "  [validate] GLOBAL kernel_main present"
	@$(LLVM_READELF) -Ws $(KERNEL_ELF) | grep -E 'GLOBAL.*\bkernel_main\b'
	@echo "  [validate] GLOBAL _text present"
	@$(LLVM_READELF) -Ws $(KERNEL_ELF) | grep -E 'GLOBAL.*\b_text\b'
validate-uefi: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(UEFI_EFI))
	$(call require_capability,rootfs)
	@echo "  [validate] $(notdir $(UEFI_EFI)) coff-exports"
	@$(LLVM_READOBJ) --coff-exports $(UEFI_EFI) >/dev/null
validate-profile:
	@printf 'profile=%s triple=%s sysroot=%s capabilities=%s\n' "$(PROFILE)" "$(TARGET_TRIPLE)" "$(SYSROOT)" "$(PROFILE_CAPABILITIES)"

# ── Test ────────────────────────────────────────────────────
# Each x86 E2E test builds its image VARIANT in an isolated dir
# (build/<profile>/image/<variant>/disk.img) and runs qemutests/run_test.py
# against that exact image (DISK_IMG env). Variant builds NEVER delete or
# write the normal image: when the normal image exists, its sha256 is
# recorded before and after the variant build (image/normal.before /
# normal.after) and compared. The normal image itself is built by the normal
# build flow (make / make disk.img); if it does not exist the integrity
# comparison is skipped, not faked.
#
# Recipe-line structure matters for dry runs: GNU make EXECUTES a recipe
# line that contains $(MAKE) even under -n, so the sha256 sandwich and the
# python3 run sit on their own recipe lines (printed only under -n) and the
# variant sub-make on its own line (the standard recursive-make behavior).

# Explicit variant image paths (mirror the IMAGE_DIR computation the variant
# sub-make performs, so the suite runs against the exact image just built).
TEST_SYSTEST_IMAGE  := $(BUILD_DIR)/image/systest/disk.img
TEST_NETTEST_IMAGE  := $(BUILD_DIR)/image/nettest/disk.img
TEST_INITTAB_IMAGE  := $(BUILD_DIR)/image/inittab-test/disk.img
TEST_SELFTEST_IMAGE := $(BUILD_DIR)/image/selftest/disk.img
# The fd_refcount selftest asserts actual cross-CPU coverage.  Four vCPUs
# avoid a scheduler-migration false negative seen with the interactive
# default of two, while keeping this test-only setting independently
# overridable.
KERNEL_SELFTEST_SMP ?= 4

.PHONY: test-host test-pmm-boot-reservation
test-host:
	$(call require_capability,rootfs)
	@$(call os01_submake,hosttests,run $(OS01_SUBMAKE_ARGS))
	python3 qemutests/pmm_boot_reservation_test.py
test-pmm-boot-reservation:
	python3 qemutests/pmm_boot_reservation_test.py

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

.PHONY: test-qemu
# Use the per-SUITE Make variables from Step 1. The image path is
# informational — it is NOT used as a prerequisite, because the
# variant image path has no direct build rule (the chain is `image` (phony)
# → `$(DISK_IMG)` (variable) → rule in mk/components/image.mk). Listing
# the raw image path as a prereq fails with "No rule to make target"
# (verified: `make -n test-qemu SUITE=systest` errors with the systest
# variant path). The variant build is triggered by a `$(MAKE) ... image`
# sub-make inside the recipe.
# Reject syscall E2E combined with kernel-selftest at parse time so it
# aborts before any build. This check must run before any prerequisites
# are built.
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

# Exercise repeated exec/exit through the normal terminal and ash path.

# Exercise repeated exec/exit through the normal terminal and ash path.
.PHONY: test-syscall-repeat
ifneq ($(filter test-syscall-repeat,$(MAKECMDGOALS)),)
ifneq ($(filter 1,$(OS01_SYSTEST) $(KERNEL_SELFTEST)),)
$(error ERROR: test-syscall-repeat requires normal init and KERNEL_SELFTEST=0)
endif
endif
test-syscall-repeat: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(NORMAL_IMAGE) $(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	python3 qemutests/x86_64_systest_repeat.py --disk "$(NORMAL_IMAGE)" \
	  --firmware "$(OVMF_FIRMWARE)" --smp "$(SMP)"

# test-static = the umbrella: runs every static audit in one shot.
# Delegates the 4 runtime-audit checks + validate-kernel to test-runtime
# (its prerequisite) so the recipe never drifts from the runtime target.
# The remaining 3 audits (layout, canary-contract, user-canary) are
# distinct in harness/prereqs and stay in this recipe.
.PHONY: test-static test-runtime test-kernel-layout test-kernel-canary-contract test-user-canary

# ── test-static: 5 runtime audits via test-runtime + 3 standalone ──
test-static: test-runtime
	$(call require_capability,rootfs)
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
	test -n "$$($(LLVM_NM) -P $(SYSROOT)/usr/lib/libc.a | awk '$$1=="__stack_chk_fail" && $$2=="T"')" \
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

# This target builds and boots only the selftest-scoped image.  In particular
# it never uses the ordinary image, and it refuses a combined syscall/selftest
# request because those suites are intentionally run independently.
.PHONY: test-kernel-selftest
test-kernel-selftest: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	@if [ "$(OS01_SYSTEST)" = "1" ]; then \
	  echo "ERROR: test-kernel-selftest must not be combined with OS01_SYSTEST=1; run make OS01_SYSTEST=1 test-qemu SUITE=systest separately" >&2; \
	  exit 1; \
	fi
	$(MAKE) KERNEL_SELFTEST=1 image
	@set -eu; \
	log="$(BUILD_DIR)/logs/kernel-selftest.log"; \
	mkdir -p "$$(dirname "$$log")"; \
	rm -f "$$log"; \
	set +e; \
	timeout 75 "$(QEMU_BIN)" -M q35 -smp "$(KERNEL_SELFTEST_SMP)" \
	  -drive if=pflash,format=raw,readonly=on,file="$(OVMF_FIRMWARE)" \
	  -drive file="$(TEST_SELFTEST_IMAGE)",format=raw,if=none,id=disk \
	  -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
	  -object rng-random,filename=/dev/urandom,id=rng0 \
	  -device virtio-rng-pci,rng=rng0 \
	  -m "$(MEMORY)" -display none -serial stdio -no-reboot -no-shutdown >"$$log" 2>&1; \
	rc=$$?; \
	set -e; \
	if [ "$$rc" -ne 0 ] && [ "$$rc" -ne 124 ]; then \
	  echo "ERROR: kernel selftest QEMU exited with status $$rc; log: $$log" >&2; \
	  cat "$$log" >&2; \
	  exit 1; \
	fi; \
	grep -aF '[selftest] running built-in tests...' "$$log"; \
	grep -aE '\[selftest\] [1-9][0-9]* total: [1-9][0-9]* passed, 0 failed' "$$log"; \
	grep -aF '[selftest] done' "$$log"; \
	if grep -aE '\[selftest\].*[[:space:]][1-9][0-9]* failed' "$$log" || grep -aF 'FAIL' "$$log"; then \
	  echo "ERROR: kernel selftest reported a failure; log: $$log" >&2; \
	  exit 1; \
	fi; \
	if [ "$$rc" -eq 124 ]; then \
	  echo "  [selftest] QEMU timed out after complete passing markers (expected)"; \
	fi

# ── Run paths ────────────────────────────────────────────────
# Prints the current profile's absolute firmware and active image path so a
# manual QEMU invocation can reuse exactly the selected variant. Gated on
# rootfs like every other x86 run entry point.
.PHONY: print-run-paths
print-run-paths:
	$(call require_capability,rootfs)
	@echo firmware=$(abspath $(OVMF_FIRMWARE))
	@echo image=$(abspath $(DISK_IMG))

# ── Help ─────────────────────────────────────────────────────
# Lists the root Makefile's user-facing targets, grouped by category, with the
# capability each one needs under the active profile. Works for any profile
# (no capability gate): the categories shown are the same, and the per-target
# capability badge is evaluated against the active profile's
# PROFILE_CAPABILITIES, so a user on aarch64-clang sees (uefi) badges
# on aarch64 targets and "n/a (active profile lacks <cap>)" on x86-only ones
# instead of a hard error. AGENTS.md Quick start and the docs are the canonical
# recipes; this is the discoverability surface for `make` itself.
.PHONY: help
help:
	@echo 'OS01 build — profile=$(PROFILE) capabilities=$(PROFILE_CAPABILITIES)'
	@echo ''
	@printf '  %-22s %-13s %s\n' 'TARGET' 'CAPABILITY' 'PURPOSE'
	@echo '  --------------------- ------------- ------------------------------'
	@printf '  %-22s %-13s %s\n' \
		 'disk.img'          '(rootfs)'     'Build the normal disk image (default goal)';
	@printf '  %-22s %-13s %s\n' \
		 'kernel.bin'        '(rootfs)'     'Project-root kernel copy (cmp-guarded)';
	@printf '  %-22s %-13s %s\n' \
		 'lib'               '(userland)'   'Build the sysroot (libc, mbedtls, etc.)';
	@printf '  %-22s %-13s %s\n' \
		 'user'              '(userland)'   'Build userland ELFs (incl. busybox.elf)';
	@printf '  %-22s %-13s %s\n' \
		 'image'             '(rootfs)'     'Current profile'"'"'s disk image (variant-resolved)';
	@printf '  %-22s %-13s %s\n' \
		 'sysroot'           '(userland)'   'Project-root sysroot/ symlink for clangd (editor)';
	@echo ''
	@echo 'Run / Debug (x86, rootfs):'
	@printf '  %-22s %-13s %s\n' \
		 'run'               '(rootfs)'     'QEMU q35 + e1000e + serial stdio';
	@printf '  %-22s %-13s %s\n' \
		 'run-kvm'           '(rootfs)'     'QEMU with KVM acceleration';
	@printf '  %-22s %-13s %s\n' \
		 'run-virtio'        '(rootfs)'     'QEMU with virtio-net-pci (instead of e1000e)';
	@printf '  %-22s %-13s %s\n' \
		 'debug'             '(rootfs)'     'QEMU paused, GDB :1234 (-S -s)';
	@echo ''
	@echo 'aarch64 UEFI bring-up (uefi):'
	@printf '  %-22s %-13s %s\n' \
		 'aarch64-uefi'           '(uefi)' 'Build aarch64 disk + firmware';
	@printf '  %-22s %-13s %s\n' \
		 'aarch64-uefi-kernel'    '(uefi)' 'Build aarch64 kernel.elf only';
	@printf '  %-22s %-13s %s\n' \
		 'run-aarch64-uefi'       '(uefi)' 'QEMU virt + cortex-a53 + virtio-blk (override SMP with SMP=N; AARCH64_UEFI_SMP_DIAGNOSTIC_DTB=0 to skip auto DTB)';
	@echo ''
	@echo 'Validation (x86 rootfs):'
	@printf '  %-22s %-13s %s\n' \
		 'validate'          '(rootfs)'     'kernel ELF + UEFI COFF + profile info';
	@printf '  %-22s %-13s %s\n' \
		 'validate-kernel'   '(rootfs)'     'kernel ELF sanity (no undef, EM_X86_64, exports)';
	@printf '  %-22s %-13s %s\n' \
		 'validate-uefi'     '(rootfs)'     'EFI app COFF exports parseable';
	@printf '  %-22s %-13s %s\n' \
		 'validate-profile'  '(always)'     'Print profile / triple / sysroot / capabilities';
	@echo ''
	@echo 'Inspection:'
	@printf '  %-22s %-13s %s\n' \
		 'print-run-paths'   '(rootfs)'     'Print absolute firmware + active image path';
	@echo ''
	@echo 'Test (6 canonical buckets; varied capability):'
	@printf '  %-22s %-13s %s\n' \
		 'test-qemu'           '(rootfs)'     'QEMU E2E suite (SUITE=<phase-0|systest|inittab-phase|network>)';
	@printf '  %-22s %-13s %s\n' \
		 'test-host'           '(rootfs)'     'os01_submake hosttests + pmm_boot_reservation_test.py';
	@printf '  %-22s %-13s %s\n' \
		 'test-static'         '(rootfs)'     'All 8 static audits (runtime, stack-canary, validate-kernel, link-order, kernel-layout, kernel-canary-contract, test-user-canary)';
	@printf '  %-22s %-13s %s\n' \
		 'test-kernel-selftest' '(rootfs)'   'QEMU built-in selftests (isolated selftest image, KERNEL_SELFTEST=1)';
	@printf '  %-22s %-13s %s\n' \
		 'test-aarch64'        '(uefi)'       'aarch64 UEFI test (MODE=<smp|no-ack|gic-spi>)';
	@printf '  %-22s %-13s %s\n' \
		 'test-contract'       '(rootfs|uefi)' 'Full build contract (PROFILE=<x86_64-clang|aarch64-clang>)';
	@echo ''
	@echo 'Standalone test targets (distinct harness / image variant):'
	@printf '  %-22s %-13s %s\n' \
		 'test-syscall-repeat'   '(rootfs)'   'QEMU exec/exit stability through normal terminal (x86_64_systest_repeat.py)';
	@printf '  %-22s %-13s %s\n' \
		 'test-user-canary'      '(rootfs)'   '7-step SSP / crt0 user-stack canary audit (spec 2026-09-17)';
	@printf '  %-22s %-13s %s\n' \
		 'test-pmm-boot-reservation' '(rootfs)' 'PMM boot-time memory reservation guard (host-only)';
	@echo ''
	@echo 'Focused compatibility checks (retained; see docs/build-system-harness.md §4):'
	@printf '  %-22s %-13s %s\n' \
		 'test-kernel-layout'    '(rootfs)'   'x86_64 kernel.elf layout audit (post-_end reserved)';
	@printf '  %-22s %-13s %s\n' \
		 'test-kernel-canary-contract' '(rootfs)' 'Kernel canary compile-flag contract';
	@echo ''
	@echo 'Maintenance:'
	@printf '  %-22s %-13s %s\n' \
		 'clean'             '(always)'     'Remove build/<profile> + root compat copies (lock-checked)';
	@printf '  %-22s %-13s %s\n' \
		 'unlock-profile'    '(always)'     'Diagnose stale publish lock (FORCE_UNLOCK=1 to remove)';
	@echo ''
	@echo 'Common flags: PROFILE=<name>, DEBUG_CHANNELS=<a,b>, OS01_SYSTEST=1,'
	@echo '              OS01_NETTEST=1, INITTAB_FILE=<path>, KERNEL_SELFTEST=1,'
	@echo '              NDEBUG=1, LOG_TARGET=serial|both.'
	@echo 'See AGENTS.md Quick start and docs/build-run-debug.md for recipes.'
	@echo 'See docs/build-system-harness.md §4 for the retained focused checks (test-runtime, test-kernel-layout, test-kernel-canary-contract, test-user-canary, test-pmm-boot-reservation).'

# ── Image alias ─────────────────────────────────────────────
# `make image` builds the current profile's disk image — variant-resolved
# (OS01_SYSTEST=1 / OS01_NETTEST=1 / INITTAB_FILE=config/inittab.test select
# the isolated variant image; otherwise the normal image).
.PHONY: image
image: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(DISK_IMG))
	$(call require_capability,rootfs)

# ── Build contract checks ───────────────────────────────────
.PHONY: test-contract
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

# ── Clean ───────────────────────────────────────────────────
# Only the default profile owns the project-root kernel.bin / disk.img compat
# copies; other profiles remove just build/<profile>. The obsolete source-tree
# boot/uefi/OVMF.fd is removed at ROOT level (never by recursing into a
# component) and only when it passes both guards: git-ignored AND untracked.
# A tracked or user-owned file is preserved with an error.
CLEAN_COMPAT := $(if $(filter $(DEFAULT_PROFILE),$(PROFILE)),rm -f disk.img kernel.bin;)

# clean takes the publish lock (60 s retry), fails without deleting anything
# if a generation read lease exists, then removes build/<profile> (NOT the
# whole build/ tree — build/.locks/ and other profiles survive) plus, for the
# default profile, the project-root compat artifacts. Component Makefiles are
# never recursed into: they are profile-only now, and clean already removes
# every component's profile output wholesale. All of it runs in ONE shell line
# so the trap releases the lock in every path (success or failure), and the
# lock is held for the whole clean.
.PHONY: clean unlock-profile
clean:
	@if [ -n "$(DRY_RUN)" ]; then \
	  echo "  [clean] dry-run: not deleting the $(PROFILE) build"; \
	  exit 0; \
	fi; \
	set -e; \
	mkdir -p "$(dir $(LOCK_DIR))"; \
	i=0; \
	while ! mkdir "$(LOCK_DIR)" 2>/dev/null; do \
	  i=$$((i+1)); \
	  if [ $$i -ge 600 ]; then \
	    echo "ERROR: publish lock $(LOCK_DIR) held by:"; \
	    cat "$(LOCK_DIR)/owner" 2>/dev/null || true; \
	    exit 1; \
	  fi; \
	  sleep 0.1; \
	done; \
	trap 'rm -f "$(LOCK_DIR)/owner"; rmdir "$(LOCK_DIR)" 2>/dev/null || true' EXIT; \
	echo "$$$$ $(MAKECMDGOALS) $$(date +%s)" > "$(LOCK_DIR)/owner"; \
	if [ -d "$(LEASES_DIR)" ] && [ -n "$$(ls -A "$(LEASES_DIR)" 2>/dev/null)" ]; then \
	  echo "ERROR: cannot clean while sysroot generations are leased:"; \
	  ls -A "$(LEASES_DIR)"; \
	  echo "lock owner: $$(cat "$(LOCK_DIR)/owner")"; \
	  exit 1; \
	fi; \
	rm -rf build/$(PROFILE); \
	$(CLEAN_COMPAT) \
	if [ -f boot/uefi/OVMF.fd ]; then \
	  if git check-ignore -q boot/uefi/OVMF.fd && ! git ls-files --error-unmatch boot/uefi/OVMF.fd >/dev/null 2>&1; then \
	    rm -f boot/uefi/OVMF.fd; \
	  else \
	    echo "ERROR: refusing to remove boot/uefi/OVMF.fd (not a gitignored, untracked generated artifact); preserving it" >&2; \
	  fi; \
	fi; \
	rm -rf sysroot

# Diagnose and remove a stale publish lock. Prints the owner data and only
# removes the lock when FORCE_UNLOCK=1 is set.
unlock-profile:
	@if [ -d "$(LOCK_DIR)" ]; then \
	  echo "publish lock $(LOCK_DIR):"; \
	  cat "$(LOCK_DIR)/owner" 2>/dev/null || true; \
	  if [ "$$FORCE_UNLOCK" = "1" ]; then \
	    rm -rf "$(LOCK_DIR)"; \
	    echo "lock removed"; \
	  else \
	    echo "set FORCE_UNLOCK=1 to remove the stale lock"; \
	    exit 1; \
	  fi; \
	else \
	  echo "no lock held at $(LOCK_DIR)"; \
	fi
