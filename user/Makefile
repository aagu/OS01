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

# ── Syscall test mode ──────────────────────────────────────
# When OS01_SYSTEST=1, init.c spawns /systest.elf instead of /busybox.elf sh
ifeq ($(OS01_SYSTEST),1)
override CFLAGS += -DOS01_SYSTEST
endif

LDFLAGS  := -T linker.ld -static -nostdlib -no-pie \
            -L$(SYSROOT)/usr/lib

# ── Source discovery ───────────────────────────────────────
# crt0.S is startup code, not a program
# terminal_core.c is a library for terminal.elf, not a standalone program
# tetris_logic.c / terminal_core.c are libraries, not standalone programs
C_SOURCES := $(filter-out terminal_core.c tetris_logic.c,$(wildcard *.c))
ASM_SOURCES := $(wildcard *.S)

# Each .c → .elf (e.g. init.c → init.elf)
PROGRAMS := $(patsubst %.c,$(BUILD_DIR)/%.elf,$(C_SOURCES))

# ── Targets ────────────────────────────────────────────────
.PHONY: all clean

# Build all programs into $(BUILD_DIR); the root Makefile packages
# them from there into config/fsroot/bin/ — do NOT copy back to the
# source dir (that would pollute user/ with build artifacts).
all: $(PROGRAMS)

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

# ── sigreturn trampoline (compiled once, linked into every program) ─
SIGRETURN_OBJ := $(OBJ_DIR)/sigreturn_trampoline.o

$(SIGRETURN_OBJ): sigreturn_trampoline.S
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

# ── Link rules (pattern: each .c → .elf linked with crt0) ─
# init.c → $(OBJ_DIR)/init.o → $(BUILD_DIR)/init.elf
$(BUILD_DIR)/%.elf: $(OBJ_DIR)/%.o $(CRT0_OBJ) $(SIGRETURN_OBJ) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(CRT0_OBJ) $(SIGRETURN_OBJ) $< -lc
	$(OBJ_CPY) --strip-debug $@

# ══════════════════════════════════════════════════════════════
#  terminal.elf — needs embedded font (override pattern rule)
# ══════════════════════════════════════════════════════════════
TERMINAL_FONT_OBJ := $(OBJ_DIR)/terminal_font.o

$(BUILD_DIR)/terminal.elf: $(OBJ_DIR)/terminal.o $(OBJ_DIR)/terminal_core.o $(CRT0_OBJ) $(SIGRETURN_OBJ) $(TERMINAL_FONT_OBJ) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(CRT0_OBJ) $(SIGRETURN_OBJ) $(OBJ_DIR)/terminal.o $(OBJ_DIR)/terminal_core.o $(TERMINAL_FONT_OBJ) -lc
	$(OBJ_CPY) --strip-debug $@

$(TERMINAL_FONT_OBJ): terminal_font.psf
	@mkdir -p $(dir $@)
	$(OBJ_CPY) -B i386 -I binary -O elf64-x86-64 terminal_font.psf $@

# ══════════════════════════════════════════════════════════════
#  tetris.elf — needs tetris_logic.o (override pattern rule)
# ══════════════════════════════════════════════════════════════
$(BUILD_DIR)/tetris.elf: $(OBJ_DIR)/tetris.o $(OBJ_DIR)/tetris_logic.o $(CRT0_OBJ) $(SIGRETURN_OBJ) linker.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -o $@ $(CRT0_OBJ) $(SIGRETURN_OBJ) $(OBJ_DIR)/tetris.o $(OBJ_DIR)/tetris_logic.o -lc
	$(OBJ_CPY) --strip-debug $@

# ── Clean ──────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR) *.elf

# ── Dependency tracking ────────────────────────────────────
C_DEPS := $(patsubst %.c,$(OBJ_DIR)/%.d,$(C_SOURCES))
ASM_DEPS := $(patsubst %.S,$(OBJ_DIR)/%.d,$(ASM_SOURCES))
-include $(C_DEPS) $(ASM_DEPS)
