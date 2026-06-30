# ── Architecture detection ──────────────────────────────────
HOSTARCH   := x86_64
ARCHDIR    := arch/$(HOSTARCH)
# Build directory: all .o .d .elf .bin go here, separate from source
BUILD_DIR  := ../build/$(HOSTARCH)/kernel
DEST_DIR   := ..                  # root of the project (for kernel.bin)

# ── Load architecture configuration ────────────────────────
include $(ARCHDIR)/make.config

# ── Compiler / Linker (inherited from root Makefile exports) ─
CC       ?= clang -target x86_64-unknown-none
LD       ?= ld.lld -m elf_x86_64
AR       ?= llvm-ar
OBJ_CPY  ?= llvm-objcopy

# Kernel headers path (relative to kernel/ CWD)
KERNEL_HEADERS := include

# ── Source discovery via wildcard (no manual lists!) ───────
# All paths are relative to kernel/ (CWD after make -C kernel)
KERNEL_C_SOURCES := \
    $(wildcard kernel/*.c) \
    $(wildcard memory/*.c) \
    $(wildcard fs/*.c) \
    $(wildcard sched/*.c) \
    $(wildcard driver/*.c) \
    $(wildcard tty/*.c) \
    $(wildcard apic/*.c) \
    $(wildcard intr/*.c) \
    $(wildcard block/*.c) \
    $(wildcard timer/*.c) \
    $(wildcard percpu/*.c) \
    $(wildcard pic/*.c) \
    $(wildcard *.c)               # completion.c

# Architecture C sources and asm sources
ARCH_C_SOURCES  := $(wildcard $(ARCHDIR)/*.c)
ARCH_ASM_SOURCES := $(wildcard $(ARCHDIR)/*.S)
# Exclude trampoline.S from normal compilation (special handling below)
ARCH_ASM_SOURCES := $(filter-out $(ARCHDIR)/trampoline.S, $(ARCH_ASM_SOURCES))

ALL_C_SOURCES   := $(KERNEL_C_SOURCES) $(ARCH_C_SOURCES)
# kallsyms is a two-pass build artifact, not a regular source
ALL_C_SOURCES   := $(filter-out kernel/kallsyms.c, $(ALL_C_SOURCES))
ALL_S_SOURCES   := $(ARCH_ASM_SOURCES)

# ── Object files (under BUILD_DIR) ─────────────────────────
# Path mapping:  kernel/main.c    → build/x86_64/kernel/main.o
#                memory/pmm.c     → build/x86_64/memory/pmm.o
#                arch/x86_64/trap.c → build/x86_64/arch/x86_64/trap.o
C_OBJECTS       := $(patsubst %.c,$(BUILD_DIR)/%.o,$(ALL_C_SOURCES))
ASM_OBJECTS     := $(patsubst %.S,$(BUILD_DIR)/%.o,$(ALL_S_SOURCES))

EXTRA_OBJECTS := \
    $(BUILD_DIR)/kernel/font.o \
    $(BUILD_DIR)/$(ARCHDIR)/trampoline_bin.o

KERNEL_OBJECTS := $(C_OBJECTS) $(ASM_OBJECTS) $(EXTRA_OBJECTS)

# ── Flags ──────────────────────────────────────────────────
ALL_CFLAGS   := $(CFLAGS) $(ARCH_CFLAGS)   -I$(KERNEL_HEADERS)
ALL_ASMFLAGS := $(ASFLAGS) $(ARCH_ASMFLAGS)
ALL_LDFLAGS  := $(LDFLAGS) $(ARCH_LDFLAGS) -L$(SYSROOT)/usr/lib

LIBS := -nostdlib -lk -lgcc

ifeq ($(DEBUG),1)
ALL_CFLAGS += -DDEBUG=1
endif

# ── Targets ────────────────────────────────────────────────
.PHONY: all clean install-headers

all: $(BUILD_DIR)/kernel.bin
	cp $< $(CURDIR)/../kernel.bin

# ── Compilation rules ──────────────────────────────────────
# .c → .o + .d
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) -MMD -MP -c $< -o $@

# .S → .o + .d
$(BUILD_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) -MMD -MP -c $< -o $@

# ── Special objects ────────────────────────────────────────
# PSF font → .o
$(BUILD_DIR)/kernel/font.o: kernel/font.psf
	@mkdir -p $(dir $@)
	$(LD) -r -b binary -o $@ $<

# Trampoline: assemble → link at 0x8000 → binary → embed
$(BUILD_DIR)/$(ARCHDIR)/trampoline.o: $(ARCHDIR)/trampoline.S
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/$(ARCHDIR)/trampoline.elf: $(BUILD_DIR)/$(ARCHDIR)/trampoline.o $(ARCHDIR)/trampoline.ld
	$(LD) -T $(ARCHDIR)/trampoline.ld $< -o $@

$(BUILD_DIR)/$(ARCHDIR)/trampoline.bin: $(BUILD_DIR)/$(ARCHDIR)/trampoline.elf
	$(OBJ_CPY) -O binary $< $@

$(BUILD_DIR)/$(ARCHDIR)/trampoline_bin.o: $(BUILD_DIR)/$(ARCHDIR)/trampoline.bin
	cp $< $(ARCHDIR)/trampoline.bin
	$(LD) -r -b binary $(ARCHDIR)/trampoline.bin -o $@
	rm -f $(ARCHDIR)/trampoline.bin

# ── Temporary: irq.o avoids .bss section issue ─────────────
$(BUILD_DIR)/intr/irq.o: intr/irq.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CFLAGS) -MMD -MP -c $< -o $@

# ── Kallsyms (two-pass link) ───────────────────────────────
# Stage 1: link without kallsyms, just to get symbol table
$(BUILD_DIR)/kernel.elf.stage1: $(KERNEL_OBJECTS) $(ARCHDIR)/$(ARCH_LINKER)
	$(CC) $(ALL_LDFLAGS) -o $@ $(KERNEL_OBJECTS) -T $(ARCHDIR)/$(ARCH_LINKER) $(LIBS)

# Generate kallsyms object from stage1's nm output
$(BUILD_DIR)/kernel/kallsyms.o: kernel/kallsyms.c $(BUILD_DIR)/kernel.elf.stage1
	@mkdir -p $(dir $@)
	clang -o $(BUILD_DIR)/kernel/kallsyms $<
	nm -n $(BUILD_DIR)/kernel.elf.stage1 | $(BUILD_DIR)/kernel/kallsyms > $(BUILD_DIR)/kernel/kallsyms.S
	$(CC) $(ALL_CFLAGS) -c $(BUILD_DIR)/kernel/kallsyms.S -o $@
	rm -f $(BUILD_DIR)/kernel.elf.stage1

# ── Final link ──────────────────────────────────────────────
$(BUILD_DIR)/kernel.elf: $(KERNEL_OBJECTS) $(BUILD_DIR)/kernel/kallsyms.o $(ARCHDIR)/$(ARCH_LINKER)
	$(CC) $(ALL_LDFLAGS) -o $@ $(KERNEL_OBJECTS) $(BUILD_DIR)/kernel/kallsyms.o \
	    -T $(ARCHDIR)/$(ARCH_LINKER) $(LIBS)

$(BUILD_DIR)/kernel.bin: $(BUILD_DIR)/kernel.elf
	$(OBJ_CPY) -I elf64-x86-64 -S -R ".eh_frame" -R ".comment" \
	    -O binary $< $@

# Copy kernel.bin to project root via the "all" target above
.PHONY: kernel.bin
kernel.bin: all
	@true

# ── Install headers ────────────────────────────────────────
install-headers:
	mkdir -p $(SYSROOT)$(INCLUDEDIR)
	cp -R --preserve=timestamps include/. $(SYSROOT)$(INCLUDEDIR)/.

# ── Clean ───────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR)

# ── Auto-dependency tracking ───────────────────────────────
ALL_DEPS := $(patsubst %.o,%.d,$(C_OBJECTS))
-include $(ALL_DEPS)
