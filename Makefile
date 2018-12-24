##################################################
#		 Makefile
##################################################
BOOT:=boot/boot.asm
BOOT_BIN:=$(subst .asm,.bin,$(BOOT))
BOOT_LST:=$(subst .asm,.lst,$(BOOT))
KERNEL:=kernel/kernel.asm
KERNEL_BIN:=$(subst .asm,.bin,$(KERNEL))
KERNEL_ELF:=$(subst .asm,.elf,$(KERNEL))
KERNEL_OBJECT:=$(subst .asm,.o,$(KERNEL))
C_SOURCES:=$(shell find . -name "*.c")
C_OBJECTS:=$(patsubst %.c,%.o,$(C_SOURCES))

ASM = nasm
CC = gcc
LD = ld
OBJCOPY = objcopy

C_FLAGS   = -c -Wall -m32 -nostdinc -fno-builtin -fno-stack-protector -I include
LD_FLAGS  = -T kernel.ld -m elf_i386
ASM_FLAGS = -felf

IMG:=system.img

$(IMG) : $(BOOT_BIN) $(C_OBJECTS) $(KERNEL_OBJECT) link
	dd if=/dev/zero of=$(IMG) bs=512 count=2880
	dd if=$(BOOT_BIN) of=$(IMG) conv=notrunc
	dd if=$(KERNEL_BIN) of=$(IMG) seek=1 conv=notrunc

$(BOOT_BIN) : $(BOOT)
	$(ASM) $< -o $@

$(KERNEL_OBJECT) : $(KERNEL)
	$(ASM) $(ASM_FLAGS) $< -o $@

.c.o :
	@echo $(C_SOURCES)
	$(CC) $(C_FLAGS) $< -o $@

link :
	@echo $(C_SOURCES)
	$(LD) $(LD_FLAGS) $(KERNEL_OBJECT) $(C_OBJECTS) -o $(KERNEL_ELF)
	$(OBJCOPY) -O binary -R .note -R .comment -S $(KERNEL_ELF) $(KERNEL_BIN)

$(BOOT_LST) : $(BOOT)
	$(ASM) $< -l $@

.PHONY:qemu
qemu:
	@echo '启动虚拟机...'
	qemu-system-x86_64  -boot order=a -fda $(IMG)

PHONY:debug
debug:
	qemu-system-x86_64 -d guest_errors -boot order=a -fda $(IMG)
.PHONY:clean
clean :
	rm -f $(BOOT_BIN) $(KERNEL_BIN) $(KERNEL_ELF) $(KERNEL_OBJECT) $(S_OBJECTS) \
	 $(C_OBJECTS) $(IMG) boot/boot.txt kernel/kernel.txt
dis:
	ndisasm ./boot/boot.bin > ./boot/boot.txt
	objdump -d -M intel ./kernel/kernel.elf > ./kernel/kernel.txt