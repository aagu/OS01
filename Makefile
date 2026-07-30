base:=$(shell pwd)
export CC=clang -target x86_64-unknown-none
export LD=ld.lld -m elf_x86_64
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

# ── Log output target (serial | fb | both) ───────────────
LOG_TARGET ?= serial
DEBUG      ?=
KERNEL_SELFTEST ?=
export KERNEL_SELFTEST

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

thirdpart/busybox-1.36.1/busybox: lib $(BUSYBOX_SRC)/Makefile $(BUSYBOX_CFG) user/crt0.S user/sigreturn_trampoline.S
	@test -f $(BUSYBOX_SRC)/Makefile || { \
	    echo "ERROR: busybox submodule not initialized"; \
	    echo "Run: git submodule update --init"; false; }
	cp user/crt0.S $(BUSYBOX_SRC)/applets/crt0.S
	cp user/sigreturn_trampoline.S $(BUSYBOX_SRC)/applets/sigreturn_trampoline.S
	cp $(BUSYBOX_CFG) $(BUSYBOX_SRC)/.config
	@grep -q 'crt0.o' $(BUSYBOX_SRC)/applets/Kbuild.src || { \
	    cp $(BUSYBOX_SRC)/applets/Kbuild.src $(BUSYBOX_SRC)/applets/Kbuild.src.bak; \
	    echo 'obj-y += crt0.o' >> $(BUSYBOX_SRC)/applets/Kbuild.src; }
	@grep -q 'sigreturn_trampoline.o' $(BUSYBOX_SRC)/applets/Kbuild.src || { \
	    echo 'obj-y += sigreturn_trampoline.o' >> $(BUSYBOX_SRC)/applets/Kbuild.src; }
	$(MAKE) -C $(BUSYBOX_SRC) silentoldconfig CC=clang LD=clang 2>/dev/null || \
	yes "" | $(MAKE) -C $(BUSYBOX_SRC) oldconfig CC=clang LD=clang
	@# Fixup config for cross-compilation quirks (oldconfig may flip these)
	cd $(BUSYBOX_SRC) && sed -i \
	    -e 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' \
	    -e 's|^CONFIG_BUSYBOX_EXEC_PATH=.*|CONFIG_BUSYBOX_EXEC_PATH="/bin/busybox"|' \
	    .config
	@mkdir -p $(SYSROOT)/usr/lib
	@touch /tmp/stub.c && clang -c -x c /tmp/stub.c -o /tmp/stub.o 2>/dev/null
	@llvm-ar rcs $(SYSROOT)/usr/lib/libm.a /tmp/stub.o 2>/dev/null
	@llvm-ar rcs $(SYSROOT)/usr/lib/librt.a /tmp/stub.o 2>/dev/null
	@rm -f /tmp/stub.c /tmp/stub.o
	$(MAKE) -C $(BUSYBOX_SRC) CC=clang LD=clang
	@mv $(BUSYBOX_SRC)/applets/Kbuild.src.bak $(BUSYBOX_SRC)/applets/Kbuild.src 2>/dev/null || true

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
	$(MAKE) -C tools check-deps
	$(MAKE) -C tools
	tools/mkdisk disk.img \
	    --efi boot/uefi/BOOTX64.EFI \
	    --kernel kernel.bin \
	    --rootfs config/fsroot/

# ── Run / Debug ─────────────────────────────────────────

run: disk.img boot/uefi/OVMF.fd
	$(QEMU_BIN) -M q35 -smp 2 -pflash boot/uefi/OVMF.fd \
	  -drive file=disk.img,format=raw,if=none,id=disk \
	  -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
	  -m $(MEMORY) -display $(DISPLAY) -serial stdio -no-reboot

run-kvm: disk.img boot/uefi/OVMF.fd
	$(QEMU_BIN) -M q35 -smp 2 -pflash boot/uefi/OVMF.fd \
	  -accel kvm \
	  -drive file=disk.img,format=raw,if=none,id=disk \
	  -device ahci,id=ahci -device ide-hd,drive=disk,bus=ahci.0 \
	  -m $(MEMORY) -display $(DISPLAY) -serial stdio -no-reboot

debug: disk.img boot/uefi/OVMF.fd
	$(QEMU_BIN) -M q35 -smp 2 -pflash boot/uefi/OVMF.fd \
	  -S -s \
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
	$(MAKE) OS01_SYSTEST=1 disk.img boot/uefi/OVMF.fd
	python3 tests/run_test.py systest

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
