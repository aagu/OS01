##################################################
#		 Makefile
##################################################
BOOT:=boot.asm
BOOT_BIN:=$(subst .asm,.bin,$(BOOT))
KERNEL:=kernel.asm
KERNEL_BIN:=$(subst .asm,.bin,$(KERNEL))

ASM = nasm

IMG:=system.img

$(IMG) : $(BOOT_BIN) $(KERNEL_BIN)
	dd if=/dev/zero of=$(IMG) bs=512 count=2880
	dd if=$(BOOT_BIN) of=$(IMG) conv=notrunc
	dd if=$(KERNEL_BIN) of=$(IMG) seek=1 conv=notrunc

$(BOOT_BIN) : $(BOOT)
	nasm $< -o $@

$(KERNEL_BIN) : $(KERNEL)
	nasm $< -o $@

.PHONY:qemu
qemu:
	@echo '启动虚拟机...'
	qemu-system-x86_64  -boot order=a -fda $(IMG)