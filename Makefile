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

all: disk.img

# ── Bootloader ──────────────────────────────────────────

boot/uefi/BOOTX64.EFI: boot/uefi/main.c
	make -C boot/uefi CFLAGS="-nostdlibinc -ffreestanding -fshort-wchar -fno-strict-aliasing -fno-stack-protector -fno-stack-check -I. -I./uefi -Dx86_64 -DHAVE_USE_MS_ABI -mno-red-zone -maccumulate-outgoing-args -Wno-builtin-declaration-mismatch -fpic -fPIC" USE_GCC=

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

# ── User programs ───────────────────────────────────────

.PHONY: user
user:
	make -C user

user/busybox.elf: thirdpart/busybox-1.36.1/busybox
	cp $< $@

# ── BusyBox ─────────────────────────────────────────────
# Submodule must be initialized: git submodule update --init

BUSYBOX_SRC  = thirdpart/busybox-1.36.1
BUSYBOX_CFG  = config/busybox.config

thirdpart/busybox-1.36.1/busybox: lib $(BUSYBOX_SRC)/Makefile $(BUSYBOX_CFG) user/crt0.S
	@test -f $(BUSYBOX_SRC)/Makefile || { \
	    echo "ERROR: busybox submodule not initialized"; \
	    echo "Run: git submodule update --init"; false; }
	cp user/crt0.S $(BUSYBOX_SRC)/applets/crt0.S
	cp $(BUSYBOX_CFG) $(BUSYBOX_SRC)/.config
	@grep -q 'crt0.o' $(BUSYBOX_SRC)/applets/Kbuild.src || { \
	    cp $(BUSYBOX_SRC)/applets/Kbuild.src $(BUSYBOX_SRC)/applets/Kbuild.src.bak; \
	    echo 'obj-y += crt0.o' >> $(BUSYBOX_SRC)/applets/Kbuild.src; }
	yes "" | $(MAKE) -C $(BUSYBOX_SRC) oldconfig CC=clang LD=clang
	@# Fixup config for cross-compilation quirks (oldconfig may flip these)
	cd $(BUSYBOX_SRC) && sed -i \
	    -e 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' \
	    -e 's|^CONFIG_BUSYBOX_EXEC_PATH=.*|CONFIG_BUSYBOX_EXEC_PATH="/busybox.elf"|' \
	    .config
	@mkdir -p $(SYSROOT)/usr/lib
	@touch /tmp/stub.c && clang -c -x c /tmp/stub.c -o /tmp/stub.o 2>/dev/null
	@llvm-ar rcs $(SYSROOT)/usr/lib/libm.a /tmp/stub.o 2>/dev/null
	@llvm-ar rcs $(SYSROOT)/usr/lib/librt.a /tmp/stub.o 2>/dev/null
	@rm -f /tmp/stub.c /tmp/stub.o
	$(MAKE) -C $(BUSYBOX_SRC) CC=clang LD=clang
	@mv $(BUSYBOX_SRC)/applets/Kbuild.src.bak $(BUSYBOX_SRC)/applets/Kbuild.src 2>/dev/null || true

# ── Disk image ──────────────────────────────────────────

disk.img: boot/uefi/BOOTX64.EFI lib kernel/kernel.bin user user/busybox.elf
	dd if=/dev/zero of=$@ bs=1M count=64
	mkfs.vfat -F 32 $@
	mmd -i $@ ::/EFI
	mmd -i $@ ::/EFI/BOOT
	mcopy -i $@ boot/uefi/BOOTX64.EFI ::/EFI/BOOT
	mcopy -i $@ kernel/kernel.bin ::/
	mcopy -i $@ user/init.elf ::/init.elf
	mcopy -i $@ user/spin.elf ::/spin.elf
	mcopy -i $@ user/sigtest.elf ::/sigtest.elf
	mcopy -i $@ user/poweroff.elf ::/poweroff.elf
	mcopy -i $@ user/busybox.elf ::/busybox.elf
ifneq (,$(wildcard config/config.txt))
	mcopy -i $@ config/config.txt ::/
endif

# ── Run / Debug ─────────────────────────────────────────

run: disk.img boot/uefi/OVMF.fd
	$(QEMU_BIN) -M q35 -smp 2 -pflash boot/uefi/OVMF.fd \
	  -hda disk.img -m $(MEMORY) -display $(DISPLAY) -serial stdio

debug: disk.img boot/uefi/OVMF.fd
	$(QEMU_BIN) -M q35 -smp 2 -pflash boot/uefi/OVMF.fd \
	  -S -s -hda disk.img -m $(MEMORY) -display $(DISPLAY) -serial stdio

# ── Test ────────────────────────────────────────────────

.PHONY: test
test:
	make -C test run

.PHONY: test-phase-0
test-phase-0: disk.img
	python3 tests/run_test.py phase-0

# ── Clean ───────────────────────────────────────────────

.PHONY: clean
clean:
	rm -rf disk.img
	make -C boot/uefi distclean
	make -C kernel clean
	make -C libc clean
	make -C user clean
	rm -rf test/build sysroot
	if [ -f $(BUSYBOX_SRC)/Makefile ]; then \
	    $(MAKE) -C $(BUSYBOX_SRC) clean 2>/dev/null || true; \
	    rm -f $(BUSYBOX_SRC)/applets/crt0.S $(BUSYBOX_SRC)/applets/Kbuild.src.bak; \
	fi
