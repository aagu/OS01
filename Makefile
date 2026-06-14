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

export CFLAGS=--sysroot=${SYSROOT} -isystem=${INCLUDEDIR} -g
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

.PHONY: kernel/kernel.bin
kernel/kernel.bin: lib
	make -C kernel kernel.bin

disk.img: boot/uefi/BOOTX64.EFI lib kernel/kernel.bin user
	dd if=/dev/zero of=$@ seek=0 bs=1M count=64
	mkfs.vfat -F 32 $@
	mmd -i $@ ::/EFI
	mmd -i $@ ::/EFI/BOOT
	mcopy -i $@ boot/uefi/BOOTX64.EFI ::/EFI/BOOT
	mcopy -i $@ kernel/kernel.bin ::/
	mcopy -i $@ user/init.elf ::/init.elf
ifneq (,$(wildcard config/config.txt))
	mcopy -i $@ config/config.txt ::/
endif

.PHONY: run clean debug screenshot

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

clean:
	rm -rf disk.img
	make -C boot/uefi distclean
	make -C kernel clean
	make -C libc clean
	make -C user clean
	rm -rf sysroot
