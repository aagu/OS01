##################################################
#		 Makefile
##################################################
BOOT:=boot.asm
BOOT_BIN:=$(subst .asm,.bin,$(BOOT))
KERNEL:=kernel.asm
KERNEL_BIN:=$(subst .asm,.bin,$(KERNEL))
KERNEL_ELF:=$(subst .asm,.elf,$(KERNEL))
C_SOURCES:=start.c
C_OBJECTS:=start.o

ASM = nasm
CC = gcc
LD = ld

C_FLAGS   = -c -Wall -m32 -ggdb -gstabs+ -nostdinc -fno-builtin -fno-stack-protector -I include
LD_FLAGS  = -T kernel.ld -m elf_i386
ASM_FLAGS = -felf -g -F stabs

IMG:=system.img

$(IMG) : $(BOOT_BIN) $(C_OBJECTS) link
	dd if=/dev/zero of=$(IMG) bs=512 count=2880
	dd if=$(BOOT_BIN) of=$(IMG) conv=notrunc
	dd if=$(KERNEL_BIN) of=$(IMG) seek=1 conv=notrunc

$(BOOT_BIN) : $(BOOT)
	$(ASM) $< -o $@

$(KERNEL_ELF) : $(KERNEL)
	$(ASM) $(ASM_FLAGS) $< -o $@

$(C_OBJECTS) : $(C_SOURCES)
	$(CC) $(C_FLAGS) $< -o $@

link :
	@echo $(S_SOURCES)
	$(LD) $(LD_FLAGS) $(C_OBJECTS) $(KERNEL_ELF) -o $(KERNEL_BIN)

.PHONY:qemu
qemu:
	@echo '启动虚拟机...'
	qemu-system-x86_64  -boot order=a -fda $(IMG)

PHONY:debug
debug:
	qemu-system-x86_64 -d guest_errors -boot order=a -fda $(IMG)