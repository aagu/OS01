base:=$(shell pwd)
# CC/LD are NOT exported: kernel/Makefile picks them per ARCH (x86_64 vs
# aarch64). User-space targets (busybox, mbedtls, etc.) below invoke
# clang -target x86_64-unknown-none directly — those remain x86-only and
# are out of scope for the aarch64 port.
export AR=llvm-ar
export OBJ_CPY=llvm-objcopy

ifneq ($(shell uname -m),aarch64)
QEMU_BIN=qemu-system-x86_64
else
export CROSS_BASE=$(base)/toolchain/cross
QEMU_BIN=$(CROSS_BASE)/bin/qemu-system-x86_64
endif

export SYSROOT=$(base)/sysroot
export PREFIX=/usr
export EXEC_PREFIX=${PREFIX}
export LIBDIR=${EXEC_PREFIX}/lib
export INCLUDEDIR=${PREFIX}/include
export CFLAGS=--sysroot=${SYSROOT} -isystem=${INCLUDEDIR} -g -fno-stack-protector
export LDFLAGS=--sysroot=${SYSROOT}

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

boot/uefi/BOOTX64.EFI: boot/uefi/main.c
	make -C boot/uefi

boot/uefi/OVMF.fd:
	make -C boot/uefi OVMF.fd

# ── Libraries ───────────────────────────────────────────

.PHONY: lib
lib:
	make -C kernel install-headers
	make -C libc install

# ── Kernel ──────────────────────────────────────────────

.PHONY: kernel/kernel.bin
kernel/kernel.bin: lib
	make -C kernel kernel.bin

# kernel.bin is built by kernel/Makefile and placed at project root
kernel.bin: lib
	make -C kernel kernel.bin

# ── User programs ───────────────────────────────────────

.PHONY: user
user:
	make -C user

build/x86_64/user/busybox.elf: thirdpart/busybox-1.36.1/busybox
	@mkdir -p $(dir $@)
	cp $< $@

# ── BusyBox ─────────────────────────────────────────────
# Submodule must be initialized: git submodule update --init

BUSYBOX_SRC  = thirdpart/busybox-1.36.1
BUSYBOX_CFG  = config/busybox.config
BUSYBOX_LIBS = $(SYSROOT)$(LIBDIR)/libc.a $(SYSROOT)$(LIBDIR)/libk.a

# The archives are normally installed by the phony `lib` target before the
# disk-image prerequisites are considered.  Keep direct BusyBox builds usable
# too, without making the BusyBox binary depend on the phony target itself.
libc/libc.a libc/libk.a &:
	$(MAKE) -C kernel install-headers
	$(MAKE) -C libc

$(BUSYBOX_LIBS) &: libc/libc.a libc/libk.a
	$(MAKE) -C kernel install-headers
	$(MAKE) -C libc install

# ── mbedTLS ─────────────────────────────────────────────────
MBEDTLS_SRC = thirdpart/mbedtls
MBEDTLS_LIB = $(SYSROOT)/usr/lib/libmbedtls.a

$(MBEDTLS_LIB): lib config/mbedtls_config.h libc/network/entropy.c
	@test -d $(MBEDTLS_SRC)/library || { \
	    echo "ERROR: mbedtls submodule not initialized"; \
	    echo "Run: git submodule update --init"; false; }
	@cp config/mbedtls_config.h $(MBEDTLS_SRC)/include/mbedtls/os01_mbedtls_config.h
	@mkdir -p $(SYSROOT)/usr/lib $(SYSROOT)/usr/include
	@rm -rf /tmp/mbedtls_build && mkdir -p /tmp/mbedtls_build
	@echo "  [mbedtls] compiling 108 library files..."
	@ok=0; fail=0; \
	for src in $(MBEDTLS_SRC)/library/*.c; do \
	    name=$$(basename $$src .c); \
	    if clang -target x86_64-unknown-none \
	        --sysroot=$(SYSROOT) -isystem=$(INCLUDEDIR) \
	        -g -ffreestanding -fno-stack-protector \
	        -I$(MBEDTLS_SRC)/include -I$(MBEDTLS_SRC)/library \
	        -DMBEDTLS_USER_CONFIG_FILE='<mbedtls/os01_mbedtls_config.h>' \
	        -c $$src -o /tmp/mbedtls_build/$$name.o 2>/dev/null; then \
	        ok=$$((ok+1)); \
	    else \
	        fail=$$((fail+1)); \
	        [ $$fail -le 3 ] && echo "  [mbedtls] FAIL: $$name" && \
	          clang -target x86_64-unknown-none \
	            --sysroot=$(SYSROOT) -isystem=$(INCLUDEDIR) \
	            -g -ffreestanding -fno-stack-protector \
	            -I$(MBEDTLS_SRC)/include -I$(MBEDTLS_SRC)/library \
	            -DMBEDTLS_USER_CONFIG_FILE='<mbedtls/os01_mbedtls_config.h>' \
	            -c $$src -o /tmp/mbedtls_build/$$name.o 2>&1 | grep "error:" | head -1; \
	    fi; \
	done; \
	clang -target x86_64-unknown-none \
	    --sysroot=$(SYSROOT) -isystem=$(INCLUDEDIR) \
	    -g -ffreestanding -fno-stack-protector \
	    -I$(MBEDTLS_SRC)/include -I$(MBEDTLS_SRC)/library \
	    -DMBEDTLS_USER_CONFIG_FILE='<mbedtls/os01_mbedtls_config.h>' \
	    -c libc/network/entropy.c -o /tmp/mbedtls_build/os01_entropy.o 2>/dev/null && ok=$$((ok+1)); \
	echo "  [mbedtls] $$ok compiled"
	@llvm-ar rcs $(MBEDTLS_LIB) /tmp/mbedtls_build/*.o
	@cp -R $(MBEDTLS_SRC)/include/mbedtls $(SYSROOT)/usr/include/
	@rm -rf /tmp/mbedtls_build
	@echo "  [mbedtls] libmbedtls.a installed"

thirdpart/busybox-1.36.1/busybox: $(BUSYBOX_LIBS) $(BUSYBOX_SRC)/Makefile $(BUSYBOX_CFG) user/crt0.S user/sigreturn_trampoline.S
	@test -f $(BUSYBOX_SRC)/Makefile || { \
	    echo "ERROR: busybox submodule not initialized"; \
	    echo "Run: git submodule update --init"; false; }
	@cmp -s user/crt0.S $(BUSYBOX_SRC)/applets/crt0.S || cp user/crt0.S $(BUSYBOX_SRC)/applets/crt0.S
	@cmp -s user/sigreturn_trampoline.S $(BUSYBOX_SRC)/applets/sigreturn_trampoline.S || cp user/sigreturn_trampoline.S $(BUSYBOX_SRC)/applets/sigreturn_trampoline.S
	@cmp -s $(BUSYBOX_CFG) $(BUSYBOX_SRC)/.config || cp $(BUSYBOX_CFG) $(BUSYBOX_SRC)/.config
	@grep -qxF 'obj-y += crt0.o' $(BUSYBOX_SRC)/applets/Kbuild.src || \
	    echo 'obj-y += crt0.o' >> $(BUSYBOX_SRC)/applets/Kbuild.src
	@grep -qxF 'obj-y += sigreturn_trampoline.o' $(BUSYBOX_SRC)/applets/Kbuild.src || \
	    echo 'obj-y += sigreturn_trampoline.o' >> $(BUSYBOX_SRC)/applets/Kbuild.src
	$(MAKE) -C $(BUSYBOX_SRC) silentoldconfig CC=clang LD=clang 2>/dev/null || \
	yes "" | $(MAKE) -C $(BUSYBOX_SRC) oldconfig CC=clang LD=clang
	@mkdir -p $(SYSROOT)/usr/lib
	@if [ ! -f $(SYSROOT)/usr/lib/libm.a ] || [ ! -f $(SYSROOT)/usr/lib/librt.a ]; then \
	    touch /tmp/os01-busybox-stub.c && clang -c -x c /tmp/os01-busybox-stub.c -o /tmp/os01-busybox-stub.o 2>/dev/null; \
	    test -f $(SYSROOT)/usr/lib/libm.a || llvm-ar rcs $(SYSROOT)/usr/lib/libm.a /tmp/os01-busybox-stub.o; \
	    test -f $(SYSROOT)/usr/lib/librt.a || llvm-ar rcs $(SYSROOT)/usr/lib/librt.a /tmp/os01-busybox-stub.o; \
	    rm -f /tmp/os01-busybox-stub.c /tmp/os01-busybox-stub.o; \
	fi
	$(MAKE) -C $(BUSYBOX_SRC) CC=clang LD=clang

# ── Disk image ──────────────────────────────────────────

disk.img: boot/uefi/BOOTX64.EFI lib kernel.bin user build/x86_64/user/busybox.elf
	@mkdir -p config/fsroot/bin config/fsroot/home config/fsroot/etc
	@cp build/x86_64/user/init.elf          config/fsroot/bin/init
	@cp build/x86_64/user/busybox.elf        config/fsroot/bin/busybox
	@cp build/x86_64/user/spin.elf           config/fsroot/bin/spin
	@cp build/x86_64/user/sigtest.elf        config/fsroot/bin/sigtest
	@cp build/x86_64/user/poweroff.elf       config/fsroot/bin/poweroff
	@cp build/x86_64/user/halt.elf           config/fsroot/bin/halt
	@cp build/x86_64/user/reboot.elf         config/fsroot/bin/reboot
	@cp build/x86_64/user/systest.elf        config/fsroot/bin/systest
	@cp build/x86_64/user/test_mmap.elf      config/fsroot/bin/test_mmap
	@cp build/x86_64/user/test_fork_mmap.elf config/fsroot/bin/test_fork_mmap
	@cp build/x86_64/user/test_cow.elf       config/fsroot/bin/test_cow
	@cp build/x86_64/user/terminal.elf       config/fsroot/bin/terminal
	@cp build/x86_64/user/smp_stress.elf     config/fsroot/bin/smp_stress
	@cp $(INITTAB_FILE) config/fsroot/etc/inittab
	@cp build/x86_64/user/socktest.elf      config/fsroot/bin/socktest
	@cp build/x86_64/user/udptest.elf       config/fsroot/bin/udptest
	@cp build/x86_64/user/ipaddr.elf        config/fsroot/bin/ipaddr
	@cp build/x86_64/user/nettest.elf       config/fsroot/bin/nettest
	@cp build/x86_64/user/tetris.elf        config/fsroot/bin/tetris
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
	    --efi boot/uefi/BOOTX64.EFI \
	    --kernel kernel.bin \
	    --rootfs config/fsroot/

# ── Run / Debug ─────────────────────────────────────────

run: disk.img boot/uefi/OVMF.fd
	$(QEMU_BIN) -M q35 -smp $(SMP) -pflash boot/uefi/OVMF.fd \
	  -netdev user,id=net0 -device e1000e,netdev=net0 \
	  -drive file=disk.img,format=raw,if=none,id=disk \
	  -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
	  -m $(MEMORY) -display $(DISPLAY) -serial stdio -no-reboot

# aarch64 phase 1: bare-ELF -kernel boot (no disk.img / UEFI / userland).
# See docs/superpowers/specs/2026-08-29-aarch64-phase1-design.md §9 for the
# three QEMU-11.1 deviations: secure=on (else only BSP starts), gic-version=2
# (code hardcodes GICv2 MMIO), and bare-ELF x0=0 / DTB@0x40000000.
AARCH64_QEMU ?= qemu-system-aarch64
AARCH64_SMP  ?= 4
AARCH64_BUILD_DIR := build/aarch64
AARCH64_UEFI_APP := $(AARCH64_BUILD_DIR)/uefi/BOOTAA64.EFI
AARCH64_UEFI_DISK := $(AARCH64_BUILD_DIR)/disk.img
AARCH64_UEFI_FIRMWARE := $(AARCH64_BUILD_DIR)/QEMU_EFI.fd
AARCH64_UEFI_FIRMWARE_SOURCE ?= /usr/share/edk2/aarch64/QEMU_EFI.fd
AARCH64_KERNEL_ELF := $(AARCH64_BUILD_DIR)/kernel/kernel.elf
AARCH64_HEAD_OBJECT := $(AARCH64_BUILD_DIR)/kernel/arch/aarch64/head.o

.PHONY: run-aarch64
run-aarch64:
	$(MAKE) -C kernel ARCH=aarch64
	$(AARCH64_QEMU) -M virt,secure=on,gic-version=2 -cpu cortex-a53 \
	  -smp $(AARCH64_SMP) -m $(MEMORY) \
	  -kernel build/aarch64/kernel/kernel.elf \
	  -display $(DISPLAY) -serial stdio -no-reboot

.PHONY: aarch64-uefi
aarch64-uefi: $(AARCH64_UEFI_DISK) $(AARCH64_UEFI_FIRMWARE)

$(AARCH64_UEFI_APP): boot/uefi/aarch64/Makefile \
		boot/uefi/aarch64/main.c boot/uefi/aarch64/elf.c \
		boot/uefi/aarch64/handoff.S \
		boot/uefi/aarch64/loader.h kernel/include/kernel/bootinfo.h
	$(MAKE) -C boot/uefi/aarch64

.PHONY: aarch64-uefi-kernel
aarch64-uefi-kernel:
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

# Start the AArch64 phase-1 kernel paused with QEMU's GDB remote stub on
# TCP port 1234.  This is consumed by .vscode/launch.json.
.PHONY: debug-aarch64
debug-aarch64:
	$(MAKE) -C kernel ARCH=aarch64 DEBUG=1
	@echo "AArch64 QEMU GDB server listening on tcp://127.0.0.1:1234"
	$(AARCH64_QEMU) -M virt,secure=on,gic-version=2 -cpu cortex-a53 \
	  -smp $(AARCH64_SMP) -m $(MEMORY) \
	  -kernel build/aarch64/kernel/kernel.elf \
	  -display $(DISPLAY) -serial stdio -no-reboot \
	  -S -gdb tcp::1234

run-kvm: disk.img boot/uefi/OVMF.fd
	$(QEMU_BIN) -M q35 -smp $(SMP) -pflash boot/uefi/OVMF.fd \
	  -accel kvm \
	  -netdev user,id=net0 -device e1000e,netdev=net0 \
	  -drive file=disk.img,format=raw,if=none,id=disk \
	  -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
	  -m $(MEMORY) -display $(DISPLAY) -serial stdio -no-reboot

# ── VirtIO-net test ──────────────────────────────────────
run-virtio: disk.img boot/uefi/OVMF.fd
	$(QEMU_BIN) -M q35 -smp $(SMP) -pflash boot/uefi/OVMF.fd \
	  -netdev user,id=net0 -device virtio-net-pci,netdev=net0 \
	  -drive file=disk.img,format=raw,if=none,id=disk \
	  -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
	  -m $(MEMORY) -display $(DISPLAY) -serial stdio

debug: disk.img boot/uefi/OVMF.fd
	$(QEMU_BIN) -M q35 -smp $(SMP) -pflash boot/uefi/OVMF.fd \
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

.PHONY: test-network
test-network:
	rm -f disk.img
	$(MAKE) OS01_NETTEST=1 disk.img boot/uefi/OVMF.fd
	python3 tests/run_test.py network

# ── Clean ───────────────────────────────────────────────

.PHONY: clean
clean:
	rm -rf disk.img
	make -C boot/uefi distclean
	make -C kernel clean
	make -C libc clean
	make -C user clean
	rm -rf test/build sysroot build
	if [ -f $(BUSYBOX_SRC)/Makefile ]; then \
	    $(MAKE) -C $(BUSYBOX_SRC) clean 2>/dev/null || true; \
	    rm -f $(BUSYBOX_SRC)/applets/crt0.S $(BUSYBOX_SRC)/applets/Kbuild.src.bak; \
	fi
