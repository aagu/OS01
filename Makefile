base:=$(shell pwd)
export CC=clang -target x86_64-unknown-elf
export LD=ld.lld -m elf_x86_64
export AS=as
export AR=ar
export OBJ_CPY=objcopy
export OBJ_DUMP=objdump

export PREFIX=/usr
export EXEC_PREFIX=${PREFIX}
export BOOTDIR=/boot
export LIBDIR=${EXEC_PREFIX}/lib
export INCLUDEDIR=${PREFIX}/include

export SYSROOT=$(base)/sysroot

export CFLAGS=--sysroot=${SYSROOT} -isystem=${INCLUDEDIR} -g

all: disk.img

boot/uefi/BOOTX64.EFI: boot/uefi/main.c
	make -C boot/uefi

boot/uefi/OVMF.fd:
	make -C boot/uefi OVMF.fd

.PHONY: kernel/kernel.bin
kernel/kernel.bin:
	make -C kernel kernel.bin

lib:
	make -C kernel install-headers
	make -C libc install

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
	qemu-system-x86_64 -pflash boot/uefi/OVMF.fd -hda disk.img -m 2G -serial stdio

debug: disk.img boot/uefi/OVMF.fd
	qemu-system-x86_64 -pflash boot/uefi/OVMF.fd -S -s -hda disk.img -m 2G -serial stdio

clean:
	rm -rf disk.img
	make -C boot/uefi distclean
	make -C kernel clean
	make -C libc clean
	rm -rf sysroot