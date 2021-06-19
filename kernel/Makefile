HOSTARCH:=$(shell ../target-triplet-to-arch.sh $(shell ../default-host.sh))

ARCHDIR=arch/$(HOSTARCH)

include $(ARCHDIR)/make.config

DESTDIR=$(SYSROOT)

KERNEL_SOURCES := \
	kernel/main.c \
	kernel/printk.c \
	kernel/panic.c \
	memory/pmm.c \
	memory/vmm.c \
	memory/slab.c \
	memory/dump.c

KERNEL_OBJECTS := \
	$(KERNEL_SOURCES:.c=.o) \
	$(KERNEL_ARCH_OBJECTS)

KERNEL_HEADERS := include

CFLAGS := $(CFLAGS) -mcmodel=large -ffreestanding -mno-red-zone -Wall -Wextra
LDFLAGS := $(CFLAGS) -mcmodel=large -ffreestanding -O2 -z muldefs
ASFLAGS :=
# ifeq ($(DEBUG),1)
# CFLAGS += -g
# endif
LIBS := $(LIBS) -nostdlib -lk -lgcc
LIBS := $(LIBS) $(KERNEL_ARXH_LIBS)

KERNEL_OBJECTS+=kernel/font.o

all: kernel.bin

.c.o:
	$(CC) $(CFLAGS) -I$(KERNEL_HEADERS) -c $< -o $@

.S.o:
	$(CC) $(CFLAGS) -c $< -o $@

kernel/font.o: kernel/font.psf
	$(LD) -r -b binary -o $@ $<

kernel.elf: $(KERNEL_OBJECTS) $(ARCHDIR)/linker.ld
	$(CC) $(LDFLAGS) -o $@ $(KERNEL_OBJECTS) -T $(ARCHDIR)/linker.ld $(LIBS)

kernel.bin: kernel.elf
	objcopy -I elf64-x86-64 -S -R ".eh_frame" -R ".comment" -O binary kernel.elf kernel.bin

clean:
	rm -f */*.o kernel.bin kernel.elf *.txt
	rm -f $(ARCHDIR)/*.o

install-headers:
	mkdir -p $(DESTDIR)$(INCLUDEDIR)
	cp -R --preserve=timestamps include/. $(DESTDIR)$(INCLUDEDIR)/.