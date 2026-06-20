base:=$(shell pwd)
export CC=clang -target x86_64-unknown-none
export LD=ld.lld -m elf_x86_64
export AS=llvm-as
export AR=llvm-ar
export OBJ_CPY=llvm-objcopy
export OBJ_DUMP=llvm-objdump
ifneq ($(shell uname -m),aarch64)
QEMU_BIN=qemu-system-x86_64
else
export CROSS_BASE=$(base)/toolchain/cross
QEMU_BIN=$(CROSS_BASE)/bin/qemu-system-x86_64
endif

export PREFIX=/usr
export EXEC_PREFIX=${PREFIX}
export BOOTDIR=/boot
export LIBDIR=${EXEC_PREFIX}/lib
export INCLUDEDIR=${PREFIX}/include

export SYSROOT=$(base)/sysroot

export CFLAGS=--sysroot=${SYSROOT} -isystem=${INCLUDEDIR} -g -fno-stack-protector
export LDFLAGS=--sysroot=${SYSROOT}

DISPLAY=gtk
MEMORY=512M
QEMU_MONITOR = 127.0.0.1:4444

all: disk.img

boot/uefi/BOOTX64.EFI: boot/uefi/main.c
	make -C boot/uefi

boot/uefi/OVMF.fd:
	make -C boot/uefi OVMF.fd

.PHONY: lib user
lib:
	make -C kernel install-headers
	make -C libc install

user:
	make -C user

.PHONY: test
test:
	make -C test run

.PHONY: kernel/kernel.bin
kernel/kernel.bin: lib
	make -C kernel kernel.bin

disk.img: boot/uefi/BOOTX64.EFI lib kernel/kernel.bin user user/busybox.elf
	dd if=/dev/zero of=$@ seek=0 bs=1M count=64
	mkfs.vfat -F 32 $@
	mmd -i $@ ::/EFI
	mmd -i $@ ::/EFI/BOOT
	mcopy -i $@ boot/uefi/BOOTX64.EFI ::/EFI/BOOT
	mcopy -i $@ kernel/kernel.bin ::/
	mcopy -i $@ user/init.elf ::/init.elf
	mcopy -i $@ user/spin.elf ::/spin.elf
	mcopy -i $@ user/busybox.elf ::/busybox.elf
ifneq (,$(wildcard config/config.txt))
	mcopy -i $@ config/config.txt ::/
endif

.PHONY: run clean debug screenshot

user/busybox.elf: thirdpart/busybox-1.36.1/busybox
	cp $< $@

# ── BusyBox build from submodule ───────────────────────────
# Submodule must be initialized: git submodule update --init
BUSYBOX_SRC = thirdpart/busybox-1.36.1
BUSYBOX_CONFIG = config/busybox.config

# Rebuild on kernel/libc changes (CFLAGS/LDFLAGS reference OS01 paths)
thirdpart/busybox-1.36.1/busybox: lib \
    $(BUSYBOX_SRC)/Makefile \
    $(BUSYBOX_CONFIG) \
    user/crt0.S
	@# Verify submodule is initialized
	@test -f $(BUSYBOX_SRC)/Makefile || (echo "ERROR: busybox submodule not initialized" && echo "Run: git submodule update --init" && false)
	@# Inject OS01 crt0.S and Kbuild rule
	cp user/crt0.S $(BUSYBOX_SRC)/applets/crt0.S
	cp $(BUSYBOX_CONFIG) $(BUSYBOX_SRC)/.config
	if ! grep -q 'crt0.o' $(BUSYBOX_SRC)/applets/Kbuild.src; then \
	    cp $(BUSYBOX_SRC)/applets/Kbuild.src $(BUSYBOX_SRC)/applets/Kbuild.src.bak; \
	    echo 'obj-y += crt0.o' >> $(BUSYBOX_SRC)/applets/Kbuild.src; \
	fi
	yes "" | $(MAKE) -C $(BUSYBOX_SRC) oldconfig CC=clang LD=clang
	# Ensure critical settings survive oldconfig
	cd $(BUSYBOX_SRC) && sed -i \
	    -e 's/^# CONFIG_STATIC is not set/CONFIG_STATIC=y/' \
	    -e 's/^# CONFIG_ASH is not set/CONFIG_ASH=y/' \
	    -e 's/^# CONFIG_SH_IS_ASH is not set/CONFIG_SH_IS_ASH=y/' \
	    -e 's/^# CONFIG_SHELL_ASH is not set/CONFIG_SHELL_ASH=y/' \
	    -e 's/^# CONFIG_FEATURE_SH_STANDALONE is not set/CONFIG_FEATURE_SH_STANDALONE=y/' \
	    -e 's/^# CONFIG_FEATURE_PREFER_APPLETS is not set/CONFIG_FEATURE_PREFER_APPLETS=y/' \
	    -e 's/^CONFIG_GETFATTR=y/# CONFIG_GETFATTR is not set/' \
	    -e 's/^CONFIG_FEATURE_IP_LINK_CAN=y/# CONFIG_FEATURE_IP_LINK_CAN is not set/' \
	    -e 's/^# CONFIG_LS is not set/CONFIG_LS=y/' \
	    -e 's/^# CONFIG_CAT is not set/CONFIG_CAT=y/' \
	    -e 's/^# CONFIG_ECHO is not set/CONFIG_ECHO=y/' \
	    -e 's/^# CONFIG_PWD is not set/CONFIG_PWD=y/' \
	    -e 's/^# CONFIG_RM is not set/CONFIG_RM=y/' \
	    -e 's/^# CONFIG_CP is not set/CONFIG_CP=y/' \
	    -e 's/^# CONFIG_MV is not set/CONFIG_MV=y/' \
	    -e 's/^# CONFIG_MKDIR is not set/CONFIG_MKDIR=y/' \
	    -e 's/^# CONFIG_RMDIR is not set/CONFIG_RMDIR=y/' \
	    .config
	@# Create stub libraries for busybox link detection (trylink tests -lm -lrt)
	@mkdir -p $(SYSROOT)/usr/lib
	@touch empty.c
	@clang -c -x c empty.c -o empty.o 2>/dev/null || true
	@llvm-ar rcs $(SYSROOT)/usr/lib/libm.a empty.o 2>/dev/null
	@llvm-ar rcs $(SYSROOT)/usr/lib/librt.a empty.o 2>/dev/null
	@rm -f empty.c empty.o
	$(MAKE) -C $(BUSYBOX_SRC) CC=clang LD=clang
	@# Restore Kbuild.src
	if [ -f $(BUSYBOX_SRC)/applets/Kbuild.src.bak ]; then \
	    mv $(BUSYBOX_SRC)/applets/Kbuild.src.bak $(BUSYBOX_SRC)/applets/Kbuild.src; \
	fi

run: disk.img boot/uefi/OVMF.fd
	$(QEMU_BIN) -M q35 -smp 2 -pflash boot/uefi/OVMF.fd -hda disk.img -m $(MEMORY) -display $(DISPLAY) -serial stdio -monitor tcp:$(QEMU_MONITOR),server,nowait

debug: disk.img boot/uefi/OVMF.fd
	$(QEMU_BIN) -M q35 -smp 2 -pflash boot/uefi/OVMF.fd -S -s -hda disk.img -m $(MEMORY) -display $(DISPLAY) -serial stdio -monitor tcp:$(QEMU_MONITOR),server,nowait

# Take a screenshot of the QEMU framebuffer.
# Requires: QEMU running with -monitor tcp (automatic with make run/debug)
#           ImageMagick (convert) for PPM→PNG conversion
# Usage: make screenshot
# Output: /tmp/os01_screen.png
screenshot:
	@exec 3<>/dev/tcp/127.0.0.1/4444 && \
	 echo "screendump /tmp/os01_screen.ppm" >&3 && exec 3>&- || \
	 { echo "ERROR: QEMU not running or monitor port not reachable"; false; }
	@sleep 0.5
	@magick /tmp/os01_screen.ppm /tmp/os01_screen.png 2>/dev/null || \
	 convert /tmp/os01_screen.ppm /tmp/os01_screen.png 2>/dev/null || \
	 { echo "ERROR: ImageMagick not found, install imagemagick"; false; }
	@echo "Screenshot saved to /tmp/os01_screen.png"

# ── Test harness ──────────────────────────────────────────
# Each test phase runs QEMU with serial pipe and verifies output.
# Usage: make test-phase-0, make test-phase-1, etc.
#
# Environment variables:
#   QEMU           path to qemu (default: qemu-system-x86_64)
#   TEST_TIMEOUT   timeout in seconds (default: 30)
#   DISK_IMG       disk image to test (default: disk.img)

.PHONY: test-phase-0 test-phase-1 test-phase-2 test-phase-3 test-phase-4 test-phase-5 test-phase-6

test-phase-0: disk.img
	python3 tests/run_test.py phase-0

test-phase-1: disk.img
	python3 tests/run_test.py phase-1

test-phase-2: disk.img
	python3 tests/run_test.py phase-2

test-phase-3: disk.img
	python3 tests/run_test.py phase-3

test-phase-4: disk.img
	python3 tests/run_test.py phase-4

test-phase-5: disk.img
	python3 tests/run_test.py phase-5

test-phase-6: disk.img
	python3 tests/run_test.py phase-6

clean:
	rm -rf disk.img
	make -C boot/uefi distclean
	make -C kernel clean
	make -C libc clean
	make -C user clean
	rm -rf test/build
	rm -rf sysroot
	if [ -f $(BUSYBOX_SRC)/Makefile ]; then \
	    $(MAKE) -C $(BUSYBOX_SRC) clean 2>/dev/null || true; \
	    rm -f $(BUSYBOX_SRC)/applets/crt0.S $(BUSYBOX_SRC)/applets/Kbuild.src.bak; \
	fi
