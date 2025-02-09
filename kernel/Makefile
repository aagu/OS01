HOSTARCH:=$(shell ../target-triplet-to-arch.sh $(shell ../default-host.sh))

ARCHDIR=arch/$(HOSTARCH)

include $(ARCHDIR)/make.config

DESTDIR=$(SYSROOT)

KERNEL_SOURCES := \
	kernel/main.c \
	intr/irq.c \
	intr/softirq.c \
	kernel/printk.c \
	kernel/panic.c \
	kernel/trace.c \
	memory/pmm.c \
	memory/vmm.c \
	memory/slab.c \
	memory/dump.c \
	pic/8259A.c \
	timer/timer.c

KERNEL_SOURCES += $(wildcard driver/*.c)

KERNEL_OBJECTS := \
	$(KERNEL_SOURCES:.c=.o) \
	$(KERNEL_ARCH_OBJECTS)

KERNEL_HEADERS := include

CFLAGS := $(CFLAGS) -mcmodel=kernel -ffreestanding -mno-red-zone -Wall -Wextra -mno-80387 -mno-sse -mno-sse2 -mno-3dnow -mno-mmx -fpie
LDFLAGS := $(CFLAGS) -mcmodel=kernel -ffreestanding -mno-red-zone -Wall -Wextra -mno-80387 -mno-sse -mno-sse2 -mno-3dnow -mno-mmx -fpie -z muldefs -L${SYSROOT}/usr/lib
ASFLAGS :=
ifeq ($(DEBUG),1)
CFLAGS += -DDEBUG=1
endif
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

# temporary solution to avoid interrupt code asigned to .bss
intr/irq.o: intr/irq.c
	$(CC) $(CFLAGS) -I$(KERNEL_HEADERS) -c $< -o $@

kernel.elf: $(KERNEL_OBJECTS) $(ARCHDIR)/linker.ld
	$(CC) $(LDFLAGS) -o $@ $(KERNEL_OBJECTS) -T $(ARCHDIR)/linker.ld $(LIBS)

kernel/kallsyms.o: kernel/kallsyms.c kernel.elf
	clang -o kernel/kallsyms $<
	nm -n kernel.elf | ./kernel/kallsyms > kernel/kallsyms.S
	$(CC) $(CFLAGS) -c kernel/kallsyms.S -o $@

kernel.bin: kernel.elf kernel/kallsyms.o
	$(CC) $(LDFLAGS) -o kernel.elf $(KERNEL_OBJECTS) kernel/kallsyms.o -T $(ARCHDIR)/linker.ld $(LIBS)
	$(OBJ_CPY) -I elf64-x86-64 -S -R ".eh_frame" -R ".comment" -O binary kernel.elf kernel.bin

clean:
	rm -f */*.o kernel.bin kernel.elf *.txt
	rm -f $(ARCHDIR)/*.o
	rm -f kernel/kallsyms

install-headers:
	mkdir -p $(DESTDIR)$(INCLUDEDIR)
	cp -R --preserve=timestamps include/. $(DESTDIR)$(INCLUDEDIR)/.