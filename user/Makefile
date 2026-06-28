CC = clang -target x86_64-unknown-none
LD = ld.lld -m elf_x86_64

SYSROOT = ../sysroot
CFLAGS = --sysroot=$(SYSROOT) \
         -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables \
         -fno-pic -fno-pie -mno-red-zone \
         -I$(SYSROOT)/usr/include \
         -Wall -Wextra -O2

LDFLAGS = -T linker.ld -static -nostdlib -no-pie \
          -L$(SYSROOT)/usr/lib -lc

PROGRAMS = init.elf spin.elf sh.elf sigtest.elf

.PHONY: all clean

all: $(PROGRAMS)

init.elf: crt0.o init.o linker.ld
	$(LD) $(LDFLAGS) -o $@ crt0.o init.o
	llvm-objcopy --strip-debug $@

spin.elf: crt0.o spin.o linker.ld
	$(LD) $(LDFLAGS) -o $@ crt0.o spin.o
	llvm-objcopy --strip-debug $@

sh.elf: crt0.o sh.o linker.ld
	$(LD) $(LDFLAGS) -o $@ crt0.o sh.o
	llvm-objcopy --strip-debug $@

sigtest.elf: crt0.o sigtest.o linker.ld
	$(LD) $(LDFLAGS) -o $@ crt0.o sigtest.o
	llvm-objcopy --strip-debug $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o *.elf
