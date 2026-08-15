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
	@cp $(INITTAB_FILE) config/fsroot/etc/inittab
	@cp build/x86_64/user/socktest.elf      config/fsroot/bin/socktest
	@cp build/x86_64/user/udptest.elf       config/fsroot/bin/udptest
	@cp build/x86_64/user/ipaddr.elf        config/fsroot/bin/ipaddr
	@cp build/x86_64/user/nettest.elf       config/fsroot/bin/nettest
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
