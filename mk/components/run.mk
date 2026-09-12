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
.PHONY: run
run: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(NORMAL_IMAGE) $(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	$(QEMU_BIN) -M q35 -smp $(SMP) \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_FIRMWARE) \
	  -netdev user,id=net0 -device e1000e,netdev=net0 \
	  -drive file=$(NORMAL_IMAGE),format=raw,if=none,id=disk \
	  -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
	  -m $(MEMORY) -display $(DISPLAY) -serial stdio -no-reboot

.PHONY: run-kvm
run-kvm: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(NORMAL_IMAGE) $(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	$(QEMU_BIN) -M q35 -smp $(SMP) \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_FIRMWARE) \
	  -accel kvm \
	  -netdev user,id=net0 -device e1000e,netdev=net0 \
	  -drive file=$(NORMAL_IMAGE),format=raw,if=none,id=disk \
	  -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
	  -m $(MEMORY) -display $(DISPLAY) -serial stdio -no-reboot

.PHONY: run-virtio
run-virtio: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(NORMAL_IMAGE) $(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	$(QEMU_BIN) -M q35 -smp $(SMP) \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_FIRMWARE) \
	  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
	  -drive file=$(NORMAL_IMAGE),format=raw,if=none,id=disk \
	  -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
	  -m $(MEMORY) -display $(DISPLAY) -serial stdio

.PHONY: debug
debug: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(NORMAL_IMAGE) $(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	$(QEMU_BIN) -M q35 -smp $(SMP) \
	  -drive if=pflash,format=raw,readonly=on,file=$(OVMF_FIRMWARE) \
	  -S -s \
	  -netdev user,id=net0 -device e1000e,netdev=net0 \
	  -drive file=$(NORMAL_IMAGE),format=raw,if=none,id=disk \
	  -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
	  -m $(MEMORY) -display $(DISPLAY) -serial stdio

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
.PHONY: test-aarch64-uefi-smp
test-aarch64-uefi-smp:
	$(call require_aarch64_uefi)
	$(call require_capability,uefi)
	$(MAKE) KERNEL_SELFTEST=1 aarch64-uefi
	python3 tests/aarch64_uefi_smp.py \
	  --cpus 1 2 4 --repeat 3 --timeout 90 --expect-selftest \
	  $(if $(filter 0,$(AARCH64_UEFI_SMP_DIAGNOSTIC_DTB)),,--diagnostic-dtb=auto) \
	  --firmware "$(AARCH64_UEFI_FIRMWARE)" \
	  --image "$(AARCH64_UEFI_DISK)" \
	  --qemu "$(AARCH64_QEMU)" \
	  --log-dir "$(OS01_ROOT)/test-results/aarch64-uefi-smp/$$(date -u +%Y%m%dT%H%M%S)-normal-$$$$"

# This target only consumes an already injected image. Switching the
# compiled value requires the following profile-clean sequence:
# make PROFILE=aarch64-clang clean
# make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 aarch64-uefi
# make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=1 test-aarch64-uefi-smp-no-ack
# make PROFILE=aarch64-clang clean
# make PROFILE=aarch64-clang AARCH64_SMP_TEST_NO_ACK_CPU=0 aarch64-uefi
# Then run a normal two-core recovery case with the rebuilt image paths.
.PHONY: test-aarch64-uefi-smp-no-ack
test-aarch64-uefi-smp-no-ack:
	$(call require_aarch64_uefi)
	$(call require_capability,uefi)
	$(if $(and $(filter 1,$(words $(AARCH64_SMP_TEST_NO_ACK_CPU))),$(filter 1,$(AARCH64_SMP_TEST_NO_ACK_CPU))),,$(error AARCH64_SMP_TEST_NO_ACK_CPU must be 1; clean and build the injected image first))
	@test "$(AARCH64_SMP_TEST_NO_ACK_CPU)" = 1
	@test -f "$(AARCH64_UEFI_DISK)" -a -f "$(AARCH64_UEFI_FIRMWARE)" || { echo 'Build the injected aarch64-uefi image first' >&2; exit 1; }
	python3 tests/aarch64_uefi_smp.py \
	  --cpus 2 --repeat 1 --timeout 90 --expect-no-ack 1 \
	  $(if $(filter 0,$(AARCH64_UEFI_SMP_DIAGNOSTIC_DTB)),,--diagnostic-dtb=auto) \
	  --firmware "$(AARCH64_UEFI_FIRMWARE)" \
	  --image "$(AARCH64_UEFI_DISK)" \
	  --qemu "$(AARCH64_QEMU)" \
	  --log-dir "$(OS01_ROOT)/test-results/aarch64-uefi-smp/$$(date -u +%Y%m%dT%H%M%S)-no-ack-$$$$"

# ── Validation ─────────────────────────────────────────────
# validate keeps the x86 kernel + UEFI artifact checks (kernel ELF has no
# undefined symbols / INTERP / DYNAMIC, is EM_X86_64, exports _start /
# kernel_main / _text; the EFI app has a parseable COFF export table) and
# prints the selected profile's identity. x86-only: gated on the `uefi`
# capability (x86_64-clang has it; aarch64-clang has uefi) so an
# incapable profile gets the clean capability error instead of cryptic
# empty-LLVM_* failures.
.PHONY: validate validate-kernel validate-uefi validate-profile
validate: $(if $(filter uefi,$(PROFILE_CAPABILITIES)),validate-kernel validate-uefi validate-profile)
	$(call require_capability,uefi)
validate-kernel: $(if $(filter uefi,$(PROFILE_CAPABILITIES)),kernel.bin)
	$(call require_capability,uefi)
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
validate-uefi: $(if $(filter uefi,$(PROFILE_CAPABILITIES)),$(UEFI_EFI))
	$(call require_capability,uefi)
	@echo "  [validate] $(notdir $(UEFI_EFI)) coff-exports"
	@$(LLVM_READOBJ) --coff-exports $(UEFI_EFI) >/dev/null
validate-profile:
	@printf 'profile=%s triple=%s sysroot=%s capabilities=%s\n' "$(PROFILE)" "$(TARGET_TRIPLE)" "$(SYSROOT)" "$(PROFILE_CAPABILITIES)"

# ── Test ────────────────────────────────────────────────────
# Each x86 E2E test builds its image VARIANT in an isolated dir
# (build/<profile>/image/<variant>/disk.img) and runs tests/run_test.py
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

.PHONY: test
test:
	$(call require_capability,rootfs)
	@$(call os01_submake,test,run $(OS01_SUBMAKE_ARGS))

.PHONY: test-phase-0
test-phase-0: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(NORMAL_IMAGE) $(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	OVMF_FIRMWARE="$(OVMF_FIRMWARE)" python3 tests/run_test.py phase-0 --disk $(NORMAL_IMAGE)

.PHONY: test-syscall
# KERNEL_SELFTEST starts kernel threads at boot and can perturb the syscall
# suite's fork/exec/wait sequencing.  Reject the combination while Make is
# parsing the requested goals, before firmware/image prerequisites or the
# normal-image hash sandwich can run.
ifneq ($(filter test-syscall,$(MAKECMDGOALS)),)
ifeq ($(filter 1,$(KERNEL_SELFTEST)),1)
$(error ERROR: test-syscall must not be combined with KERNEL_SELFTEST=1; run test-kernel-selftest separately)
endif
endif
test-syscall: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	@set -e; \
	if [ -f "$(NORMAL_IMAGE)" ]; then \
	  sha256sum "$(NORMAL_IMAGE)" > "$(NORMAL_IMAGE_DIR)/normal.before"; \
	fi
	$(MAKE) OS01_SYSTEST=1 image
	@set -e; \
	if [ -f "$(NORMAL_IMAGE_DIR)/normal.before" ]; then \
	  sha256sum "$(NORMAL_IMAGE)" > "$(NORMAL_IMAGE_DIR)/normal.after"; \
	  cmp "$(NORMAL_IMAGE_DIR)/normal.before" "$(NORMAL_IMAGE_DIR)/normal.after"; \
	fi
	DISK_IMG="$(TEST_SYSTEST_IMAGE)" OVMF_FIRMWARE="$(OVMF_FIRMWARE)" python3 tests/run_test.py systest

.PHONY: test-inittab
test-inittab: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	@set -e; \
	if [ -f "$(NORMAL_IMAGE)" ]; then \
	  sha256sum "$(NORMAL_IMAGE)" > "$(NORMAL_IMAGE_DIR)/normal.before"; \
	fi
	$(MAKE) INITTAB_FILE=config/inittab.test image
	@set -e; \
	if [ -f "$(NORMAL_IMAGE_DIR)/normal.before" ]; then \
	  sha256sum "$(NORMAL_IMAGE)" > "$(NORMAL_IMAGE_DIR)/normal.after"; \
	  cmp "$(NORMAL_IMAGE_DIR)/normal.before" "$(NORMAL_IMAGE_DIR)/normal.after"; \
	fi
	DISK_IMG="$(TEST_INITTAB_IMAGE)" OVMF_FIRMWARE="$(OVMF_FIRMWARE)" python3 tests/run_test.py inittab-phase

.PHONY: test-network
test-network: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	@set -e; \
	if [ -f "$(NORMAL_IMAGE)" ]; then \
	  sha256sum "$(NORMAL_IMAGE)" > "$(NORMAL_IMAGE_DIR)/normal.before"; \
	fi
	$(MAKE) OS01_NETTEST=1 image
	@set -e; \
	if [ -f "$(NORMAL_IMAGE_DIR)/normal.before" ]; then \
	  sha256sum "$(NORMAL_IMAGE)" > "$(NORMAL_IMAGE_DIR)/normal.after"; \
	  cmp "$(NORMAL_IMAGE_DIR)/normal.before" "$(NORMAL_IMAGE_DIR)/normal.after"; \
	fi
	DISK_IMG="$(TEST_NETTEST_IMAGE)" OVMF_FIRMWARE="$(OVMF_FIRMWARE)" python3 tests/run_test.py network

# Runtime validation is deliberately rooted in a real, profile-resolved
# kernel artifact.  The host-suite's link-order fixture is supplementary: it
# must never substitute for auditing the two links that produced kernel.elf.
.PHONY: test-runtime
test-runtime: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(KERNEL_ARTIFACT))
	$(call require_capability,rootfs)
	python3 tests/runtime_audit.py \
	  --stage1 "$(KERNEL_BUILD_DIR)/kernel.elf.stage1" \
	  --final "$(KERNEL_ELF)" \
	  --link-receipt "$(KERNEL_RUNTIME_LINK_RECEIPT)" \
	  --runtime-input "$(KERNEL_RUNTIME_INPUTS)" \
	  --llvm-nm "$(LLVM_NM)" \
	  --llvm-readobj "$(LLVM_READOBJ)"
	python3 tests/stack_canary_audit.py \
	  --object "$(KERNEL_BUILD_DIR)/sched/task.o" \
	  --elf "$(KERNEL_ELF)" \
	  --llvm-readelf "$(LLVM_READELF)" \
	  --llvm-objdump "$(LLVM_OBJDUMP)"
	@$(MAKE) validate-kernel
	python3 tests/runtime_link_order_test.py
	python3 tests/kernel_runtime_link_test.py \
	  --source-receipt "$(KERNEL_RUNTIME_LINK_RECEIPT)" \
	  --sysroot "$(SYSROOT)" \
	  --profile-file "$(OS01_PROFILE_FILE)" \
	  --profile "$(PROFILE)"

# This target builds and boots only the selftest-scoped image.  In particular
# it never uses the ordinary image, and it refuses a combined syscall/selftest
# request because those suites are intentionally run independently.
.PHONY: test-kernel-selftest
test-kernel-selftest: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	@if [ "$(OS01_SYSTEST)" = "1" ]; then \
	  echo "ERROR: test-kernel-selftest must not be combined with OS01_SYSTEST=1; run test-syscall separately" >&2; \
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

# Verify the actual linked x86 image before PMM can reuse memory at _end.
.PHONY: test-kernel-layout
test-kernel-layout: kernel.bin
	python3 tests/x86_64_kernel_layout_test.py "$(KERNEL_BUILD_DIR)/kernel.elf" \
	  --llvm-nm "$(LLVM_NM)" --llvm-readelf "$(LLVM_READELF)"

.PHONY: test-kernel-canary-contract
test-kernel-canary-contract:
	python3 tests/kernel_canary_contract_test.py

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
		 'all / disk.img'    '(rootfs)'     'Build the normal disk image (default goal)';
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
	@printf '  %-22s %-13s %s\n' \
		 'print-run-paths'   '(rootfs)'     'Print absolute firmware + image paths';
	@echo ''
	@echo 'aarch64 UEFI bring-up (uefi):'
	@printf '  %-22s %-13s %s\n' \
		 'aarch64-uefi'           '(uefi)' 'Build aarch64 disk + firmware';
	@printf '  %-22s %-13s %s\n' \
		 'aarch64-uefi-kernel'    '(uefi)' 'Build aarch64 kernel.elf only';
	@printf '  %-22s %-13s %s\n' \
		 'run-aarch64-uefi'       '(uefi)' 'QEMU virt + cortex-a53 + virtio-blk (override SMP with SMP=N; AARCH64_UEFI_SMP_DIAGNOSTIC_DTB=0 to skip auto DTB)';
	@printf '  %-22s %-13s %s\n' \
		 'test-aarch64-uefi-smp'  '(uefi)' 'QEMU 1/2/4 PSCI SMP ×3 with PASS/DEGRADED evidence';
	@printf '  %-22s %-13s %s\n' \
		 'test-aarch64-uefi-smp-no-ack' '(uefi)' 'Consumes prebuilt AARCH64_SMP_TEST_NO_ACK_CPU=1 image for DEGRADED recovery';
	@echo ''
	@echo 'Validation (x86 uefi):'
	@printf '  %-22s %-13s %s\n' \
		 'validate'          '(rootfs)'     'kernel ELF + UEFI COFF + profile info';
	@printf '  %-22s %-13s %s\n' \
		 'validate-kernel'   '(rootfs)'     'kernel ELF sanity (no undef, EM_X86_64, exports)';
	@printf '  %-22s %-13s %s\n' \
		 'validate-uefi'     '(rootfs)'     'EFI app COFF exports parseable';
	@printf '  %-22s %-13s %s\n' \
		 'validate-profile'  '(always)'     'Print profile / triple / sysroot / capabilities';
	@echo ''
	@echo 'Test (x86, rootfs):'
	@printf '  %-22s %-13s %s\n' \
		 'test'              '(rootfs)'     'Recursive make run (alias)';
	@printf '  %-22s %-13s %s\n' \
		 'test-phase-0'      '(rootfs)'     'QEMU phase-0 E2E against the normal image';
	@printf '  %-22s %-13s %s\n' \
		 'test-runtime'      '(rootfs)'     'Audit actual kernel runtime links + link-order fixture';
	@printf '  %-22s %-13s %s\n' \
		 'test-kernel-selftest' '(rootfs)'   'QEMU built-in selftests (isolated selftest image)';
	@printf '  %-22s %-13s %s\n' \
		 'test-syscall'      '(rootfs)'     'QEMU syscall E2E (OS01_SYSTEST=1, 228 tests)';
	@printf '  %-22s %-13s %s\n' \
		 'test-inittab'      '(rootfs)'     'inittab variant E2E (INITTAB_FILE=config/inittab.test)';
	@printf '  %-22s %-13s %s\n' \
		 'test-network'      '(rootfs)'     'QEMU network E2E (OS01_NETTEST=1, 6 tests)';
	@echo ''
	@echo 'Build contract (CI):'
	@printf '  %-22s %-13s %s\n' \
		 'test-build-contract-x86'      '(rootfs)'      'x86_64-clang full contract (7 modes)';
	@printf '  %-22s %-13s %s\n' \
		 'test-build-contract-aarch64'  '(uefi)' 'aarch64-clang full contract';
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

# ── Image alias ─────────────────────────────────────────────
# `make image` builds the current profile's disk image — variant-resolved
# (OS01_SYSTEST=1 / OS01_NETTEST=1 / INITTAB_FILE=config/inittab.test select
# the isolated variant image; otherwise the normal image).
.PHONY: image
image: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(DISK_IMG))
	$(call require_capability,rootfs)

# ── Build contract checks ───────────────────────────────────
# Ordered behind the profile's artifacts so `make -j2 ... test-build-contract-*`
# cannot race the build they inspect; capability-gated like every other alias.
# host-test runs LAST: it ends with `make clean`, which destroys the profile
# build dir (its purpose is to assert clean removes the profile outputs), and
# the earlier modes — targets in particular, whose `-n` kernel artifact recipe
# executes and resolves the sysroot generation — need that build dir intact.
.PHONY: test-build-contract-x86
test-build-contract-x86: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),disk.img)
	$(call require_capability,rootfs)
	sh tests/build_contract.sh x86_64-clang legacy-components
	sh tests/build_contract.sh x86_64-clang legacy
	sh tests/build_contract.sh x86_64-clang x86
	sh tests/build_contract.sh x86_64-clang sysroot
	sh tests/build_contract.sh x86_64-clang firmware
	sh tests/build_contract.sh x86_64-clang targets
	sh tests/build_contract.sh x86_64-clang host-test

.PHONY: test-build-contract-aarch64
test-build-contract-aarch64: aarch64-uefi
	$(call require_aarch64_uefi)
	$(call require_capability,uefi)
	sh tests/build_contract.sh aarch64-clang aarch64
	sh tests/build_contract.sh aarch64-clang targets

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
