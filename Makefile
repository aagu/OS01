ROOT_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
base := $(patsubst %/,%,$(dir $(ROOT_MAKEFILE)))

# ── Profile selection ───────────────────────────────────────
# mk/project.mk selects PROFILE (default x86_64-clang), includes the
# profile (validated toolchain + host/run/link parameters), and defines
# require_capability / os01_submake / OS01_SUBMAKEFLAGS. The old ARCH
# dispatch, root toolchain.mk include and broad exports are gone: component
# Makefiles consume the profile directly (via OS01_PROFILE_FILE), never
# through implicit environment inheritance.
include $(base)/mk/project.mk
# Cross-component dependency graph (spec: mk/components/*.mk is the only
# place that wires components together). sysroot.mk is the single writer of
# the sysroot; kernel.mk owns the kernel artifact; uefi.mk owns the UEFI
# runtime adapter + EFI apps; image.mk owns the rootfs manifest and the disk
# images.
include $(base)/mk/components/sysroot.mk
include $(base)/mk/components/kernel.mk
include $(base)/mk/components/user.mk
include $(base)/mk/components/uefi.mk

# ── Run parameters (profile-agnostic; shared by x86 and aarch64) ──
DISPLAY=gtk
MEMORY=512M
SMP ?= 2

# ── Log output target (serial | fb | both) ───────────────
LOG_TARGET ?= serial
DEBUG      ?=
KERNEL_SELFTEST ?=
export KERNEL_SELFTEST

# ── Inittab ────────────────────────────────────────────────
INITTAB_FILE ?= config/inittab
ifeq ($(OS01_SYSTEST),1)
INITTAB_FILE := config/inittab.systest
endif
ifeq ($(OS01_NETTEST),1)
INITTAB_FILE := config/inittab.nettest
endif

# image.mk owns the rootfs manifest + disk images. It is included after the
# INITTAB_FILE selection because config/rootfs.mk references $(INITTAB_FILE).
include $(base)/mk/components/image.mk

all: disk.img

# ── Validation ─────────────────────────────────────────

.PHONY: validate validate-kernel validate-uefi
validate: validate-kernel validate-uefi
validate-kernel: kernel.bin
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
validate-uefi: $(UEFI_EFI)
	@echo "  [validate] BOOTX64.EFI coff-exports"
	@$(LLVM_READOBJ) --coff-exports $(UEFI_EFI) >/dev/null

boot/uefi/OVMF.fd:
	make -C boot/uefi OVMF.fd

# ── Libraries ───────────────────────────────────────────

# `lib` = the profile's sysroot libraries: stages kernel headers, libc/libk,
# mbedTLS and compat-libs, then publishes an immutable sysroot generation
# (the stamp's recipe in mk/components/sysroot.mk does the work). The
# sysroot.stamp prerequisite only exists for userland profiles, so on others
# this is a bare phony whose recipe fires the capability gate.
.PHONY: lib
lib: $(if $(filter userland,$(PROFILE_CAPABILITIES)),$(SYSROOT_STAMP))
	$(call require_capability,userland)

# ── Kernel ──────────────────────────────────────────────

# Project-root kernel.bin is a one-way copy of the profile's kernel artifact
# (mk/components/kernel.mk). Only defined when the profile declares a kernel
# artifact (x86_64-clang); other profiles simply fail on `make kernel.bin`.
ifdef KERNEL_ARTIFACT
kernel.bin: $(KERNEL_ARTIFACT)
	@cmp -s $(KERNEL_ARTIFACT) $@ || cp $(KERNEL_ARTIFACT) $@
endif

# ── User programs ───────────────────────────────────────

# user = the profile's user ELFs + BusyBox (mk/components/user.mk), all
# built against the leased immutable sysroot generation. Only defined for
# userland-capable profiles; on others the recipe fires the capability gate.
.PHONY: user
user: $(if $(filter userland,$(PROFILE_CAPABILITIES)),$(USER_ARTIFACTS) $(USER_ARTIFACT_DIR)/busybox.elf)
	$(call require_capability,userland)

# ── Disk image ──────────────────────────────────────────
# disk.img (project root) is the image.mk compat target: a content-guarded
# copy of the profile's $(BUILD_DIR)/image/disk.img. The rootfs contents and
# mkdisk invocation live in mk/components/image.mk + config/rootfs.mk.

# ── Run / Debug ─────────────────────────────────────────

run: disk.img boot/uefi/OVMF.fd
	$(QEMU_BIN) -M q35 -smp $(SMP) \
	  -drive if=pflash,format=raw,readonly=on,file=boot/uefi/OVMF.fd \
	  -netdev user,id=net0 -device e1000e,netdev=net0 \
	  -drive file=disk.img,format=raw,if=none,id=disk \
	  -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
	  -m $(MEMORY) -display $(DISPLAY) -serial stdio -no-reboot

# ── aarch64 UEFI bring-up (uefi-bringup capability) ─────────
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

run-kvm: disk.img boot/uefi/OVMF.fd
	$(QEMU_BIN) -M q35 -smp $(SMP) \
	  -drive if=pflash,format=raw,readonly=on,file=boot/uefi/OVMF.fd \
	  -accel kvm \
	  -netdev user,id=net0 -device e1000e,netdev=net0 \
	  -drive file=disk.img,format=raw,if=none,id=disk \
	  -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
	  -m $(MEMORY) -display $(DISPLAY) -serial stdio -no-reboot

# ── VirtIO-net test ──────────────────────────────────────
run-virtio: disk.img boot/uefi/OVMF.fd
	$(QEMU_BIN) -M q35 -smp $(SMP) \
	  -drive if=pflash,format=raw,readonly=on,file=boot/uefi/OVMF.fd \
	  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
	  -drive file=disk.img,format=raw,if=none,id=disk \
	  -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
	  -m $(MEMORY) -display $(DISPLAY) -serial stdio

debug: disk.img boot/uefi/OVMF.fd
	$(QEMU_BIN) -M q35 -smp $(SMP) \
	  -drive if=pflash,format=raw,readonly=on,file=boot/uefi/OVMF.fd \
	  -S -s \
	  -netdev user,id=net0 -device e1000e,netdev=net0 \
	  -drive file=disk.img,format=raw,if=none,id=disk \
	  -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
	  -m $(MEMORY) -display $(DISPLAY) -serial stdio

# ── Test ────────────────────────────────────────────────

.PHONY: test
test:
	make -C test run

.PHONY: test-phase-0
test-phase-0: disk.img
	python3 tests/run_test.py phase-0

.PHONY: test-syscall
test-syscall:
	rm -f disk.img
	$(MAKE) OS01_SYSTEST=1 disk.img boot/uefi/OVMF.fd
	python3 tests/run_test.py systest

.PHONY: test-inittab
test-inittab:
	rm -f disk.img
	$(MAKE) INITTAB_FILE=config/inittab.test disk.img boot/uefi/OVMF.fd
	python3 tests/run_test.py inittab-phase

.PHONY: test-build-contract-x86
test-build-contract-x86:
	sh tests/build_contract.sh x86_64-clang x86
	sh tests/build_contract.sh x86_64-clang sysroot
	sh tests/build_contract.sh x86_64-clang targets

.PHONY: test-build-contract-aarch64
test-build-contract-aarch64:
	sh tests/build_contract.sh aarch64-clang aarch64
	sh tests/build_contract.sh aarch64-clang targets

.PHONY: test-network
test-network:
	rm -f disk.img
	$(MAKE) OS01_NETTEST=1 disk.img boot/uefi/OVMF.fd
	python3 tests/run_test.py network

# ── Clean ───────────────────────────────────────────────

# clean takes the publish lock (60 s retry), fails without deleting anything
# if a generation read lease exists, then removes build/<profile> (NOT the
# whole build/ tree — build/.locks/ and other profiles survive) plus the
# legacy per-component build dirs and the project-root compat artifacts. All
# of it runs in ONE shell line so the trap releases the lock in every path
# (success or failure), and the lock is held for the whole clean.
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
	rm -f disk.img kernel.bin; \
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
