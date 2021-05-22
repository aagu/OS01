all: disk.img

boot/uefi/BOOTX64.EFI:
	make -C boot/uefi

boot/uefi/OVMF.fd:
	make -C boot/uefi OVMF.fd

disk.img: boot/uefi/BOOTX64.EFI
	dd if=/dev/zero of=$@ seek=0 bs=1M count=64
	mkfs.vfat -F 32 $@
	mmd -i $@ ::/EFI
	mmd -i $@ ::/EFI/BOOT
	mcopy -i $@ $< ::/EFI/BOOT

.PHONY: run
run: disk.img boot/uefi/OVMF.fd
	qemu-system-x86_64 -pflash boot/uefi/OVMF.fd -hda disk.img

.PHONY: clean
clean:
	rm -rf disk.img
	make -C boot/uefi clean