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
# the sysroot; kernel.mk owns the kernel artifact.
include $(base)/mk/components/sysroot.mk
include $(base)/mk/components/kernel.mk
include $(base)/mk/components/user.mk

# UEFI_EFI compat: boot/uefi still writes build/x86_64/uefi/BOOTX64.EFI until
# Task 6 rewires it onto the profile layout; the profile's UEFI_EFI points at
# build/<profile>/uefi which nothing produces yet. Keep validate-uefi working.
UEFI_EFI := build/x86_64/uefi/BOOTX64.EFI

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

all: disk.img

# ── Bootloader ──────────────────────────────────────────

BUILD_X86_64_UEFI := build/x86_64/uefi/BOOTX64.EFI

$(BUILD_X86_64_UEFI): boot/uefi/Makefile boot/uefi/main.c \
		boot/uefi/arch/arch.h boot/uefi/arch/x86_64/boot.c \
		kernel/include/kernel/bootinfo.h
	make -C boot/uefi ARCH=x86_64

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
validate-uefi: $(BUILD_X86_64_UEFI)
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
	cp $(KERNEL_ARTIFACT) kernel.bin
endif

# ── User programs ───────────────────────────────────────

# user = the profile's user ELFs + BusyBox (mk/components/user.mk), all
# built against the leased immutable sysroot generation. Only defined for
# userland-capable profiles; on others the recipe fires the capability gate.
.PHONY: user
user: $(if $(filter userland,$(PROFILE_CAPABILITIES)),$(USER_ARTIFACTS) $(USER_ARTIFACT_DIR)/busybox.elf)
	$(call require_capability,userland)

# ── Disk image ──────────────────────────────────────────

disk.img: $(BUILD_X86_64_UEFI) lib kernel.bin user $(USER_ARTIFACT_DIR)/busybox.elf
	@mkdir -p config/fsroot/bin config/fsroot/home config/fsroot/etc
	@cp $(USER_ARTIFACT_DIR)/init.elf          config/fsroot/bin/init
	@cp $(USER_ARTIFACT_DIR)/busybox.elf        config/fsroot/bin/busybox
	@cp $(USER_ARTIFACT_DIR)/spin.elf           config/fsroot/bin/spin
	@cp $(USER_ARTIFACT_DIR)/sigtest.elf        config/fsroot/bin/sigtest
	@cp $(USER_ARTIFACT_DIR)/poweroff.elf       config/fsroot/bin/poweroff
	@cp $(USER_ARTIFACT_DIR)/halt.elf           config/fsroot/bin/halt
	@cp $(USER_ARTIFACT_DIR)/reboot.elf         config/fsroot/bin/reboot
	@cp $(USER_ARTIFACT_DIR)/systest.elf        config/fsroot/bin/systest
	@cp $(USER_ARTIFACT_DIR)/test_mmap.elf      config/fsroot/bin/test_mmap
	@cp $(USER_ARTIFACT_DIR)/test_fork_mmap.elf config/fsroot/bin/test_fork_mmap
	@cp $(USER_ARTIFACT_DIR)/test_cow.elf       config/fsroot/bin/test_cow
	@cp $(USER_ARTIFACT_DIR)/terminal.elf       config/fsroot/bin/terminal
	@cp $(USER_ARTIFACT_DIR)/smp_stress.elf     config/fsroot/bin/smp_stress
	@cp $(INITTAB_FILE) config/fsroot/etc/inittab
	@cp $(USER_ARTIFACT_DIR)/socktest.elf      config/fsroot/bin/socktest
	@cp $(USER_ARTIFACT_DIR)/udptest.elf       config/fsroot/bin/udptest
	@cp $(USER_ARTIFACT_DIR)/ipaddr.elf        config/fsroot/bin/ipaddr
	@cp $(USER_ARTIFACT_DIR)/nettest.elf       config/fsroot/bin/nettest
	@cp $(USER_ARTIFACT_DIR)/tetris.elf        config/fsroot/bin/tetris
	@ln -sf busybox config/fsroot/bin/wget
	@ln -sf busybox config/fsroot/bin/login
	@ln -sf busybox config/fsroot/bin/sh
	@ln -sf busybox config/fsroot/bin/[
	@ln -sf busybox config/fsroot/bin/[[
	@ln -sf busybox config/fsroot/bin/cat
	@ln -sf busybox config/fsroot/bin/cp
	@ln -sf busybox config/fsroot/bin/mv
	@ln -sf busybox config/fsroot/bin/rm
	@ln -sf busybox config/fsroot/bin/mkdir
	@ln -sf busybox config/fsroot/bin/rmdir
	@ln -sf busybox config/fsroot/bin/echo
	@ln -sf busybox config/fsroot/bin/printf
	@ln -sf busybox config/fsroot/bin/sort
	@ln -sf busybox config/fsroot/bin/ps
	@ln -sf busybox config/fsroot/bin/kill
	@ln -sf busybox config/fsroot/bin/mount
	@ln -sf busybox config/fsroot/bin/grep
	@ln -sf busybox config/fsroot/bin/sed
	@ln -sf busybox config/fsroot/bin/awk
	@ln -sf busybox config/fsroot/bin/find
	@ln -sf busybox config/fsroot/bin/xargs
	@ln -sf busybox config/fsroot/bin/tar
	@ln -sf busybox config/fsroot/bin/gzip
	@ln -sf busybox config/fsroot/bin/gunzip
	@ln -sf busybox config/fsroot/bin/ping
	@ln -sf busybox config/fsroot/bin/ifconfig
	@ln -sf busybox config/fsroot/bin/clear
	@ln -sf busybox config/fsroot/bin/dmesg
	$(MAKE) -C tools check-deps
	$(MAKE) -C tools
	tools/mkdisk disk.img \
	    --efi $(BUILD_X86_64_UEFI) \
	    --kernel kernel.bin \
	    --rootfs config/fsroot/

# ── Run / Debug ─────────────────────────────────────────

run: disk.img boot/uefi/OVMF.fd
	$(QEMU_BIN) -M q35 -smp $(SMP) \
	  -drive if=pflash,format=raw,readonly=on,file=boot/uefi/OVMF.fd \
	  -netdev user,id=net0 -device e1000e,netdev=net0 \
	  -drive file=disk.img,format=raw,if=none,id=disk \
	  -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
	  -m $(MEMORY) -display $(DISPLAY) -serial stdio -no-reboot

AARCH64_QEMU ?= qemu-system-aarch64
AARCH64_SMP  ?= 4
AARCH64_BUILD_DIR := build/aarch64
AARCH64_UEFI_APP := $(AARCH64_BUILD_DIR)/uefi/BOOTAA64.EFI
AARCH64_UEFI_DISK := $(AARCH64_BUILD_DIR)/disk.img
AARCH64_UEFI_FIRMWARE := $(AARCH64_BUILD_DIR)/QEMU_EFI.fd
AARCH64_UEFI_FIRMWARE_SOURCE ?= /usr/share/edk2/aarch64/QEMU_EFI.fd
AARCH64_KERNEL_ELF := $(AARCH64_BUILD_DIR)/kernel/kernel.elf
AARCH64_HEAD_OBJECT := $(AARCH64_BUILD_DIR)/kernel/arch/aarch64/head.o

.PHONY: aarch64-uefi
aarch64-uefi: $(AARCH64_UEFI_DISK) $(AARCH64_UEFI_FIRMWARE)

$(AARCH64_UEFI_APP): boot/uefi/Makefile \
		boot/uefi/main.c boot/uefi/arch/arch.h \
		boot/uefi/arch/aarch64/boot.c boot/uefi/arch/aarch64/elf.c \
		boot/uefi/arch/aarch64/handoff.S \
		boot/uefi/arch/aarch64/loader.h kernel/include/kernel/bootinfo.h
	$(MAKE) -C boot/uefi ARCH=aarch64

.PHONY: aarch64-uefi-kernel
aarch64-uefi-kernel: lib
	$(MAKE) -B -C kernel ARCH=aarch64

$(AARCH64_UEFI_DISK): $(AARCH64_UEFI_APP) aarch64-uefi-kernel
	@mkdir -p $(AARCH64_BUILD_DIR)
	rm -f $@
	truncate -s 64M $@
	mkfs.fat -F 32 $@
	mmd -i $@ ::/EFI ::/EFI/BOOT
	mcopy -i $@ $(AARCH64_UEFI_APP) ::/EFI/BOOT/BOOTAA64.EFI
	mcopy -i $@ $(AARCH64_KERNEL_ELF) ::/kernel.elf

$(AARCH64_UEFI_FIRMWARE): $(AARCH64_UEFI_FIRMWARE_SOURCE)
	@mkdir -p $(dir $@)
	cp $< $@

.PHONY: run-aarch64-uefi
run-aarch64-uefi: aarch64-uefi
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
