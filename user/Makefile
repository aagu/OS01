# ── Configuration ──────────────────────────────────────────
BUILD_DIR := ../build/x86_64/user
OBJ_DIR   := $(BUILD_DIR)/obj

# ── Compiler / Linker ──────────────────────────────────────
CC       ?= clang -target x86_64-unknown-none
LD       ?= ld.lld -m elf_x86_64
OBJ_CPY  ?= llvm-objcopy

SYSROOT  := ../sysroot
CFLAGS   := --sysroot=$(SYSROOT) \
            -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables \
            -fno-pic -fno-pie -mno-red-zone \
            -I$(SYSROOT)/usr/include \
            -Wall -Wextra -O2

LDFLAGS  := -T linker.ld -static -nostdlib -no-pie \
            -L$(SYSROOT)/usr/lib -lc

# ── Source discovery ───────────────────────────────────────
# crt0.S is startup code, not a program
C_SOURCES := $(wildcard *.c)
ASM_SOURCES := $(wildcard *.S)

# Each .c → .elf (e.g. init.c → init.elf)
PROGRAMS := $(patsubst %.c,$(BUILD_DIR)/%.elf,$(C_SOURCES))

# ── Targets ────────────────────────────────────────────────
.PHONY: all clean

# Build all programs, then copy .elf files back to source dir
all: $(PROGRAMS)
	cp $(BUILD_DIR)/*.elf .

# ── Compile rules (into build dir) ─────────────────────────
$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# ── crt0 startup (compiled once, linked into every program) ─
CRT0_OBJ := $(OBJ_DIR)/crt0.o

$(CRT0_OBJ): crt0.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# ── Link rules (pattern: each .c → .elf linked with crt0) ─
# init.c → $(OBJ_DIR)/init.o → $(BUILD_DIR)/init.elf
$(BUILD_DIR)/%.elf: $(OBJ_DIR)/%.o $(CRT0_OBJ) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(CRT0_OBJ) $<
	$(OBJ_CPY) --strip-debug $@

# ── Clean ──────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR) *.elf

# ── Dependency tracking ────────────────────────────────────
C_DEPS := $(patsubst %.c,$(OBJ_DIR)/%.d,$(C_SOURCES))
ASM_DEPS := $(patsubst %.S,$(OBJ_DIR)/%.d,$(ASM_SOURCES))
-include $(C_DEPS) $(ASM_DEPS)
