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
QEMU_IMAGE ?= $(NORMAL_IMAGE)
RUN_QEMU_DISK = -drive file=$(QEMU_IMAGE),format=raw,if=none,id=disk \
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
	$(RUN_QEMU_BASE) -netdev user,id=net0 -device e1000,netdev=net0 $(RUN_QEMU_DISK) -no-reboot
run-kvm:    $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(NORMAL_IMAGE) $(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	$(RUN_QEMU_BASE) $(RUN_QEMU_FLAGS_run-kvm) -netdev user,id=net0 -device e1000,netdev=net0 $(RUN_QEMU_DISK) -no-reboot
run-virtio: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(NORMAL_IMAGE) $(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	$(RUN_QEMU_BASE) -netdev user,id=net0 -device virtio-net-pci,disable-modern=on,netdev=net0 $(RUN_QEMU_DISK)
debug: QEMU_IMAGE = $(DISK_IMG)
debug:      $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(DISK_IMG) $(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	$(RUN_QEMU_BASE) $(RUN_QEMU_FLAGS_debug) -netdev user,id=net0 -device e1000,netdev=net0 $(RUN_QEMU_DISK)

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

# M3.1 audit gate (Task 13): nm half. The compiled aarch64 kernel must
# carry NO x86-only VMM symbols — no T/t symbol named vma_* / uaccess_* /
# fork_* (vmm_* / arch_vmm_* are deliberately excluded: aarch64 has its
# own page_table/vmm_gate surface). The source-scan twin of this gate is
# hosttests/cases/test_vmm_caller_audit.c; the full chain-by-chain table
# lives in docs/memory/memory.md ("vmm 变更调用链审计（M3.1 验收）").
# llvm-nm host tool (same LLVM install that provides this profile's
# llvm-ar / llvm-objcopy; the aarch64 profile does not define LLVM_NM,
# which belongs to the x86 clang toolchain discovery).
AARCH64_NM     ?= llvm-nm
# The aarch64 profile defines no x86 toolchain variables (no LLVM_NM /
# LLVM_OBJDUMP): the intr-dispatch wiring audit needs a disassembler for
# the aarch64 ELF, so it gets its own overridable host tool here.  No
# x86 toolchain include is pulled into the aarch64 path.
AARCH64_OBJDUMP ?= llvm-objdump
.PHONY: test-aarch64-audit
test-aarch64-audit: $(BUILD_DIR)/artifacts/kernel.elf
	$(call require_aarch64_uefi)
	$(call require_capability,uefi)
	@echo "  [audit] aarch64 kernel.elf: no vma_*/uaccess_*/fork_* T/t symbols"
	@bad="$$($(AARCH64_NM) $(BUILD_DIR)/artifacts/kernel.elf | awk '$$2 == "T" || $$2 == "t" { print $$3 }' | grep -E '^(vma_|uaccess_|fork_)')"; \
	  if [ -n "$$bad" ]; then \
	    echo "AUDIT GATE FAIL: forbidden symbols in aarch64 kernel.elf:" $$bad >&2; exit 1; \
	  fi
	@echo "  [audit] OK (vma_*/uaccess_*/fork_* symbol-exclusion gate only)"
	$(RUN_STATIC_AUDIT) intr-dispatch-audit -- \
	  python3 qemutests/intr_dispatch_audit.py \
	    --profile "$(PROFILE)" \
	    --elf "$(BUILD_DIR)/artifacts/kernel.elf" \
	    --llvm-nm "$(AARCH64_NM)" \
	    --llvm-objdump "$(AARCH64_OBJDUMP)" \
	    --repo-root "$(OS01_ROOT)"
# Manual negative control (Fix round 1): the same pipeline run against the
# x86 kernel.elf must hit symbols (proves the filter works):
#   make PROFILE=x86_64-clang kernel.bin
#   llvm-nm build/x86_64-clang/kernel/kernel.elf | awk '$2=="T"||$2=="t"{print $3}' \
#     | grep -E '^(vma_|uaccess_|fork_)'   # expect: vma_find, fork_vma_copy, ...

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
	    -machine "dumpdtb=$$sparse" -display $(DISPLAY) -m $(MEMORY); \
	  dtc -I dtb -O dtb -o "$$packed" "$$sparse"; \
	  rm -f "$$sparse"; \
	  extra_dtb="-dtb $$packed" ;; \
	esac; \
	$(AARCH64_QEMU) -M virt,gic-version=2$${extra_dtb:+,acpi=off} -cpu cortex-a53 -smp $(SMP) -m $(MEMORY) \
	  -drive if=pflash,format=raw,readonly=on,file=$(AARCH64_UEFI_FIRMWARE) \
	  -drive if=none,file=$(AARCH64_UEFI_DISK),format=raw,readonly=on,id=disk \
	  -device virtio-blk-device,drive=disk \
	  -device ramfb \
	  $$extra_dtb \
	  -serial stdio -display $(DISPLAY) -no-reboot

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
TEST_AARCH64_EXTRA_smp        := $(if $(filter 0,$(AARCH64_UEFI_SMP_DIAGNOSTIC_DTB)),,--diagnostic-dtb=auto)
TEST_AARCH64_EXTRA_no-ack     := $(if $(filter 0,$(AARCH64_UEFI_SMP_DIAGNOSTIC_DTB)),,--diagnostic-dtb=auto)
TEST_AARCH64_EXTRA_gic-spi    := --diagnostic-dtb=auto
# sync-fault has no DTB-mode knob — it runs the EL1h sync fault probe on
# the existing UEFI/DTB mechanism but with an isolated image/firmware tree.

# Pre-build helpers (one per MODE) put the $(MAKE) sub-invocation on its
# own recipe line so `make -n` honors the dry-run contract. A single
# multi-branch recipe (the v2 approach with `if [ "$(MODE)" = ... ]`)
# put $(MAKE) inside an `if/fi` block on one recipe line and would
# execute under -n.
.PHONY: _test-aarch64-prep-smp _test-aarch64-prep-no-ack _test-aarch64-prep-gic-spi _test-aarch64-prep-sync-fault
_test-aarch64-prep-smp:
	$(MAKE) KERNEL_SELFTEST=1 aarch64-uefi
_test-aarch64-prep-no-ack:
	@test "$(AARCH64_SMP_TEST_NO_ACK_CPU)" = 1 \
	  || { echo "AARCH64_SMP_TEST_NO_ACK_CPU must be 1; clean and rebuild" >&2; exit 1; }
	@test -f "$(AARCH64_UEFI_DISK)" -a -f "$(AARCH64_UEFI_FIRMWARE)" \
	  || { echo "Build the injected aarch64-uefi image first" >&2; exit 1; }
_test-aarch64-prep-gic-spi:
	$(MAKE) KERNEL_SELFTEST=1 aarch64-uefi
# AAGU-EL1-sync (spec §5): the sync-fault variant builds the dedicated
# kernel/image/firmware tree (KERNEL_VARIANT=sync-fault) and propagates
# the fault injection flag through the controlled sub-make boundary. The
# recipe mirrors _test-aarch64-prep-smp so the dry-run contract still
# honours `make -n`; Task 3's harness target consumes the produced
# image/firmware from AARCH64_UEFI_SYNC_FAULT_{DISK,FIRMWARE}.
_test-aarch64-prep-sync-fault:
	$(MAKE) KERNEL_SELFTEST=1 AARCH64_SYNC_FAULT_TEST=1 aarch64-uefi

# Run helpers (one per MODE) keep the python harness on its own recipe
# line, after the pre-build. The python call therefore is NOT executed
# under `-n` (per the dry-run contract).
.PHONY: _test-aarch64-run-smp _test-aarch64-run-no-ack _test-aarch64-run-gic-spi _test-aarch64-run-sync-fault
_test-aarch64-run-smp:
	python3 qemutests/aarch64_uefi_smp.py \
	  --cpus 1 2 4 --repeat 3 --timeout 90 --expect-selftest --expect-gic --expect-clk \
	  --expect-slab-selftest --expect-m3-selftest --expect-m3mc-selftest \
	  $(TEST_AARCH64_EXTRA_smp) \
	  --firmware "$(AARCH64_UEFI_SELFTEST_FIRMWARE)" \
	  --image "$(AARCH64_UEFI_SELFTEST_DISK)" \
	  --qemu "$(AARCH64_QEMU)" \
	  --build-dir "$(BUILD_DIR)" --profile "$(PROFILE)" \
	  --log-dir "$(OS01_ROOT)/test-results/aarch64-uefi-smp/$$(date -u +%Y%m%dT%H%M%S)-normal-$$$$"
_test-aarch64-run-no-ack:
	python3 qemutests/aarch64_uefi_smp.py \
	  --cpus 2 --repeat 1 --timeout 90 --expect-no-ack 1 \
	  $(TEST_AARCH64_EXTRA_no-ack) \
	  --firmware "$(AARCH64_UEFI_FIRMWARE)" \
	  --image "$(AARCH64_UEFI_DISK)" \
	  --qemu "$(AARCH64_QEMU)" \
	  --build-dir "$(BUILD_DIR)" --profile "$(PROFILE)" \
	  --log-dir "$(OS01_ROOT)/test-results/aarch64-uefi-smp/$$(date -u +%Y%m%dT%H%M%S)-no-ack-$$$$"
_test-aarch64-run-gic-spi:
	python3 qemutests/aarch64_gic_spi.py \
	  --diagnostic-dtb=auto \
	  --firmware "$(AARCH64_UEFI_SELFTEST_FIRMWARE)" \
	  --image "$(AARCH64_UEFI_SELFTEST_DISK)" \
	  --qemu "$(AARCH64_QEMU)" \
	  --build-dir "$(BUILD_DIR)" --profile "$(PROFILE)" \
	  --log-dir "$(OS01_ROOT)/test-results/aarch64-gic-spi/$$(date -u +%Y%m%dT%H%M%S)-$$$$"
# AAGU-EL1-sync Task 3: drives the dedicated sync-fault QEMU image
# (KERNEL_VARIANT=sync-fault, AARCH64_UEFI_SYNC_FAULT_{DISK,FIRMWARE}) in 1-CPU
# mode and asserts the EL1h sync fatal diagnostic via
# qemutests/aarch64_sync_fault.py. The harness parser is the only
# success criterion: timeouts do NOT count, and the run target only
# consumes the dedicated sync-fault paths (never the selftest image).
_test-aarch64-run-sync-fault:
	python3 qemutests/aarch64_sync_fault.py \
	  --timeout 60 \
	  --firmware "$(AARCH64_UEFI_SYNC_FAULT_FIRMWARE)" \
	  --image "$(AARCH64_UEFI_SYNC_FAULT_DISK)" \
	  --qemu "$(AARCH64_QEMU)" \
	  --build-dir "$(BUILD_DIR)" --profile "$(PROFILE)" \
	  --log-dir "$(OS01_ROOT)/test-results/aarch64-sync-fault/$$(date -u +%Y%m%dT%H%M%S)-$$$$"

# M3.5 Task 25: production shootdown probe (spec §7.3). Builds the
# PRODUCTION image (no KERNEL_SELFTEST=1) and runs it at -smp 2; the
# harness greps for the exact `M3-SHOOTDOWN-PROBE: START` → `OK`
# sequence and fails on any FAIL / SKIP line or timeout. -smp 1 is
# intentionally NOT covered here: main.c gates the probe call on
# dtb_cpu_count() >= 2 so single-CPU boots skip it (the 0-AP FAIL
# contract is pinned by the hosttest instead).
.PHONY: _test-aarch64-prep-m3-probe _test-aarch64-run-m3-probe
_test-aarch64-prep-m3-probe:
	$(MAKE) aarch64-uefi
_test-aarch64-run-m3-probe:
	python3 qemutests/aarch64_m3_probe.py \
	  --diagnostic-dtb=auto \
	  --timeout 90 \
	  --firmware "$(AARCH64_UEFI_FIRMWARE)" \
	  --image "$(AARCH64_UEFI_DISK)" \
	  --qemu "$(AARCH64_QEMU)" \
	  --build-dir "$(BUILD_DIR)" --profile "$(PROFILE)" \
	  --log-dir "$(OS01_ROOT)/test-results/aarch64-m3-probe/$$(date -u +%Y%m%dT%H%M%S)-$$$$"

# test-aarch64: the umbrella. Dispatches to the per-MODE prep + run helpers.
test-aarch64: MODE ?= smp
test-aarch64: MODE := $(MODE)
# No image prerequisites: smp/gic-spi build the selftest variant in their
# prep helper, while no-ack must only consume an already injected image.
# sync-fault is added to the gate in line with the dedicated _test-aarch64-
# prep-sync-fault / _test-aarch64-run-sync-fault pair (Task 3 wires the
# run target and harness parser; this umbrella already accepts the mode).
test-aarch64:
	$(call require_aarch64_uefi)
	$(call require_capability,uefi)
	@case "$(MODE)" in \
	  smp|no-ack|gic-spi|sync-fault|m3-probe|m1-ram|m1-sparse|m1-arena-exhaust|m1-table-exhaust|m1-ap-bad-root) ;; \
	  *) echo "MODE must be smp|no-ack|gic-spi|sync-fault|m3-probe|m1-ram|m1-sparse|m1-arena-exhaust|m1-table-exhaust|m1-ap-bad-root, got '$(MODE)'" >&2; exit 1;; \
	esac
	@echo "  [test-aarch64] MODE=$(MODE) extra=$(TEST_AARCH64_EXTRA_$(MODE))"
	$(MAKE) --no-print-directory _test-aarch64-prep-$(MODE)
	$(MAKE) --no-print-directory _test-aarch64-run-$(MODE)

# Convenience alias: `make PROFILE=aarch64-clang test-aarch64-m3-probe`
# == `make PROFILE=aarch64-clang test-aarch64 MODE=m3-probe`.
.PHONY: test-aarch64-m3-probe
test-aarch64-m3-probe:
	$(MAKE) --no-print-directory test-aarch64 MODE=m3-probe

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

.PHONY: test-host test-pmm-boot-reservation test-gfx-file-lifecycle test-gfx-fault-bridge test-gfx-device test-gfx-client test-gfx-primitives test-m1-host test-arch9-host test-resolution-host test-gui-host
test-host:
	$(call require_capability,rootfs)
	@$(call os01_submake,hosttests,run $(OS01_SUBMAKE_ARGS))
	python3 qemutests/pmm_boot_reservation_test.py
	python3 qemutests/test_kernel_selftest_result.py
	python3 tools/tests/test_user_build.py
	python3 tools/tests/test_kernel_headers.py
test-pmm-boot-reservation:
	python3 qemutests/pmm_boot_reservation_test.py

# ── Test framework regression (no QEMU, always) ──────────────
# Explicit Python unittest entry for the test framework itself.
# The list is append-only across tasks: Task 2 established
# `qemutests.test_gfx_runner` as the first module; later tasks that add
# a new `qemutests/test_*.py` module append its name here in the same
# commit (Ruling 3), so a single `make test-harness` validates the
# framework end-to-end before any real QEMU run.  Task 13 (final
# acceptance) also folded in the pre-existing host-only runner
# self-tests that no other target ran (`test_driver_model_matrix`,
# `test_driver_model_boundary_audit`, `test_resolution_switcher`) plus
# `test_ci_workflow` (a static contract check over
# .github/workflows/ci.yml).  `test_lvgl_runner` is deliberately NOT
# listed: it is an executable QEMU driver script, not a unittest module.
# `test_arch9_build_contract` is also deliberately NOT listed: its
# fixtures drive real `make -n kernel.bin`, which needs a prepared
# sysroot, so it cannot run in this zero-environment-dependency gate
# (the CI `harness` job builds nothing before `make test-harness`).  It
# IS owned by the `test-contract` bucket instead: that target *does*
# build a sysroot, and its x86_64-clang branch runs this module after
# the `build_contract.sh` mode loop.
# The recipe is a single `python3 -m unittest` call against the explicit
# list — never auto-discovery, never an implicit search for `test_*.py`.
# Fixtures use fakes or host-only contract probes; no QEMU process starts.
TEST_HARNESS_MODULES := qemutests.test_gfx_runner qemutests.test_harness_process qemutests.test_harness_result qemutests.test_run_test_harness qemutests.test_make_qemu_failure qemutests.test_kernel_selftest_result qemutests.test_run_hosttests qemutests.test_systest_protocol qemutests.test_aarch64_harness qemutests.test_systest_repeat_harness qemutests.test_harness_static qemutests.test_driver_model_matrix qemutests.test_driver_model_boundary_audit qemutests.test_resolution_switcher qemutests.test_ci_workflow qemutests.test_monitor_runner qemutests.test_terminal_present_runner qemutests.test_gui_runner qemutests.test_monitor_host_dispatch qemutests.test_intr_dispatch_audit
.PHONY: test-harness test-framework-contract
test-harness: test-framework-contract
	@echo "  [test-harness] running $(words $(TEST_HARNESS_MODULES)) unittest module(s): $(TEST_HARNESS_MODULES)"
	python3 -m unittest $(TEST_HARNESS_MODULES)

# Negative-contract hosttest: builds test_framework_contract.elf and
# asserts its exit code is exactly 1.  Pins three test-framework
# contracts (assertion exit-code propagation, macro single-evaluation,
# TEST_RESULTS() not resetting stats — see
# hosttests/cases/test_framework_contract.c header for the spec and
# commit 2d594ce0 for the original fix).  Wired into `make test-harness`
# so CI catches regressions; safe to run standalone (no QEMU, no
# kernel, single-file compile).
test-framework-contract:
	$(call os01_submake,hosttests,test-framework-contract $(OS01_SUBMAKE_ARGS))

# Focused M1 host tests (aarch64 M1 plan). Each CASE runs the matching
# focused TEST_BINS entry under hosttests/.  Unknown non-empty CASE
# aborts non-zero. With CASE omitted the umbrella runs the full M1
# group (currently: layout). The target builds only the focused
# TEST_BINS entry, not the entire hosttest build, so `make ... CASE=...`
# is cheap when iterating on a single calculator.
#
# The x86_64-clang profile forwards to hosttests; aarch64-clang also
# routes through hosttests (the M1 host tests are arch-neutral C).
#
# Implementation note: os01_submake expands to a `+env -i ...` line,
# which only parses as a Make recipe prefix when the call sits at the
# start of its OWN recipe line. We therefore give each M1 case its own
# .PHONY runner and let test-m1-host select among them via CASE.
.PHONY: test-m1-host _test-m1-host-run-m1-layout _test-m1-host-run-m1-reservation _test-m1-host-run-m1-arena _test-m1-host-run-m1-tree _test-m1-host-run-m1-contract-x86
M1_CASES := m1-layout m1-reservation m1-arena m1-tree m1-contract-x86 m1-install m1-publish
_test-m1-host-run-m1-layout:
	@echo "  [test-m1-host] m1-layout"
	$(call os01_submake,hosttests,test-m1-layout $(OS01_SUBMAKE_ARGS))
_test-m1-host-run-m1-reservation:
	@echo "  [test-m1-host] m1-reservation"
	$(call os01_submake,hosttests,test-m1-reservation $(OS01_SUBMAKE_ARGS))
_test-m1-host-run-m1-arena:
	@echo "  [test-m1-host] m1-arena"
	$(call os01_submake,hosttests,test-m1-arena $(OS01_SUBMAKE_ARGS))
_test-m1-host-run-m1-tree:
	@echo "  [test-m1-host] m1-tree"
	$(call os01_submake,hosttests,test-m1-tree $(OS01_SUBMAKE_ARGS))
_test-m1-host-run-m1-contract-x86:
	@echo "  [test-m1-host] m1-contract-x86"
	$(call os01_submake,hosttests,test-m1-contract-x86 $(OS01_SUBMAKE_ARGS))
_test-m1-host-run-m1-install:
	$(call os01_submake,hosttests,test-m1-install $(OS01_SUBMAKE_ARGS))
_test-m1-host-run-m1-publish:
	$(call os01_submake,hosttests,test-m1-publish $(OS01_SUBMAKE_ARGS))
test-m1-host: CASE ?=
test-m1-host: CASE := $(CASE)
# Umbrella: with empty CASE, depend on every per-case runner; otherwise
# depend on the single matching one. The dispatch is purely structural
# so make schedules the right os01_submake invocations.
test-m1-host: $(if $(CASE),_test-m1-host-run-$(CASE),$(foreach c,$(M1_CASES),_test-m1-host-run-$(c)))
	$(call require_capability,rootfs)
	@case "$(CASE)" in \
	    "") echo "  [test-m1-host] full M1 group: $(M1_CASES)";; \
	    m1-layout) echo "  [test-m1-host] CASE=$(CASE)";; \
	    m1-reservation) echo "  [test-m1-host] CASE=$(CASE)";; \
	    m1-arena) echo "  [test-m1-host] CASE=$(CASE)";; \
	    m1-tree) echo "  [test-m1-host] CASE=$(CASE)";; \
	    m1-publish) echo "  [test-m1-host] CASE=$(CASE)";; \
	    m1-install) echo "  [test-m1-host] CASE=$(CASE)";; \
	    m1-contract-x86) echo "  [test-m1-host] CASE=$(CASE)";; \
	    *) echo "ERROR: unknown CASE='$(CASE)'; valid: $(M1_CASES)" >&2; exit 1;; \
	  esac

# ARCH-9 driver model host tests.
.PHONY: test-arch9-host _test-arch9-host-run-block _test-arch9-host-run-pci _test-arch9-host-run-backend _test-arch9-host-run-ahci _test-arch9-host-run-device-boot _test-arch9-host-run-net _test-arch9-host-run-lwip _test-arch9-host-run-e1000 _test-arch9-host-run-virtio _test-arch9-host-run-socket
ARCH9_CASES := block pci backend ahci device-boot net lwip e1000 virtio socket
_test-arch9-host-run-block:
	@echo "  [test-arch9-host] block"
	$(call os01_submake,hosttests,test-block $(OS01_SUBMAKE_ARGS))
_test-arch9-host-run-pci:
	@echo "  [test-arch9-host] pci"
	$(call os01_submake,hosttests,test-pci $(OS01_SUBMAKE_ARGS))
_test-arch9-host-run-backend:
	@echo "  [test-arch9-host] backend"
	$(call os01_submake,hosttests,test-backend $(OS01_SUBMAKE_ARGS))
_test-arch9-host-run-ahci:
	@echo "  [test-arch9-host] ahci"
	$(call os01_submake,hosttests,test-ahci $(OS01_SUBMAKE_ARGS))
_test-arch9-host-run-device-boot:
	@echo "  [test-arch9-host] device-boot"
	$(call os01_submake,hosttests,test-device-boot $(OS01_SUBMAKE_ARGS))
_test-arch9-host-run-net:
	@echo "  [test-arch9-host] net"
	$(call os01_submake,hosttests,test-net $(OS01_SUBMAKE_ARGS))
_test-arch9-host-run-lwip:
	@echo "  [test-arch9-host] lwip"
	$(call os01_submake,hosttests,test-lwip $(OS01_SUBMAKE_ARGS))
_test-arch9-host-run-e1000:
	@echo "  [test-arch9-host] e1000"
	$(call os01_submake,hosttests,test-e1000 $(OS01_SUBMAKE_ARGS))
_test-arch9-host-run-virtio:
	@echo "  [test-arch9-host] virtio"
	$(call os01_submake,hosttests,test-virtio $(OS01_SUBMAKE_ARGS))
_test-arch9-host-run-socket:
	@echo "  [test-arch9-host] socket"
	$(call os01_submake,hosttests,test-socket $(OS01_SUBMAKE_ARGS))
test-arch9-host: CASE ?=
test-arch9-host: CASE := $(CASE)
test-arch9-host: $(if $(CASE),_test-arch9-host-run-$(CASE),$(foreach c,$(ARCH9_CASES),_test-arch9-host-run-$(c)))
	$(call require_capability,rootfs)
	@case "$(CASE)" in \
	    "") echo "  [test-arch9-host] full ARCH-9 group: $(ARCH9_CASES)";; \
	    block) echo "  [test-arch9-host] CASE=$(CASE)";; \
	    pci) echo "  [test-arch9-host] CASE=$(CASE)";; \
	    backend) echo "  [test-arch9-host] CASE=$(CASE)";; \
	    ahci) echo "  [test-arch9-host] CASE=$(CASE)";; \
	    device-boot) echo "  [test-arch9-host] CASE=$(CASE)";; \
	    net) echo "  [test-arch9-host] CASE=$(CASE)";; \
	    lwip) echo "  [test-arch9-host] CASE=$(CASE)";; \
	    e1000) echo "  [test-arch9-host] CASE=$(CASE)";; \
	    virtio) echo "  [test-arch9-host] CASE=$(CASE)";; \
	    socket) echo "  [test-arch9-host] CASE=$(CASE)";; \
	    *) echo "ERROR: unknown CASE='$(CASE)'; valid: $(ARCH9_CASES)" >&2; exit 1;; \
	  esac

# ── Resolution Switcher host tests ───────────────────────────
# Focused host tests for the QEMU resolution switcher plan.
# RES_CASE accepts uapi, state, writers, bga, ioctl, pty, terminal,
# clients, hooks, or all.
# With RES_CASE omitted or all, the umbrella runs all currently registered cases.
# Unregistered cases and unknown cases abort non-zero.
.PHONY: test-resolution-host _test-resolution-host-run-uapi _test-resolution-host-run-state _test-resolution-host-run-writers _test-resolution-host-run-bga _test-resolution-host-run-ioctl _test-resolution-host-run-pty _test-resolution-host-run-terminal _test-resolution-host-run-clients _test-resolution-host-run-hooks
RESOLUTION_HOST_ALL_CASES := uapi state writers bga ioctl pty terminal clients hooks
RESOLUTION_HOST_REGISTERED_CASES := uapi state writers bga ioctl pty terminal clients hooks

_test-resolution-host-run-uapi:
	@echo "  [test-resolution-host] uapi"
	$(call os01_submake,hosttests,test-fb-uapi $(OS01_SUBMAKE_ARGS))

_test-resolution-host-run-state:
	@echo "  [test-resolution-host] state"
	$(call os01_submake,hosttests,test-fb-state $(OS01_SUBMAKE_ARGS))

_test-resolution-host-run-writers:
	@echo "  [test-resolution-host] writers"
	$(call os01_submake,hosttests,test-fb-writers $(OS01_SUBMAKE_ARGS))

_test-resolution-host-run-bga:
	@echo "  [test-resolution-host] bga"
	$(call os01_submake,hosttests,test-bga $(OS01_SUBMAKE_ARGS))

_test-resolution-host-run-ioctl:
	@echo "  [test-resolution-host] ioctl"
	$(call os01_submake,hosttests,test-fb-ioctl $(OS01_SUBMAKE_ARGS))

_test-resolution-host-run-pty:
	@echo "  [test-resolution-host] pty"
	$(call os01_submake,hosttests,test-pty-winsize $(OS01_SUBMAKE_ARGS))

_test-resolution-host-run-terminal:
	@echo "  [test-resolution-host] terminal"
	$(call os01_submake,hosttests,test-terminal-display $(OS01_SUBMAKE_ARGS))

_test-resolution-host-run-clients:
	@echo "  [test-resolution-host] clients"
	$(call os01_submake,hosttests,test-clients $(OS01_SUBMAKE_ARGS))

# hooks: the Task-9 controlled fault-injection surface.  The C harness
# links the REAL fb_test.c + fb_state.c/bga.c/fb.c built with
# FB_RESOLUTION_TEST; the python harness asserts the build profile is
# isolated (resolution-test image dir, parse-time rejection of unknown
# values / conflicting variants / non-x86 targets).
_test-resolution-host-run-hooks:
	@echo "  [test-resolution-host] hooks"
	$(call os01_submake,hosttests,test-fb-hooks $(OS01_SUBMAKE_ARGS))
	python3 qemutests/fb_resolution_profile_test.py

_test-resolution-host-run-%:
	@echo "ERROR: unknown or unregistered RES_CASE='$*'; valid: $(RESOLUTION_HOST_ALL_CASES) all (registered: $(RESOLUTION_HOST_REGISTERED_CASES))" >&2
	@exit 1

RES_CASE ?= all
RES_CASE_EFFECTIVE = $(if $(strip $(RES_CASE)),$(RES_CASE),all)
test-resolution-host: RES_CASE := $(RES_CASE)
test-resolution-host: $(if $(filter all,$(RES_CASE_EFFECTIVE)),$(foreach c,$(RESOLUTION_HOST_REGISTERED_CASES),_test-resolution-host-run-$(c)),_test-resolution-host-run-$(RES_CASE_EFFECTIVE))
	$(call require_capability,rootfs)
	@case "$(RES_CASE_EFFECTIVE)" in \
	    all) echo "  [test-resolution-host] full registered group: $(RESOLUTION_HOST_REGISTERED_CASES)";; \
	    *) echo "  [test-resolution-host] RES_CASE=$(RES_CASE_EFFECTIVE)";; \
	  esac

# ── GUI host tests ───────────────────────────────────────────
# Focused host tests for the GUI plans: the GUI-1 LVGL platform backend,
# the GUI-2 input-focus chain and the GUI-3 Material design system.
# CASE accepts any registered case (platform, lvgl, devfs-input,
# input-frame, input-route, input-decode, input-platform, theme) or all
# (default: all).
# Unknown CASE aborts non-zero before invoking submake.
.PHONY: test-gui-host _test-gui-host-run-platform _test-gui-host-run-lvgl _test-gui-host-run-devfs-input _test-gui-host-run-input-frame _test-gui-host-run-input-route _test-gui-host-run-input-decode _test-gui-host-run-input-nonce _test-gui-host-run-input-platform _test-gui-host-run-input-lvgl _test-gui-host-run-theme _test-gui-host-run-theme-apply _test-gui-host-run-desktop-state
GUI_HOST_REGISTERED_CASES := platform lvgl devfs-input input-frame input-route input-decode input-nonce input-platform input-lvgl theme theme-apply desktop-state

_test-gui-host-run-platform:
	@echo "  [test-gui-host] platform"
	$(call os01_submake,hosttests,test-gui-platform $(OS01_SUBMAKE_ARGS))

_test-gui-host-run-lvgl:
	@echo "  [test-gui-host] lvgl"
	$(call os01_submake,hosttests,test-gui-lvgl $(OS01_SUBMAKE_ARGS))

# GUI-2 plan Task 1: file-aware devfs read/poll dispatch.  Runs
# test_devfs_input_dispatch.elf, which host-compiles the REAL
# kernel/fs/devfs.c + kernel/fs/file.c + kernel/fs/poll.c and asserts the
# spec §4.3 devfs contract: the file identity reaches the device, the
# legacy node read/poll stay the fallback for devices without the new
# hooks, a read_file error is not downgraded to the node read, and
# poll_file returning 0 stays "not ready".
_test-gui-host-run-devfs-input:
	@echo "  [test-gui-host] devfs-input"
	$(call os01_submake,hosttests,test-gui-devfs-input $(OS01_SUBMAKE_ARGS))

# GUI-2 plan Task 2: pure PS/2 Set 1 scan-sequence framer.  Runs
# test_input_ps2_frame.elf, which host-compiles the REAL
# kernel/driver/input_ps2.c and asserts the spec §4.3/§6.1 framing
# contract: a complete sequence is the routing unit (the E0 prefix
# never publishes a half sequence), PrintScreen/Pause frame as ONE
# special sequence, start_epoch is the epoch of the FIRST byte, and a
# corrupt sequence recovers to a boundary without emitting a fake Shift.
_test-gui-host-run-input-frame:
	@echo "  [test-gui-host] input-frame"
	$(call os01_submake,hosttests,test-gui-input-frame $(OS01_SUBMAKE_ARGS))

# GUI-2 plan Task 3: the exclusive physical-input lease.  Runs
# test_input_route.elf, which host-compiles the REAL
# kernel/driver/input_route.c (+ the REAL kernel/driver/input_ps2.c
# framer) against the recording runtime in
# hosttests/mock/input_route_test_runtime.{c,h} and asserts the spec
# §4.3 lease contract: the device is NORMAL before the lease, the
# owner's file reads the ACTIVE queue while every other file gets
# EBUSY / POLLERR, an empty ACTIVE queue is EAGAIN, only the FINAL
# close revokes, closing any bound file revokes the whole lease, a
# recycled file address does not release a new lease, a sequence that
# does not fit and the 65th mouse event are EOVERFLOW / POLLERR, a
# sequence is never published in halves, a make that crosses an epoch
# is isolated until its break, the Caps typematic does not flip, the
# mouse button isolation keeps the movement, the lock order is
# route → NORMAL queue/poll, and a publish injected at the
# check-empty → register-waiter boundary is not lost.
#
# GUI-2 plan Task 4 adds the second binary of this case:
# test_gfx_input.elf, which host-compiles the REAL kernel/driver/gfx.c
# (the GFX_BIND_INPUT / GFX_UNBIND_INPUT ioctls), the REAL
# kernel/driver/fb_state.c (the owner / lease coordinator) and the REAL
# kernel/driver/input_route.c (the lease), and asserts the spec §4.1
# contract: the 32-byte INOUT struct, EFAULT / EBADF / EINVAL / EACCES
# / ESTALE / EBUSY, the exclusive lease, the copy-out rollback that
# must not revoke a newer generation, and the libgfx wrapper.
_test-gui-host-run-input-route:
	@echo "  [test-gui-host] input-route"
	$(call os01_submake,hosttests,test-gui-input-route $(OS01_SUBMAKE_ARGS))

# GUI-2 plan Task 5: the native input API surface (libgui/include/gui/
# gui_input.h) and the pure userspace PS/2 decoder (libgui/src/
# input_ps2.c).  Runs test_gui_input_decode.elf, which host-compiles
# the REAL production decoder and asserts the spec §5.1 / §6.1
# contract: the US ASCII layout (letters use Shift XOR Caps, digits
# and punctuation use Shift only), the frozen DOWN/UP/REPEAT values and
# modifier bit layout, the distinct left/right Shift/Ctrl/Alt physical
# identities, the UP identity with codepoint == 0, the typematic make
# that produces no event and does not flip Caps again, the swallowed
# PrintScreen / Pause sequences (no event, no latched Shift or Ctrl),
# every fragment point of an E0 sequence, the 100 ms truncated-prefix
# expiry, the unknown scan code without a character, KP slash / KP
# Enter, and the keys that report their physical identity only.
_test-gui-host-run-input-decode:
	@echo "  [test-gui-host] input-decode"
	$(call os01_submake,hosttests,test-gui-input-decode $(OS01_SUBMAKE_ARGS))

# GUI-2 plan Task 10 (fix round 1): the guest input probe's nonce rule.
# Runs test_gui_input_nonce.elf, which host-compiles the REAL rule
# (user/test_gui_input/gui_input_nonce.h) and asserts that a nonce is hex,
# that a path escape is impossible by construction and that the control
# path is exactly /tmp/gui-input-<hex>.ctl.
_test-gui-host-run-input-nonce:
	@echo "  [test-gui-host] input-nonce"
	$(call os01_submake,hosttests,test-gui-input-nonce $(OS01_SUBMAKE_ARGS))

# GUI-2 plan Task 6: the bounded input lifecycle, the FIFO, the fair pump
# and the platform error convergence.  Runs test_gui_input_platform.elf,
# which host-compiles the REAL libgui input unit (libgui/src/input.c +
# libgui/src/input_lvgl.c), the REAL platform and the REAL PS/2 decoder
# against the recording runtime in
# hosttests/mock/gui_test_runtime.{c,h} and the REAL LVGL archive, and
# asserts the spec §5.1 / §5.2 contract: the fixed creation order and
# its reverse rollback with the first errno, the fd / lease / mapping net
# counts, the keyboard-only degradation (ENOENT/ENODEV only), the EBUSY
# second create, the Caps-only decoder seeding, the mouse EIO on a short
# record, the carried-over keyboard prefix, the EINTR/EAGAIN/legacy-empty
# reads, the POLLERR/POLLHUP/POLLNVAL error mapping, the 512-item FIFO
# capacity, the per-device pump budgets, the 256-item delivery budget,
# the un-merged down/up pair and the owner check that stops the delivery
# of a static view.
_test-gui-host-run-input-platform:
	@echo "  [test-gui-host] input-platform"
	$(call os01_submake,hosttests,test-gui-input-platform $(OS01_SUBMAKE_ARGS))

# GUI-2 plan Task 7: the keyboard half of the LVGL adapter — keypad indev
# behaviour, the software repeat policy, native shortcut delivery and the
# in-process focus group with Tab navigation.  Runs
# test_gui_input_lvgl.elf, which host-compiles the REAL libgui input unit
# (input.c + input_lvgl.c + input_focus.c), the REAL platform and the REAL
# PS/2 decoder, and asserts the spec §6.2 / §7.2 contract against the REAL
# LVGL keypad indev and focus group (nothing under test is mocked).
_test-gui-host-run-input-lvgl:
	@echo "  [test-gui-host] input-lvgl"
	$(call os01_submake,hosttests,test-gui-input-lvgl $(OS01_SUBMAKE_ARGS))

# GUI-3 plan Task 1: the Material design token table.  Runs
# test_gui_theme.elf, which host-compiles the REAL libgui/src/theme.c
# (pure C, no LVGL dependency) and asserts the spec §9.1 contract:
# well-formed color roles, on_*/base pairing, strictly decreasing font
# scale, strictly increasing space and radius scales, monotonic
# elevation shadows, the elevation surface sequence, and -1 for every
# out-of-range enum.
_test-gui-host-run-theme:
	@echo "  [test-gui-host] theme"
	$(call os01_submake,hosttests,test-gui-theme $(OS01_SUBMAKE_ARGS))

# GUI-3 plan Task 2: the Material theme applied to LVGL.  Runs
# test_gui_theme_apply.elf, which links the REAL token table + theme
# apply + platform + LVGL archive and asserts the spec §9.1 apply
# contract (theme attached and idempotent, widget styles sourced from
# the token table, 2px primary focus ring, NULL platform → EINVAL).
_test-gui-host-run-theme-apply:
	@echo "  [test-gui-host] theme-apply"
	$(call os01_submake,hosttests,test-gui-theme-apply $(OS01_SUBMAKE_ARGS))

# GUI-3 plan Task 3: the desktop surface state machine.  Runs
# test_desktop_state.elf, which host-compiles the REAL
# user/desktop/desktop_state.c (pure C, no LVGL dependency) and
# exhausts the (state x intent) matrix against the spec §9.2 contract.
_test-gui-host-run-desktop-state:
	@echo "  [test-gui-host] desktop-state"
	$(call os01_submake,hosttests,test-gui-desktop-state $(OS01_SUBMAKE_ARGS))

_test-gui-host-run-%:
	@echo "ERROR: unknown CASE='$*'; valid: $(GUI_HOST_REGISTERED_CASES), all" >&2
	@exit 1
GUI_CASE_RAW := $(CASE)
GUI_CASE_EFFECTIVE = $(if $(strip $(GUI_CASE_RAW)),$(GUI_CASE_RAW),all)
test-gui-host: $(if $(filter all,$(GUI_CASE_EFFECTIVE)),$(foreach c,$(GUI_HOST_REGISTERED_CASES),_test-gui-host-run-$(c)),_test-gui-host-run-$(GUI_CASE_EFFECTIVE))
	$(call require_capability,rootfs)
	@case "$(GUI_CASE_EFFECTIVE)" in \
	    all) echo "  [test-gui-host] full registered group: $(GUI_HOST_REGISTERED_CASES)";; \
	    *) echo "  [test-gui-host] CASE=$(GUI_CASE_EFFECTIVE)";; \
	  esac


# Focused hosttest for the gfx 2D API plan Task 1 — per-file device
# ioctl/release lifecycle.  Runs the single TEST_BINS entry
# (test_gfx_file_lifecycle.elf) and asserts the contract spelled out
# in spec §3 (devfs_open_node attaches a node ref, devfs_ioctl_file
# prefers ioctl_file and falls through to ioctl on -ENOTTY,
# devfs_release_file fires release_file exactly once on the last
# file_put, and the legacy node-only ioctl / no-op release paths
# still work for devices without file callbacks).
test-gfx-file-lifecycle:
	$(call require_capability,rootfs)
	@$(call os01_submake,hosttests,test-gfx-file-lifecycle $(OS01_SUBMAKE_ARGS))
# Focused hosttest for the gfx 2D API plan Task 2 — bounded /dev/gfx0
# framebuffer present device.  Runs test_gfx_device.elf which
# host-compiles the REAL kernel/driver/gfx.c against the same
# gfx_test_runtime the lifecycle test uses, and asserts the spec §3+§4
# contract: GFX_CREATE_VIEW size/overflow validation, reconfigure
# rejection, 16-slot limit, GFX_GET_INFO local dims, independent
# views, release/reopen, GFX_PRESENT happy path + sentinels outside
# the view + row padding + overlapping views + invalid stride/reserved
# + kernel-range pointer + missing-range-check detection + mid-fault
# partial visibility, unconfigured-view rejection, and unknown-cmd
# ENOTTY.  fb_get_info / fb_write_row are mocked in the test TU
# (kernel/driver/fb.c's heavy VMA/VMM/scheduler chain is out of
# scope for this test; the helpers are tested by the QEMU suite
# as part of the integration tests).
test-gfx-device:
	$(call require_capability,rootfs)
	@$(call os01_submake,hosttests,test-gfx-device $(OS01_SUBMAKE_ARGS))
# Partial-present Task 1 Step 2: host SIGSEGV bridge self-test. Links
# ONLY the bridge + the bridge test TU (no production kernel objects).
# Pin install/restore round-trip, PROT_NONE fault longjmps inside the
# protected region, NULL get_jump does not fake EFAULT, and restore
# returns the host SIGSEGV mask to the pre-install value. Must pass
# before test-gfx-device depends on the bridge for the new rect tests.
test-gfx-fault-bridge:
	$(call require_capability,rootfs)
	@$(call os01_submake,hosttests,test-gfx-fault-bridge $(OS01_SUBMAKE_ARGS))
# Focused hosttest for the gfx 2D API plan Task 3 — libgfx client
# lifecycle.  Runs test_gfx_client.elf which host-compiles the REAL
# libgfx/gfx.c (open/close/get_info/set_clip/present) with HOST_CC
# and links it through -Wl,--wrap=open,--wrap=close,--wrap=ioctl,
# --wrap=malloc,--wrap=free so the test TU observes libgfx's libc
# call pattern.  Asserts the spec §3+§4+§5 contract: call order in
# gfx_open (open → CREATE_VIEW → GET_INFO → 2x malloc), ENOENT
# normalized to ENODEV, cleanup at every failure point (handle
# allocated before CREATE_VIEW is freed on CREATE_VIEW failure,
# GET_INFO failure, and pixel-alloc failure), gfx_get_info(NULL)
# returns zero with errno=EINVAL, gfx_set_clip never issues an
# ioctl (clip is library-local state per spec §4), gfx_present
# issues exactly one ioctl and propagates the ioctl wrapper's
# errno.  gfx_present's request struct carries the buffer pointer
# from the handle and the configured stride.  Kernel-side mocks
# (gfx_test_runtime.h) are NOT in scope here — libgfx has no
# kernel dependencies.
test-gfx-client:
	$(call require_capability,rootfs)
	@$(call os01_submake,hosttests,test-gfx-client $(OS01_SUBMAKE_ARGS))
# Focused hosttest for the gfx 2D API plan Task 4 — libgfx 2D
# primitives (pixel / line / rect / sprite).  Runs
# test_gfx_primitives.elf which host-compiles the REAL libgfx/line.c
# + libgfx/sprite.c and asserts spec §4: pixel/line/rect colour
# correctness, clipping at every boundary (negative / oversized /
# partial / fully-outside / library-local clip rectangle), Bresenham
# in all 8 octants with both endpoints, INT32_MIN/MAX endpoints
# that must not overflow or loop, sprite opaque / color-key=0
# (black also transparent) / mask MSB-first / padded source and
# mask stride.  libgfx/gfx.c is NOT compiled (the primitives test
# exercises in-memory drawing only — no fd, no syscall).
test-gfx-primitives:
	$(call require_capability,rootfs)
	@$(call os01_submake,hosttests,test-gfx-primitives $(OS01_SUBMAKE_ARGS))

# Focused hosttest for the display ownership plan Task 1 — exclusive
# display coordinator and writer admission.
.PHONY: test-display-owner
test-display-owner:
	$(call require_capability,rootfs)
	@$(call os01_submake,hosttests,test-display-owner $(OS01_SUBMAKE_ARGS))

# ── Terminal monitor host tests (2026-10-11 monitor plan) ────
# Focused host tests, one per monitor-plan task.  The CASE whitelist
# grows task by task; an unknown CASE must abort, never silently pass.
# With CASE omitted the umbrella runs the whole registered group.
# Implementation mirrors test-m1-host: each case gets its own .PHONY
# runner because os01_submake must start its own recipe line.
.PHONY: test-monitor-host _test-monitor-host-run-file _test-monitor-host-run-accounting _test-monitor-host-run-pmm _test-monitor-host-run-procfs _test-monitor-host-run-collect _test-monitor-host-run-model _test-monitor-host-run-render
MONITOR_CASES := file accounting pmm procfs collect model render
_test-monitor-host-run-file:
	@echo "  [test-monitor-host] file"
	$(call os01_submake,hosttests,test-monitor-file $(OS01_SUBMAKE_ARGS))
_test-monitor-host-run-accounting:
	@echo "  [test-monitor-host] accounting"
	$(call os01_submake,hosttests,test-monitor-accounting $(OS01_SUBMAKE_ARGS))
_test-monitor-host-run-pmm:
	@echo "  [test-monitor-host] pmm"
	$(call os01_submake,hosttests,test-monitor-pmm $(OS01_SUBMAKE_ARGS))
_test-monitor-host-run-procfs:
	@echo "  [test-monitor-host] procfs"
	$(call os01_submake,hosttests,test-monitor-procfs $(OS01_SUBMAKE_ARGS))
_test-monitor-host-run-collect:
	@echo "  [test-monitor-host] collect"
	$(call os01_submake,hosttests,test-monitor-collect $(OS01_SUBMAKE_ARGS))
_test-monitor-host-run-model:
	@echo "  [test-monitor-host] model"
	$(call os01_submake,hosttests,test-monitor-model $(OS01_SUBMAKE_ARGS))
_test-monitor-host-run-render:
	@echo "  [test-monitor-host] render"
	$(call os01_submake,hosttests,test-monitor-render $(OS01_SUBMAKE_ARGS))
# Prerequisites run before recipes, even before require_capability. Only
# populate the graph for capable profiles and one exact registered CASE.
# Keep errors in the recipe so other targets can use their own CASE values.
MONITOR_CASE := $(strip $(CASE))
MONITOR_CASE_VALID := $(if $(MONITOR_CASE),$(if $(filter 1,$(words $(MONITOR_CASE))),$(filter $(MONITOR_CASES),$(MONITOR_CASE))),yes)
test-monitor-host: $(if $(and $(filter rootfs,$(PROFILE_CAPABILITIES)),$(MONITOR_CASE_VALID)),$(foreach c,$(if $(MONITOR_CASE),$(MONITOR_CASE),$(MONITOR_CASES)),_test-monitor-host-run-$(c)))
	$(call require_capability,rootfs)
	$(if $(MONITOR_CASE_VALID),,$(error unknown CASE='$(MONITOR_CASE)'; valid: $(MONITOR_CASES)))
	@echo "  [test-monitor-host] $(if $(MONITOR_CASE),CASE=$(MONITOR_CASE),full monitor group: $(MONITOR_CASES))"

# ── User runtime consumer gate (monitor plan Task 5) ─────────
# Builds the user runtime consumer (mk/components/runtime.mk: independent
# consumer=user archive + receipt) and the user link probe, then runs
# qemutests/monitor_runtime_test.py against the REAL target artifacts:
# receipt.consumer=user, kernel/user variant paths differ, the probe ELF has
# no unresolved runtime symbols (no host-symbol backfill), and unsupported
# user providers are rejected.  hosttests passing is not a substitute for
# this gate.  Separate from the test-monitor-host group on purpose: this is
# a target-artifact contract, not a host fixture.
# ── monitor thermal plan, Task 1 umbrellas ──
# Each test-thermal-host case corresponds to a per-fixture runner in
# hosttests/Makefile.  Initial Task 1 commits only register `fixup`;
# Tasks 2/3 add `affinity` and `decode` respectively.
.PHONY: test-thermal-host _test-thermal-host-run-fixup _test-thermal-host-run-affinity _test-thermal-host-run-decode
_test-thermal-host-run-fixup:
	@echo "  [test-thermal-host] fixup"
	$(call os01_submake,hosttests,test-thermal-fixup $(OS01_SUBMAKE_ARGS))

_test-thermal-host-run-affinity:
	@echo "  [test-thermal-host] affinity"
	$(call os01_submake,hosttests,test-thermal-affinity $(OS01_SUBMAKE_ARGS))

_test-thermal-host-run-decode:
	@echo "  [test-thermal-host] decode"
	$(call os01_submake,hosttests,test-thermal-decode $(OS01_SUBMAKE_ARGS))

THERMAL_CASES := fixup affinity decode
test-thermal-host: CASE ?=
test-thermal-host: CASE := $(CASE)
# Umbrella: with empty CASE, depend on every per-case runner; otherwise
# dispatch to the single-case runner.
test-thermal-host: $(if $(CASE),_test-thermal-host-run-$(CASE),$(foreach c,$(THERMAL_CASES),_test-thermal-host-run-$(c)))
	$(call require_capability,rootfs)
	@case "$(CASE)" in \
	    "") echo "  [test-thermal-host] full thermal group: $(THERMAL_CASES)";; \
	    fixup) echo "  [test-thermal-host] CASE=$(CASE)";; \
	    affinity) echo "  [test-thermal-host] CASE=$(CASE)";; \
	    decode) echo "  [test-thermal-host] CASE=$(CASE)";; \
	    *) echo "ERROR: unknown CASE='$(CASE)'; valid: $(THERMAL_CASES)" >&2; exit 1;; \
	esac

.PHONY: test-monitor-runtime
# $(KERNEL_RUNTIME_PREREQ) guarantees the kernel variant receipt exists even
# on a clean tree, so the kernel/user path-separation assertion always has
# its counterpart; the user side builds the consumer=user archive + probe.
test-monitor-runtime: $(KERNEL_RUNTIME_PREREQ) $(USER_ARTIFACTS) $(USER_RUNTIME_PROBE)
	$(call require_capability,rootfs)
	python3 qemutests/monitor_runtime_test.py \
	  --probe "$(USER_RUNTIME_PROBE)" \
	  --user-receipt "$(USER_RUNTIME_RECEIPT)" \
	  --kernel-receipt "$(KERNEL_RUNTIME_RECEIPT)" \
	  --llvm-nm "$(LLVM_NM)" \
	  --llvm-readobj "$(LLVM_READOBJ)" \
	  --profile "$(PROFILE)" \
	  --repo-root "$(OS01_ROOT)"

# driver-model matrix support — see Task 11.  The python harness
# (qemutests/driver_matrix_run.py) drives each (case, SMP) pair; the
# build contract test (qemutests/test_arch9_build_contract.py) only
# needs the Makefile as the build contract.  Both run from the repository
# root via subprocess; the recipes below are entry points.
TEST_DRIVER_MATRIX_CASES := all e1000 virtio mixed two-e1000 two-virtio no-nic no-ahci empty-ahci poll-busy bad-nic adapter-fail unsupported modern-only net-block-smp
DRIVER_MATRIX_DRIVER_CASE ?=
DRIVER_MATRIX_DRIVER_SMP ?=
DRIVER_MODEL_LOG_DIR := $(OS01_ROOT)/test-results/driver-model
.PHONY: _test-driver-model-prep _test-driver-model-run
# _test-driver-model-prep builds the canonical (non-fault) disk image
# which the matrix harness needs for all the no-fault cases.  Fault
# cases build their own variant images via `make ARCH9_FAULT=<fault>`.
_test-driver-model-prep:
	@mkdir -p $(DRIVER_MODEL_LOG_DIR)
	$(MAKE) --no-print-directory disk.img
_test-driver-model-run:
	@mkdir -p $(DRIVER_MODEL_LOG_DIR)
	DRIVER_MODEL_LOG_DIR="$(DRIVER_MODEL_LOG_DIR)" \
	  OS01_BUILD_DIR="$(abspath $(BUILD_DIR))" \
	  python3 qemutests/driver_matrix_run.py \
	  $(if $(DRIVER_MODEL_DRIVER_CASE),--case $(DRIVER_MODEL_DRIVER_CASE),--case all) \
	  $(if $(DRIVER_MATRIX_DRIVER_SMP),--smp $(DRIVER_MATRIX_DRIVER_SMP),)

# driver-model matrix dispatch target.  `make test-qemu SUITE=driver-model`
# builds the canonical image then runs every matrix case; the
# optional DRIVER_MODEL_DRIVER_CASE narrows to a single case (mirrors
# the per-MODE pattern used by test-aarch64). DRIVER_MATRIX_DRIVER_SMP
# restricts the SMP count(s) passed to the python harness (Task 11
# parked follow-up resolved in Task 12):
#   make test-qemu SUITE=driver-model DRIVER_MATRIX_DRIVER_SMP=1
#   make test-qemu SUITE=driver-model DRIVER_MATRIX_DRIVER_SMP="1 2 4"
test-qemu-driver-model: _test-driver-model-prep _test-driver-model-run
	@true

# Per-SUITE lookups, used by test-qemu to pick the right variant build
# flavor and the right image path. These are Make variables so they
# resolve at parse time and survive across recipe lines.
TEST_QEMU_FLAVOR_phase-0        =
TEST_QEMU_FLAVOR_systest        = OS01_SYSTEST=1
TEST_QEMU_FLAVOR_inittab-phase  = INITTAB_FILE=config/inittab.test
TEST_QEMU_FLAVOR_network        = OS01_NETTEST=1
TEST_QEMU_FLAVOR_gfx            =
# resolution uses the profile's active image directly: production runs the
# normal rootfs, while FB_RESOLUTION_TEST=1 (passed through from the top-level
# invocation) resolves $(DISK_IMG) to the isolated resolution-test image.
TEST_QEMU_FLAVOR_resolution     =
# driver-model reuses the normal image path (matrix harness rebuilds
# variant images itself); the build contract test and the matrix
# python harness both produce fault variant images via
# `make ARCH9_FAULT=<fault>`.
TEST_QEMU_FLAVOR_driver-model   =
# monitor (monitor plan Task 9) runs the normal image directly: the test
# helper /bin/test_monitor and /bin/monitor are both installed into the
# normal rootfs by the shared program discovery, so no variant build is
# needed.  The matrix boots the normal image read-only (-snapshot) at
# 1/2/8 cores.
TEST_QEMU_FLAVOR_monitor        =
# terminal-present (Task 6 of partial-present plan) runs the normal
# image directly: /bin/test_gfx --present-rect-surface and /bin/monitor
# are both installed into the normal rootfs by the shared program
# discovery, so no variant build is needed.  The single-suite driver
# launches its own QEMU (TCG, q35, 512 MiB, -smp 2, -snapshot, -serial
# stdio, -display none) — the parent TestRunner does NOT pre-spawn
# QEMU for this suite.
TEST_QEMU_FLAVOR_terminal-present =
# gui (GUI-1 plan Task 7) runs the normal image directly: /bin/test_lvgl
# and /bin/test_gui_fault are both installed into the normal rootfs by
# the shared program discovery, so no variant build is needed. The matrix
# boots the normal image read-only (-snapshot) at 1/2 cores with QMP.
TEST_QEMU_FLAVOR_gui            =
# Default baseline log path for the terminal-present suite; the
# task-6 brief calls this out explicitly.  Tests pass this through to
# the dispatcher as TERMINAL_PRESENT_BASELINE_LOG; an explicit
# override is allowed (e.g. for re-capturing the baseline elsewhere).
TERMINAL_PRESENT_BASELINE_LOG ?= /tmp/os01-terminal-present-baseline/serial.log
TEST_QEMU_IMG_phase-0          = $(NORMAL_IMAGE)
TEST_QEMU_IMG_systest          = $(TEST_SYSTEST_IMAGE)
TEST_QEMU_IMG_inittab-phase    = $(TEST_INITTAB_IMAGE)
TEST_QEMU_IMG_network          = $(TEST_NETTEST_IMAGE)
TEST_QEMU_IMG_gfx              = $(NORMAL_IMAGE)
TEST_QEMU_IMG_resolution       = $(DISK_IMG)
TEST_QEMU_IMG_driver-model     = $(NORMAL_IMAGE)
TEST_QEMU_IMG_monitor          = $(NORMAL_IMAGE)
TEST_QEMU_IMG_terminal-present = $(NORMAL_IMAGE)
TEST_QEMU_IMG_gui              = $(NORMAL_IMAGE)

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

SYSTEST_TIMEOUT ?= 120

test-qemu: SUITE ?= phase-0
test-qemu: SUITE := $(SUITE)
# Firmware remains a prerequisite for every suite. No image-path prereq:
# the variant image is built by the recursive make below.
test-qemu: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	@case "$(SUITE)" in \
	  phase-0|systest|inittab-phase|network|gfx|resolution|driver-model|monitor|terminal-present|gui) ;; \
	  *) echo "SUITE must be phase-0|systest|inittab-phase|network|gfx|resolution|driver-model|monitor|terminal-present|gui, got '$(SUITE)'" >&2; exit 1;; \
	esac
	@echo "  [test-qemu] SUITE=$(SUITE) flavor=$(TEST_QEMU_FLAVOR_$(SUITE)) img=$(TEST_QEMU_IMG_$(SUITE))"
	# The normal-image hash guard skips ``phase-0`` (the historical
	# no-variant path), ``gfx`` (the ring-3 graphics suite) and
	# ``resolution`` (its runner only ever copies the image into a private
	# per-run directory and asserts the source sha256 itself, so it never
	# writes the active image).  Every other suite runs against an isolated
	# variant build and must therefore not modify the normal image.
	# driver-model runs against the canonical (no-fault) image so the
	# normal-image hash guard is skipped too; fault variant builds land
	# under image/driver-model-<fault>/ (Task 11).
	# terminal-present runs against the normal image read-only (-snapshot)
	# and uses its own QEMU launch via the dispatcher; it does not
	# modify the normal image, so the hash guard is skipped too.
	# gui runs against the normal image read-only (-snapshot) across SMP=1/2.
	@if [ "$(SUITE)" != "phase-0" ] && [ "$(SUITE)" != "gfx" ] && [ "$(SUITE)" != "resolution" ] && [ "$(SUITE)" != "driver-model" ] && [ "$(SUITE)" != "monitor" ] && [ "$(SUITE)" != "terminal-present" ] && [ "$(SUITE)" != "gui" ] && [ -f "$(NORMAL_IMAGE)" ]; then \
	  sha256sum "$(NORMAL_IMAGE)" > "$(NORMAL_IMAGE_DIR)/normal.before"; \
	fi
	# Build step. THIS RECIPE LINE CONTAINS ``$(MAKE)`` AND IS DELIBERATELY
	# KEPT ON ITS OWN LOGICAL LINE: GNU Make executes any recipe line that
	# contains ``$(MAKE)`` even under ``make -n`` (so the recursive make
	# runs and gets the ``-n`` flag propagated). Keeping ``python3
	# qemutests/run_test.py`` off this line is what lets ``make -n
	# test-qemu SUITE=*`` continue to dry-run cleanly — the build
	# contract (``build_contract.sh targets``) and the
	# MakeFailureDryRunTests fixtures both rely on the dry-run printing
	# the recipe structure without actually launching QEMU. If you
	# refactor this block, keep ``$(MAKE)`` and the python invocation on
	# SEPARATE recipe lines; see commit fix/ci-test-qemu-dryrun.
	@if [ "$(SUITE)" = "driver-model" ]; then \
	  DRIVER_MODEL_DRIVER_CASE="$(DRIVER_MODEL_DRIVER_CASE)" \
	  DRIVER_MATRIX_DRIVER_SMP="$(DRIVER_MATRIX_DRIVER_SMP)" \
	  $(MAKE) --no-print-directory test-qemu-driver-model || exit 1; \
	else \
	  if [ "$(SUITE)" = "systest" ] && [ -n "$(CASE)" ]; then \
	    case "$(CASE)" in *[!A-Za-z0-9_.-]*) echo "ERROR: CASE='$(CASE)' must match [A-Za-z0-9_.-]+ (see user/systest.c --list)" >&2; exit 2;; esac; \
	    mkdir -p "$(abspath $(BUILD_DIR))/config" || exit 1; \
	    printf 'tty1:once:/bin/systest --case %s\n' "$(CASE)" > "$(abspath $(BUILD_DIR))/config/inittab.systest.case" || exit 1; \
	    $(MAKE) OS01_SYSTEST=1 INITTAB_FILE="$(abspath $(BUILD_DIR))/config/inittab.systest.case" image || exit 1; \
	  else \
	    $(MAKE) $(TEST_QEMU_FLAVOR_$(SUITE)) image || exit 1; \
	  fi; \
	fi
	# Hash guard AFTER the build (own recipe line, no ``$(MAKE)``).
	@if [ "$(SUITE)" != "phase-0" ] && [ "$(SUITE)" != "gfx" ] && [ "$(SUITE)" != "resolution" ] && [ "$(SUITE)" != "driver-model" ] && [ "$(SUITE)" != "monitor" ] && [ "$(SUITE)" != "terminal-present" ] && [ "$(SUITE)" != "gui" ] && [ -f "$(NORMAL_IMAGE_DIR)/normal.before" ]; then \
	  sha256sum "$(NORMAL_IMAGE)" > "$(NORMAL_IMAGE_DIR)/normal.after" || exit 1; \
	  cmp "$(NORMAL_IMAGE_DIR)/normal.before" "$(NORMAL_IMAGE_DIR)/normal.after" || exit 1; \
	fi
	# Run the harness (own recipe line, no ``$(MAKE)`` — therefore NOT
	# executed under ``make -n``; this is the line that broke the
	# ``build_contract.sh targets`` dry-run when it was inlined into the
	# same ``if/else/fi`` block as ``$(MAKE)``). driver-model runs the
	# python harness inside ``test-qemu-driver-model`` and must NOT
	# launch it again here.
	@if [ "$(SUITE)" != "driver-model" ]; then \
	  DISK_IMG="$(TEST_QEMU_IMG_$(SUITE))" \
	  OVMF_FIRMWARE="$(OVMF_FIRMWARE)" \
	  NETWORK_NIC="$(NETWORK_NIC)" \
	  FB_RESOLUTION_TEST="$(FB_RESOLUTION_TEST)" \
	  TERMINAL_PRESENT_BASELINE_LOG="$(TERMINAL_PRESENT_BASELINE_LOG)" \
	  OS01_BUILD_DIR="$(abspath $(BUILD_DIR))" \
	  OS01_RESOLUTION_RESULT_DIR="$(abspath $(BUILD_DIR)/test-results/resolution)" \
	  SYSTEST_CASE="$(CASE)" \
	  SYSTEST_TIMEOUT="$(SYSTEST_TIMEOUT)" \
	  python3 qemutests/run_test.py $(SUITE); \
	fi

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
	  --firmware "$(OVMF_FIRMWARE)" --qemu "$(QEMU_BIN)" --memory "$(MEMORY)" \
	  --smp "$(SMP)" --timeout 180 \
	  --build-dir "$(abspath $(BUILD_DIR))" --profile "$(PROFILE)"

# test-syscall — retained AGENTS.md-required alias exception.
#
# AGENTS.md (lines 60, 70) requires the literal invocation
#   `make OS01_SYSTEST=1 test-syscall`
# even though the canonical bucket target is `test-qemu SUITE=systest`.
# All other forwarding aliases were removed in the 2026-09-26 cleanup
# (see docs/build/build.md §alias policy); this is the single
# permitted exception. The recipe is a one-liner that delegates to
# the canonical bucket target with the same flag set so the alias
# never drifts from the underlying harness.
#
# Two parse-time gates enforce AGENTS.md's contract:
#  - OS01_SYSTEST must be 1 (systest is the only init mode supported
#    by this alias — running it with the normal inittab would not
#    load /bin/systest as PID 1).
#  - KERNEL_SELFTEST must NOT be 1 (in-kernel selftests spawn kthreads
#    at boot which interfere with systest's fork+exec+waitpid test).
.PHONY: test-syscall
ifneq ($(filter test-syscall,$(MAKECMDGOALS)),)
ifneq ($(OS01_SYSTEST),1)
$(error ERROR: test-syscall requires OS01_SYSTEST=1 on the top-level make invocation (AGENTS.md))
endif
ifeq ($(KERNEL_SELFTEST),1)
$(error ERROR: test-syscall must NOT be combined with KERNEL_SELFTEST=1 (AGENTS.md))
endif
endif
test-syscall:
	$(call require_capability,rootfs)
	$(MAKE) OS01_SYSTEST=1 test-qemu SUITE=systest

# test-static = the umbrella: runs every static audit in one shot.
# Delegates the 4 runtime-audit checks + validate-kernel to test-runtime
# (its prerequisite) so the recipe never drifts from the runtime target.
# The remaining audits (layout, canary-contract, user-canary, syscall
# boundary) are distinct in harness/prereqs and stay in this recipe.
.PHONY: test-static test-runtime test-kernel-layout test-kernel-canary-contract test-user-canary

# ── Static-audit evidence adapter (Task 12) ─────────────────────
# Every static audit below runs through qemutests/run_static_audit.py, which
# owns the process boundary (ProcessSession) and archives exactly ONE
# `audit`-unit result per executed audit command under
# build/<profile>/logs/tests/<suite>/<UTC>-<unique-id>/.  Make still prepares
# the inputs and selects the command (capability gates, prerequisites and the
# per-audit suite id live here); the adapter runs no build steps and invents
# no case records.  The audit program owns all artifact and diagnostic
# assertions: a PASS means it actually started and exited 0 (a missing
# executable, a signal, a nonzero exit or a timeout is archived as
# ERROR/FAIL/TIMEOUT).  The `<suite>` id is the audit's own name so every
# report names its real argv.
STATIC_AUDIT_TIMEOUT ?= 120
RUN_STATIC_AUDIT = python3 qemutests/run_static_audit.py --build-dir "$(BUILD_DIR)" --profile "$(PROFILE)" --timeout $(STATIC_AUDIT_TIMEOUT) --suite
# validate-kernel is not a pure-Python audit: it is a build-y sub-make
# (it may rebuild/relink the kernel), so it must NOT inherit the 120 s
# audit default — a legitimate, slow validate would be a spurious
# TIMEOUT/1.  It gets an explicit, larger budget of its own; every other
# audit keeps STATIC_AUDIT_TIMEOUT.
VALIDATE_KERNEL_AUDIT_TIMEOUT ?= 600
RUN_STATIC_AUDIT_VALIDATE = python3 qemutests/run_static_audit.py --build-dir "$(BUILD_DIR)" --profile "$(PROFILE)" --timeout $(VALIDATE_KERNEL_AUDIT_TIMEOUT) --suite

# The seven user-canary checks, verbatim from the pre-Task-12 recipe, kept as
# one shell program so every check still runs exactly as before.  The adapter
# runs it with `sh -c` (one audit command, one archived result);
# `$(subst ','\'',...)` single-quotes the program for `sh -c`, escaping the
# single quotes its `awk` programs contain.
define USER_CANARY_AUDIT_BODY
set -e; \
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
endef

# ── test-static: 5 runtime audits via test-runtime + 7 standalone + user-canary ──
test-static: test-runtime
	$(call require_capability,rootfs)
	$(RUN_STATIC_AUDIT) syscall-boundary -- \
	  python3 qemutests/syscall_boundary_audit.py
	$(RUN_STATIC_AUDIT) kernel-layout -- \
	  python3 qemutests/x86_64_kernel_layout_test.py "$(KERNEL_BUILD_DIR)/kernel.elf" \
	  --llvm-nm "$(LLVM_NM)" --llvm-readelf "$(LLVM_READELF)"
	$(RUN_STATIC_AUDIT) kernel-canary-contract -- \
	  python3 qemutests/kernel_canary_contract_test.py
	$(RUN_STATIC_AUDIT) header-object-audit -- \
	  python3 qemutests/header_object_audit.py \
	  --include-dir "kernel/include" \
	  --sysroot "$(SYSROOT)" \
	  --runtime-inc "runtime/include" \
	  --llvm-nm "$(LLVM_NM)" \
	  --clang "$(CLANG)"
	$(RUN_STATIC_AUDIT) stack-frame-audit -- \
	  python3 qemutests/stack_frame_audit.py \
	  --elf "$(KERNEL_BUILD_DIR)/kernel.elf" \
	  --llvm-objdump "$(LLVM_OBJDUMP)" \
	  --limit 512
	$(RUN_STATIC_AUDIT) driver-model-boundary -- \
	  python3 qemutests/driver_model_boundary_audit.py
	$(RUN_STATIC_AUDIT) gui-build-audit -- \
	  python3 qemutests/gui_build_audit.py \
	    --sysroot "$(SYSROOT)" \
	    --build-dir "$(BUILD_DIR)" \
	    --repo-root "$(OS01_ROOT)" \
	    --profile "$(PROFILE)"
	$(RUN_STATIC_AUDIT) intr-dispatch-audit -- \
	  python3 qemutests/intr_dispatch_audit.py \
	    --profile "$(PROFILE)" \
	    --elf "$(KERNEL_BUILD_DIR)/kernel.elf" \
	    --llvm-nm "$(LLVM_NM)" \
	    --llvm-objdump "$(LLVM_OBJDUMP)" \
	    --repo-root "$(OS01_ROOT)"
	@$(MAKE) --no-print-directory test-user-canary

# ── test-gui-incremental: the GUI library's incremental contract ──
#
# The INCREMENTAL modes of the audit never spawn make: this recipe owns
# every build step and the audit only records and compares snapshots
# (--incremental-snapshot / --incremental-compare).  The audit's --sysroot
# mode does spawn make exactly once, read-only, to read the profile's
# build variables (make -f …/user_build_probe.mk --no-print-directory
# gui-audit-profile-vars).
#
# The four phases mirror the four assertions:
#
#   noop    a repeated build with no source change must not recompile
#   touch   touching gui_input.h must rebuild the dependents and relink
#   install install must clear orphaned files under staging/gui
#   publish a new generation must be clean, the old ones preserved
#
# It is deliberately NOT part of test-static: the publish phase allocates a
# new sysroot generation, so running it inside a static-audit bucket would
# contend with the publisher's lock.
GUI_INCREMENTAL_AUDIT = python3 qemutests/gui_build_audit.py \
	--build-dir "$(BUILD_DIR)" --repo-root "$(OS01_ROOT)" --profile "$(PROFILE)"

.PHONY: test-gui-incremental
test-gui-incremental:
	$(call require_capability,rootfs)
	@echo "  [test-gui-incremental] phase 1/4: a repeated build must not recompile"
	$(call os01_submake,libgui,all)
	$(GUI_INCREMENTAL_AUDIT) --incremental-snapshot "$(BUILD_DIR)/incr-noop.json" --incremental-phase noop
	$(call os01_submake,libgui,all)
	$(GUI_INCREMENTAL_AUDIT) --incremental-compare "$(BUILD_DIR)/incr-noop.json" --incremental-phase noop
	@echo "  [test-gui-incremental] phase 2/4: touching gui_input.h must rebuild the dependents"
	@stat -c %y libgui/include/gui/gui_input.h > $(BUILD_DIR)/incr-header-time
	@$(call os01_submake,libgui,--no-print-directory print-compile) > $(BUILD_DIR)/incr-libgui-compile.txt
	$(GUI_INCREMENTAL_AUDIT) --incremental-snapshot "$(BUILD_DIR)/incr-touch.json" --incremental-phase touch --libgui-compile "$(BUILD_DIR)/incr-libgui-compile.txt"
	@touch libgui/include/gui/gui_input.h
	$(call os01_submake,libgui,all)
	$(MAKE) --no-print-directory test-gui-host CASE=input-decode
	@touch -d "$$(cat $(BUILD_DIR)/incr-header-time)" libgui/include/gui/gui_input.h
	$(GUI_INCREMENTAL_AUDIT) --incremental-compare "$(BUILD_DIR)/incr-touch.json" --incremental-phase touch --libgui-compile "$(BUILD_DIR)/incr-libgui-compile.txt"
	@echo "  [test-gui-incremental] phase 3/4: install must clear orphaned staging files"
	@mkdir -p $(STAGING_DIR)/gui/usr/include/gui
	@printf '/* orphaned staging file */\n' > $(STAGING_DIR)/gui/usr/include/gui/gui_input_private.h
	$(GUI_INCREMENTAL_AUDIT) --incremental-snapshot "$(BUILD_DIR)/incr-install.json" --incremental-phase install
	$(call os01_submake,libgui,install INSTALL_ROOT=$(STAGING_DIR)/gui)
	$(GUI_INCREMENTAL_AUDIT) --incremental-compare "$(BUILD_DIR)/incr-install.json" --incremental-phase install
	@echo "  [test-gui-incremental] phase 4/4: a new generation must be clean, the old ones preserved"
	$(GUI_INCREMENTAL_AUDIT) --incremental-snapshot "$(BUILD_DIR)/incr-publish.json" --incremental-phase publish
	@rm -f $(BUILD_DIR)/stamps/sysroot.stamp
	$(MAKE) --no-print-directory sysroot
	$(GUI_INCREMENTAL_AUDIT) --incremental-compare "$(BUILD_DIR)/incr-publish.json" --incremental-phase publish
	@echo "  [test-gui-incremental] all four incremental assertions PASS"

# ── test-runtime: original recipe (lines 364-385 of run.mk) ──
test-runtime: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(KERNEL_ARTIFACT))
	$(call require_capability,rootfs)
	$(RUN_STATIC_AUDIT) runtime-audit -- \
	  python3 qemutests/runtime_audit.py \
	  --stage1 "$(KERNEL_BUILD_DIR)/kernel.elf.stage1" \
	  --final "$(KERNEL_ELF)" \
	  --link-receipt "$(KERNEL_RUNTIME_LINK_RECEIPT)" \
	  --runtime-input "$(KERNEL_RUNTIME_INPUTS)" \
	  --llvm-nm "$(LLVM_NM)" \
	  --llvm-readobj "$(LLVM_READOBJ)"
	$(RUN_STATIC_AUDIT) stack-canary-audit -- \
	  python3 qemutests/stack_canary_audit.py \
	  --object "$(KERNEL_BUILD_DIR)/sched/core.o" \
	  --elf "$(KERNEL_ELF)" \
	  --llvm-readelf "$(LLVM_READELF)" \
	  --llvm-objdump "$(LLVM_OBJDUMP)"
	$(RUN_STATIC_AUDIT_VALIDATE) validate-kernel -- \
	  $(MAKE) --no-print-directory validate-kernel
	$(RUN_STATIC_AUDIT) runtime-link-order -- \
	  python3 qemutests/runtime_link_order_test.py
	$(RUN_STATIC_AUDIT) kernel-runtime-link -- \
	  python3 qemutests/kernel_runtime_link_test.py \
	  --source-receipt "$(KERNEL_RUNTIME_LINK_RECEIPT)" \
	  --sysroot "$(SYSROOT)" \
	  --profile-file "$(OS01_PROFILE_FILE)" \
	  --profile "$(PROFILE)"

# ── test-kernel-layout: original recipe (line 485-488) ──────
test-kernel-layout: kernel.bin
	$(RUN_STATIC_AUDIT) kernel-layout -- \
	  python3 qemutests/x86_64_kernel_layout_test.py "$(KERNEL_BUILD_DIR)/kernel.elf" \
	  --llvm-nm "$(LLVM_NM)" --llvm-readelf "$(LLVM_READELF)"

# ── test-kernel-canary-contract: original recipe (line 490-492) ─
test-kernel-canary-contract:
	$(RUN_STATIC_AUDIT) kernel-canary-contract -- \
	  python3 qemutests/kernel_canary_contract_test.py

# ── test-user-canary: original recipe (lines 393-431) ───────
test-user-canary: $(if $(filter userland,$(PROFILE_CAPABILITIES)),$(USER_ARTIFACTS) $(USER_ARTIFACT_DIR)/busybox.elf $(ROOTFS_MANIFEST))
	$(call require_capability,rootfs)
	@$(RUN_STATIC_AUDIT) user-canary -- \
	  sh -c '$(subst ','\'',$(USER_CANARY_AUDIT_BODY))'

# This target builds and boots only the selftest-scoped image.  In particular
# it never uses the ordinary image, and it refuses a combined syscall/selftest
# request because those suites are intentionally run independently.
#
# The QEMU process boundary, the semantic gate (check_kernel_selftest.failures)
# and the run evidence now live in qemutests/run_kernel_selftest.py (Task 6).
# The runner receives explicit values — it does NOT infer profile paths — and
# stops QEMU controlled once the boot summary and scheduled task markers are
# complete, so the historical external `timeout 75` wrapper is gone.
.PHONY: test-kernel-selftest
test-kernel-selftest: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	@if [ "$(OS01_SYSTEST)" = "1" ]; then \
	  echo "ERROR: test-kernel-selftest must not be combined with OS01_SYSTEST=1; run make OS01_SYSTEST=1 test-qemu SUITE=systest separately" >&2; \
	  exit 1; \
	fi
	$(MAKE) KERNEL_SELFTEST=1 image
	python3 qemutests/run_kernel_selftest.py \
	  --firmware "$(OVMF_FIRMWARE)" \
	  --image "$(TEST_SELFTEST_IMAGE)" \
	  --qemu "$(QEMU_BIN)" \
	  --cpu "$(KERNEL_SELFTEST_SMP)" \
	  --memory "$(MEMORY)" \
	  --timeout 75 \
	  --build-dir "$(abspath $(BUILD_DIR))" \
	  --profile "$(PROFILE)"

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
		 'run'               '(rootfs)'     'QEMU q35 + e1000 + serial stdio';
	@printf '  %-22s %-13s %s\n' \
		 'run-kvm'           '(rootfs)'     'QEMU with KVM acceleration';
	@printf '  %-22s %-13s %s\n' \
		 'run-virtio'        '(rootfs)'     'QEMU with virtio-net-pci (instead of e1000)';
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
		 'test-qemu'           '(rootfs)'     'QEMU E2E suite (SUITE=<phase-0|systest|inittab-phase|network|gfx|resolution|driver-model|monitor|terminal-present|gui>, CASE=<id> selects one systest case)';
	@printf '  %-22s %-13s %s\n' \
		 'test-host'           '(rootfs)'     'os01_submake hosttests + pmm_boot_reservation_test.py';
	@printf '  %-22s %-13s %s\n' \
		 'test-static'         '(rootfs)'     'All 13 static audits (runtime, stack-canary, validate-kernel, runtime-link-order, kernel-runtime-link, syscall-boundary, kernel-layout, kernel-canary-contract, header-object, stack-frame, driver-model-boundary, gui-build, user-canary)';
	@printf '  %-22s %-13s %s\n' \
		 'test-gui-incremental' '(rootfs)'    'GUI library incremental contract (no-op rebuild, header-touch rebuild, staging orphan, generation publish)';
	@printf '  %-22s %-13s %s\n' \
		 'test-kernel-selftest' '(rootfs)'   'QEMU built-in selftests (isolated selftest image, KERNEL_SELFTEST=1)';
	@printf '  %-22s %-13s %s\n' \
		 'test-aarch64'        '(uefi)'       'aarch64 UEFI test (MODE=<smp|no-ack|gic-spi|sync-fault|m3-probe|m1-*>)';
	@printf '  %-22s %-13s %s\n' \
		 'test-contract'       '(rootfs|uefi)' 'Full build contract (PROFILE=<x86_64-clang|aarch64-clang>)';
	@echo ''
	@echo 'Framework regression (no QEMU; always):'
	@printf '  %-22s %-13s %s\n' \
		 'test-harness'         '(always)'   'Python unittest framework regression (TEST_HARNESS_MODULES list — append-only across tasks; never discovers QEMU scripts); also runs test-framework-contract as a prereq';
	@printf '  %-22s %-13s %s\n' \
		 'test-framework-contract' '(always)' 'Hostside negative contract: builds test_framework_contract.elf and asserts exit code 1 (assertion exit-code propagation / single-evaluation / TEST_RESULTS snapshot)';
	@echo ''
	@echo 'Standalone test targets (distinct harness / image variant):'
	@printf '  %-22s %-13s %s\n' \
		 'test-syscall'          '(rootfs)'   'RETAINED ALIAS — see docs/build/build.md §4 (AGENTS.md exact-target requirement; use OS01_SYSTEST=1 test-qemu SUITE=systest in new code)';
	@printf '  %-22s %-13s %s\n' \
		 'test-syscall-repeat'   '(rootfs)'   'QEMU exec/exit stability through normal terminal (x86_64_systest_repeat.py)';
	@printf '  %-22s %-13s %s\n' \
		 'test-user-canary'      '(rootfs)'   '7-step SSP / crt0 user-stack canary audit (spec 2026-09-17)';
	@printf '  %-22s %-13s %s\n' \
		 'test-pmm-boot-reservation' '(rootfs)' 'PMM boot-time memory reservation guard (host-only)';
	@echo ''
	@echo 'Focused compatibility checks (retained; see docs/build/build.md §3 alias policy):'
	@printf '  %-22s %-13s %s\n' \
		 'test-kernel-layout'    '(rootfs)'   'x86_64 kernel.elf layout audit (post-_end reserved)';
	@printf '  %-22s %-13s %s\n' \
		 'test-kernel-canary-contract' '(rootfs)' 'Kernel canary compile-flag contract';
	@printf '  %-22s %-13s %s\n' \
		 'test-resolution-host'  '(rootfs)'   'Resolution switcher host tests (RES_CASE=<uapi|...|all>)';
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
	@echo 'See AGENTS.md Quick start and docs/build/build.md for recipes.'
	@echo 'See docs/build/build.md §3 alias policy for the retained focused checks (test-runtime, test-kernel-layout, test-kernel-canary-contract, test-user-canary, test-pmm-boot-reservation).'

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
X86_CONTRACT_MODES := legacy-components legacy x86 sysroot firmware targets sysroot-headers flags-cache host-test
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
	# arch9-build-contract runs FIRST: it dry-runs ``make -n
	# ARCH9_FAULT=<fault> kernel.bin``, whose ``$(KERNEL_ARTIFACT)``
	# recipe carries an ``@+$(SHELL) -ec '...'`` line that forces
	# execution under ``-n`` (so it sees the real publish-lock /
	# sysroot-symlink state). The ``host-test`` mode at the END of
	# ``X86_CONTRACT_MODES`` does ``make clean`` (deletes the sysroot
	# symlink); running arch9 BEFORE the modes loop sees a still-
	# intact build tree. (Earlier ordering masked this: the
	# ``targets`` mode used to fail first with ``OVMF_FIRMWARE must
	# name a readable firmware file`` and short-circuited the loop
	# before ``host-test`` could run.)
	if [ "$(PROFILE)" = "x86_64-clang" ]; then \
	  echo "  [test-contract] $(PROFILE)/arch9-build-contract"; \
	  PROFILE=$(PROFILE) python3 -m unittest qemutests.test_arch9_build_contract; \
	fi; \
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

# ARCH-9 driver model matrix fault-fixture cleanup (Task 11).
# Removes ONLY the derived kernel/image artifacts for the given fault
# (kernel/driver-model-<fault>/ + artifacts/kernel/driver-model-<fault>/
# + image/driver-model-<fault>/); never touches libc, user, firmware,
# or normal artifacts.  Used between fault runs so the matrix harness
# can switch faults without `make clean` of the whole profile.
.PHONY: clean-arch9-fixture
# Parse-time validation: ARCH9_FAULT must be a known non-none slug.
# project.mk's enum gate covers all values; this target refuses empty,
# "none", or any non-driver-model variant (defence in depth).
ifneq ($(filter $(ARCH9_FAULT),none),)
clean-arch9-fixture:
	@echo "ERROR: clean-arch9-fixture requires ARCH9_FAULT=<non-none slug>" >&2; \
	exit 1
else ifeq ($(filter $(ARCH9_FAULT),observe bad-nic-bar adapter-fail ahci-empty irq-conflict),)
clean-arch9-fixture:
	@echo "ERROR: ARCH9_FAULT='$(ARCH9_FAULT)' is not a known driver-model fault slug" >&2; \
	exit 1
else
clean-arch9-fixture:
	@echo "  [clean-arch9-fixture] ARCH9_FAULT=$(ARCH9_FAULT)"
	rm -rf "$(BUILD_DIR)/kernel/driver-model-$(ARCH9_FAULT)"
	rm -rf "$(BUILD_DIR)/artifacts/kernel/driver-model-$(ARCH9_FAULT)"
	rm -rf "$(BUILD_DIR)/image/driver-model-$(ARCH9_FAULT)"
endif

# M1 uses distinct immutable build/image variants; outer flags never choose
# the matrix's normal/selftest artifact implicitly.
.PHONY: _test-aarch64-prep-m1-ram _test-aarch64-run-m1-ram
_test-aarch64-prep-m1-ram:
	$(MAKE) KERNEL_SELFTEST= AARCH64_M1_TEST= AARCH64_SYNC_FAULT_TEST= AARCH64_SMP_TEST_NO_ACK_CPU=0 aarch64-uefi
	$(MAKE) KERNEL_SELFTEST=1 AARCH64_M1_TEST= AARCH64_SYNC_FAULT_TEST= AARCH64_SMP_TEST_NO_ACK_CPU=0 aarch64-uefi
_test-aarch64-run-m1-ram:
	python3 qemutests/aarch64_m1_ap_tlbi.py --self-test \
	  --llvm-objdump "$(or $(LLVM_OBJDUMP),llvm-objdump)" \
	  --elf "$(BUILD_DIR)/kernel/kernel.elf" --elf "$(BUILD_DIR)/kernel/selftest/kernel.elf"
	python3 qemutests/aarch64_m1_matrix.py \
	  --normal-image "$(BUILD_DIR)/image/aarch64-uefi.img" \
	  --selftest-image "$(AARCH64_UEFI_SELFTEST_DISK)" \
	  --firmware "$(AARCH64_UEFI_SELFTEST_FIRMWARE)" --qemu "$(AARCH64_QEMU)" \
	  --build-dir "$(BUILD_DIR)" --profile "$(PROFILE)" \
	  --log-dir "$(OS01_ROOT)/test-results/m1-ram"

.PHONY: _test-aarch64-prep-m1-sparse _test-aarch64-run-m1-sparse
_test-aarch64-prep-m1-sparse:
	$(MAKE) KERNEL_SELFTEST=1 AARCH64_M1_TEST=sparse AARCH64_SYNC_FAULT_TEST= AARCH64_SMP_TEST_NO_ACK_CPU=0 aarch64-uefi
_test-aarch64-run-m1-sparse:
	python3 qemutests/aarch64_m1_matrix.py --variant sparse \
	  --image "$(BUILD_DIR)/image/m1-sparse/aarch64-uefi.img" \
	  --firmware "$(BUILD_DIR)/image/m1-sparse/QEMU_EFI.fd" --qemu "$(AARCH64_QEMU)" \
	  --build-dir "$(BUILD_DIR)" --profile "$(PROFILE)" \
	  --log-dir "$(OS01_ROOT)/test-results/m1-sparse"

.PHONY: _test-aarch64-prep-m1-arena-exhaust _test-aarch64-run-m1-arena-exhaust
_test-aarch64-prep-m1-arena-exhaust:
	$(MAKE) KERNEL_SELFTEST=1 AARCH64_M1_TEST=arena-exhaust AARCH64_SYNC_FAULT_TEST= AARCH64_SMP_TEST_NO_ACK_CPU=0 aarch64-uefi
_test-aarch64-run-m1-arena-exhaust:
	python3 qemutests/aarch64_m1_matrix.py --variant arena-exhaust \
	  --image "$(BUILD_DIR)/image/m1-arena-exhaust/aarch64-uefi.img" \
	  --firmware "$(BUILD_DIR)/image/m1-arena-exhaust/QEMU_EFI.fd" --qemu "$(AARCH64_QEMU)" \
	  --build-dir "$(BUILD_DIR)" --profile "$(PROFILE)" \
	  --log-dir "$(OS01_ROOT)/test-results/m1-arena-exhaust"

.PHONY: _test-aarch64-prep-m1-table-exhaust _test-aarch64-run-m1-table-exhaust
_test-aarch64-prep-m1-table-exhaust:
	$(MAKE) KERNEL_SELFTEST=1 AARCH64_M1_TEST=table-exhaust AARCH64_SYNC_FAULT_TEST= AARCH64_SMP_TEST_NO_ACK_CPU=0 aarch64-uefi
_test-aarch64-run-m1-table-exhaust:
	python3 qemutests/aarch64_m1_matrix.py --variant table-exhaust \
	  --image "$(BUILD_DIR)/image/m1-table-exhaust/aarch64-uefi.img" \
	  --firmware "$(BUILD_DIR)/image/m1-table-exhaust/QEMU_EFI.fd" --qemu "$(AARCH64_QEMU)" \
	  --build-dir "$(BUILD_DIR)" --profile "$(PROFILE)" \
	  --log-dir "$(OS01_ROOT)/test-results/m1-table-exhaust"

.PHONY: _test-aarch64-prep-m1-ap-bad-root _test-aarch64-run-m1-ap-bad-root
_test-aarch64-prep-m1-ap-bad-root:
	$(MAKE) KERNEL_SELFTEST=1 AARCH64_M1_TEST=ap-bad-root AARCH64_SYNC_FAULT_TEST= AARCH64_SMP_TEST_NO_ACK_CPU=0 aarch64-uefi
_test-aarch64-run-m1-ap-bad-root:
	python3 qemutests/aarch64_m1_matrix.py --variant ap-bad-root \
	  --image "$(BUILD_DIR)/image/m1-ap-bad-root/aarch64-uefi.img" \
	  --firmware "$(BUILD_DIR)/image/m1-ap-bad-root/QEMU_EFI.fd" --qemu "$(AARCH64_QEMU)" \
	  --build-dir "$(BUILD_DIR)" --profile "$(PROFILE)" \
	  --log-dir "$(OS01_ROOT)/test-results/m1-ap-bad-root"
