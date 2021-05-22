all: kernel.bin

KERNEL_SOURCES := \
	kernel/main.c

KERNEL_OBJECTS := $(KERNEL_SOURCES:.c=.o)

CC := $(HOME)/opt/cross/bin/x86_64-elf-gcc
LD := $(HOME)/opt/cross/bin/x86_64-elf-gcc
GAS := $(HOME)/opt/cross/bin/x86_64-elf-as

CFLAGS := -mcmodel=large -ffreestanding -mno-red-zone -Wall -Wextra
LDFLAGS := -mcmodel=large -ffreestanding -O2 -nostdlib -z muldefs
ASFLAGS :=
ifeq ($(DEBUG),1)
CFLAGS += -g
endif

.c.o:
	$(CC) $(CFLAGS) -c $< -o $@

kernel.elf: $(KERNEL_OBJECTS) kernel.ld
	$(LD) $(LDFLAGS) -o $@ $(KERNEL_OBJECTS) -T kernel.ld -lgcc

kernel.bin: kernel.elf
	objcopy -I elf64-x86-64 -S -R ".eh_frame" -R ".comment" -O binary kernel.elf kernel.bin

clean:
	rm -f */*.o kernel.bin kernel.elf *.txt