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

all: disk.img

boot/uefi/BOOTX64.EFI: boot/uefi/main.c
	make -C boot/uefi

boot/uefi/OVMF.fd:
	make -C boot/uefi OVMF.fd

.PHONY: lib
lib:
	make -C kernel install-headers
	make -C libc install

.PHONY: kernel/kernel.bin
kernel/kernel.bin: lib
	make -C kernel kernel.bin

disk.img: boot/uefi/BOOTX64.EFI lib kernel/kernel.bin
	dd if=/dev/zero of=$@ seek=0 bs=1M count=64
	mkfs.vfat -F 32 $@
	mmd -i $@ ::/EFI
	mmd -i $@ ::/EFI/BOOT
	mcopy -i $@ boot/uefi/BOOTX64.EFI ::/EFI/BOOT
	mcopy -i $@ kernel/kernel.bin ::/
ifneq (,$(wildcard config/config.txt))
	mcopy -i $@ config/config.txt ::/
endif

.PHONY: run clean debug

run: disk.img boot/uefi/OVMF.fd
	$(QEMU_BIN) -pflash boot/uefi/OVMF.fd -hda disk.img -m 2G -serial stdio

debug: disk.img boot/uefi/OVMF.fd
	$(QEMU_BIN) -pflash boot/uefi/OVMF.fd -S -s -hda disk.img -m 2G -serial stdio

clean:
	rm -rf disk.img
	make -C boot/uefi distclean
	make -C kernel clean
	make -C libc clean
	rm -rf sysroot
