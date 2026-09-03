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

# ── aarch64 UEFI bring-up (uefi-bringup capability) ────────────
# Targets are always defined so `make aarch64-uefi` under the default x86
# profile fails at the capability gate (not "no rule to make target"); the
# artifact prereqs exist only for uefi-bringup profiles (aarch64-clang). The
# kernel artifact + image rules live in mk/components/image.mk; there is no
# `lib` dependency — aarch64 does not consume the sysroot.
.PHONY: aarch64-uefi
aarch64-uefi: $(if $(filter uefi-bringup,$(PROFILE_CAPABILITIES)),$(AARCH64_UEFI_DISK) $(AARCH64_UEFI_FIRMWARE))
	$(call require_capability,uefi-bringup)

.PHONY: aarch64-uefi-kernel
aarch64-uefi-kernel: $(if $(filter uefi-bringup,$(PROFILE_CAPABILITIES)),$(BUILD_DIR)/artifacts/kernel.elf)
	$(call require_capability,uefi-bringup)

.PHONY: run-aarch64-uefi
run-aarch64-uefi: $(if $(filter uefi-bringup,$(PROFILE_CAPABILITIES)),aarch64-uefi)
	$(call require_capability,uefi-bringup)
	$(AARCH64_QEMU) -M virt,gic-version=2 -cpu cortex-a53 -smp 1 -m $(MEMORY) \
	  -drive if=pflash,format=raw,file=$(AARCH64_UEFI_FIRMWARE) \
	  -drive if=none,file=$(AARCH64_UEFI_DISK),format=raw,id=disk \
	  -device virtio-blk-device,drive=disk \
	  -serial stdio -display none -no-reboot

# ── Validation ─────────────────────────────────────────────
# validate keeps the x86 kernel + UEFI artifact checks (kernel ELF has no
# undefined symbols / INTERP / DYNAMIC, is EM_X86_64, exports _start /
# kernel_main / _text; the EFI app has a parseable COFF export table) and
# prints the selected profile's identity. x86-only: gated on the `uefi`
# capability (x86_64-clang has it; aarch64-clang has uefi-bringup) so an
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
	@echo "  [validate] kernel.elf machine is EM_X86_64"
	@$(LLVM_READOBJ) --file-headers $(KERNEL_ELF) | grep -F 'EM_X86_64'
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

.PHONY: test
test:
	$(call require_capability,rootfs)
	@$(call os01_submake,test,run $(OS01_SUBMAKE_ARGS))

.PHONY: test-phase-0
test-phase-0: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(NORMAL_IMAGE) $(OVMF_FIRMWARE))
	$(call require_capability,rootfs)
	OVMF_FIRMWARE="$(OVMF_FIRMWARE)" python3 tests/run_test.py phase-0 --disk $(NORMAL_IMAGE)

.PHONY: test-syscall
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

# ── Run paths ────────────────────────────────────────────────
# Prints the current profile's absolute firmware and normal-image paths so a
# manual QEMU invocation can reuse exactly the artifacts the root targets
# use. Gated on rootfs like every other x86 run entry point.
.PHONY: print-run-paths
print-run-paths:
	$(call require_capability,rootfs)
	@echo firmware=$(abspath $(OVMF_FIRMWARE))
	@echo image=$(abspath $(NORMAL_IMAGE))

# ── Help ─────────────────────────────────────────────────────
# Lists the root Makefile's user-facing targets, grouped by category, with the
# capability each one needs under the active profile. Works for any profile
# (no capability gate): the categories shown are the same, and the per-target
# capability badge is evaluated against the active profile's
# PROFILE_CAPABILITIES, so a user on aarch64-clang sees (uefi-bringup) badges
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
	@echo 'aarch64 UEFI bring-up (uefi-bringup):'
	@printf '  %-22s %-13s %s\n' \
		 'aarch64-uefi'           '(uefi-bringup)' 'Build aarch64 disk + firmware';
	@printf '  %-22s %-13s %s\n' \
		 'aarch64-uefi-kernel'    '(uefi-bringup)' 'Build aarch64 kernel.elf only';
	@printf '  %-22s %-13s %s\n' \
		 'run-aarch64-uefi'       '(uefi-bringup)' 'QEMU virt + cortex-a53 + virtio-blk';
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
		 'test-build-contract-aarch64'  '(uefi-bringup)' 'aarch64-clang full contract';
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
test-build-contract-aarch64: $(if $(filter uefi-bringup,$(PROFILE_CAPABILITIES)),aarch64-uefi)
	$(call require_capability,uefi-bringup)
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
