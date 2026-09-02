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
boot/uefi/OVMF.fd:
	$(MAKE) -C boot/uefi OVMF.fd

# ── Kernel compat copy ─────────────────────────────────────────
# Project-root kernel.bin is a one-way copy of the profile's kernel artifact
# (mk/components/kernel.mk). Only defined when the profile declares a kernel
# artifact (x86_64-clang); other profiles simply fail on `make kernel.bin`.
ifdef KERNEL_ARTIFACT
kernel.bin: $(KERNEL_ARTIFACT)
	@cmp -s $(KERNEL_ARTIFACT) $@ || cp $(KERNEL_ARTIFACT) $@
endif

# ── Run (x86, rootfs capability) ───────────────────────────────
# Project-root disk.img is the concrete default-profile compat copy of the
# NORMAL image (image.mk); OVMF.fd is the shared x86 UEFI firmware.
.PHONY: run
run: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),disk.img boot/uefi/OVMF.fd)
	$(call require_capability,rootfs)
	$(QEMU_BIN) -M q35 -smp $(SMP) \
	  -drive if=pflash,format=raw,readonly=on,file=boot/uefi/OVMF.fd \
	  -netdev user,id=net0 -device e1000e,netdev=net0 \
	  -drive file=disk.img,format=raw,if=none,id=disk \
	  -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
	  -m $(MEMORY) -display $(DISPLAY) -serial stdio -no-reboot

.PHONY: run-kvm
run-kvm: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),disk.img boot/uefi/OVMF.fd)
	$(call require_capability,rootfs)
	$(QEMU_BIN) -M q35 -smp $(SMP) \
	  -drive if=pflash,format=raw,readonly=on,file=boot/uefi/OVMF.fd \
	  -accel kvm \
	  -netdev user,id=net0 -device e1000e,netdev=net0 \
	  -drive file=disk.img,format=raw,if=none,id=disk \
	  -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
	  -m $(MEMORY) -display $(DISPLAY) -serial stdio -no-reboot

.PHONY: run-virtio
run-virtio: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),disk.img boot/uefi/OVMF.fd)
	$(call require_capability,rootfs)
	$(QEMU_BIN) -M q35 -smp $(SMP) \
	  -drive if=pflash,format=raw,readonly=on,file=boot/uefi/OVMF.fd \
	  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
	  -drive file=disk.img,format=raw,if=none,id=disk \
	  -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
	  -m $(MEMORY) -display $(DISPLAY) -serial stdio

.PHONY: debug
debug: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),disk.img boot/uefi/OVMF.fd)
	$(call require_capability,rootfs)
	$(QEMU_BIN) -M q35 -smp $(SMP) \
	  -drive if=pflash,format=raw,readonly=on,file=boot/uefi/OVMF.fd \
	  -S -s \
	  -netdev user,id=net0 -device e1000e,netdev=net0 \
	  -drive file=disk.img,format=raw,if=none,id=disk \
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
	$(MAKE) -C test run

.PHONY: test-phase-0
test-phase-0: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),$(NORMAL_IMAGE))
	$(call require_capability,rootfs)
	python3 tests/run_test.py phase-0 --disk $(NORMAL_IMAGE)

.PHONY: test-syscall
test-syscall:
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
	DISK_IMG="$(TEST_SYSTEST_IMAGE)" python3 tests/run_test.py systest

.PHONY: test-inittab
test-inittab:
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
	DISK_IMG="$(TEST_INITTAB_IMAGE)" python3 tests/run_test.py inittab-phase

.PHONY: test-network
test-network:
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
	DISK_IMG="$(TEST_NETTEST_IMAGE)" python3 tests/run_test.py network

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
.PHONY: test-build-contract-x86
test-build-contract-x86: $(if $(filter rootfs,$(PROFILE_CAPABILITIES)),disk.img)
	$(call require_capability,rootfs)
	sh tests/build_contract.sh x86_64-clang legacy-components
	sh tests/build_contract.sh x86_64-clang legacy
	sh tests/build_contract.sh x86_64-clang x86
	sh tests/build_contract.sh x86_64-clang sysroot
	sh tests/build_contract.sh x86_64-clang firmware
	sh tests/build_contract.sh x86_64-clang host-test
	sh tests/build_contract.sh x86_64-clang targets

.PHONY: test-build-contract-aarch64
test-build-contract-aarch64: $(if $(filter uefi-bringup,$(PROFILE_CAPABILITIES)),aarch64-uefi)
	$(call require_capability,uefi-bringup)
	sh tests/build_contract.sh aarch64-clang aarch64
	sh tests/build_contract.sh aarch64-clang targets

# ── Clean ───────────────────────────────────────────────────
# Only the default profile owns the project-root kernel.bin / disk.img compat
# copies; other profiles remove just build/<profile>.
CLEAN_COMPAT := $(if $(filter $(DEFAULT_PROFILE),$(PROFILE)),rm -f disk.img kernel.bin;)

# clean takes the publish lock (60 s retry), fails without deleting anything
# if a generation read lease exists, then removes build/<profile> (NOT the
# whole build/ tree — build/.locks/ and other profiles survive) plus the
# legacy per-component build dirs and, for the default profile, the
# project-root compat artifacts. All of it runs in ONE shell line so the trap
# releases the lock in every path (success or failure), and the lock is held
# for the whole clean.
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
	$(MAKE) -C boot/uefi clean; \
	$(MAKE) -C kernel clean; \
	$(MAKE) -C libc clean; \
	$(MAKE) -C user clean; \
	rm -rf test/build sysroot

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
