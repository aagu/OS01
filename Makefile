all: disk.img

boot/uefi/BOOTX64.EFI: boot/uefi/main.c
	make -C boot/uefi

boot/uefi/OVMF.fd:
	make -C boot/uefi OVMF.fd

kernel/kernel.bin:
	make -C kernel kernel.bin

disk.img: boot/uefi/BOOTX64.EFI kernel/kernel.bin
	dd if=/dev/zero of=$@ seek=0 bs=1M count=64
	mkfs.vfat -F 32 $@
	mmd -i $@ ::/EFI
	mmd -i $@ ::/EFI/BOOT
	mcopy -i $@ boot/uefi/BOOTX64.EFI ::/EFI/BOOT
	mcopy -i $@ kernel/kernel.bin ::/

.PHONY: run clean

run: disk.img boot/uefi/OVMF.fd
	qemu-system-x86_64 -pflash boot/uefi/OVMF.fd -hda disk.img

clean:
	rm -rf disk.img
	make -C boot/uefi clean
	make -C kernel clean