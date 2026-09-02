# ── Configuration ──────────────────────────────────────────
ARCH ?= x86_64
LOCAL_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))

# ── Profile mode (root os01_submake) vs standalone legacy ──
# With OS01_PROFILE_FILE set, include the selected profile instead of the
# legacy toolchain.mk and take the build dir from the profile
# (USER_BUILD_DIR). The two paths are mutually exclusive.
ifdef OS01_PROFILE_FILE
include $(OS01_PROFILE_FILE)
BUILD_DIR := $(USER_BUILD_DIR)
# Profile mode consumes the immutable generation the root artifact rule
# (mk/components/user.mk) leased and passed as SYSROOT_GENERATION_DIR — never
# the $(SYSROOT) symlink.
SYSROOT_DIR   := $(SYSROOT_GENERATION_DIR)
TARGET_INCDIR := $(SYSROOT_GENERATION_DIR)/usr/include
TARGET_LIBDIR_LOCAL := $(SYSROOT_GENERATION_DIR)/usr/lib
else
BUILD_DIR := ../build/x86_64/user
ifeq ($(ARCH),x86_64)
include $(dir $(LOCAL_MAKEFILE))../toolchain.mk
endif
# Legacy standalone mode: unchanged — links against the published $(SYSROOT).
SYSROOT_DIR   := $(SYSROOT)
TARGET_INCDIR := $(TARGET_INCLUDEDIR)
TARGET_LIBDIR_LOCAL := $(TARGET_LIBDIR)
endif

OBJ_DIR   := $(BUILD_DIR)/obj

CFLAGS   := --sysroot=$(SYSROOT_DIR) \
            -ffreestanding -fno-stack-protector -fno-asynchronous-unwind-tables \
            -fno-pic -fno-pie -mno-red-zone \
            -I$(TARGET_INCDIR) \
            -Wall -Wextra -O2

# ── Syscall test mode ──────────────────────────────────────
# When OS01_SYSTEST=1, init.c spawns /systest.elf instead of /busybox.elf sh
ifeq ($(OS01_SYSTEST),1)
override CFLAGS += -DOS01_SYSTEST
endif

USER_RAW_LDFLAGS := -m elf_x86_64 -static -no-pie -T linker.ld -L$(TARGET_LIBDIR_LOCAL)

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
	$(EFFECTIVE_CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(CFLAGS) -MMD -MP -c $< -o $@

# ── Keep intermediate objects ──────────────────────────────────
# The user program objects are GNU-make intermediates of the .elf links.
# GNU make deletes intermediates after a successful build, so the second
# `make disk.img` after a clean rebuild would recompile the pattern-linked
# objects once. .SECONDARY keeps them so the second build is a true
# 0-compile no-op. NOTE: the pattern form `.SECONDARY: $(OBJ_DIR)/%.o` does
# NOT hold on GNU Make 4.4 — the object list must be expanded explicitly.
# (The objects of the explicit-link programs — terminal/tetris and their
# helpers — and the explicit targets crt0/sigreturn/terminal_font are not
# intermediates and need no protection.)
.SECONDARY: $(patsubst %.c,$(OBJ_DIR)/%.o,$(wildcard *.c)) \
            $(patsubst %.S,$(OBJ_DIR)/%.o,$(wildcard *.S))

# ── crt0 startup (compiled once, linked into every program) ─
CRT0_OBJ := $(OBJ_DIR)/crt0.o

$(CRT0_OBJ): crt0.S
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(CFLAGS) -MMD -MP -c $< -o $@

# ── sigreturn trampoline (compiled once, linked into every program) ─
SIGRETURN_OBJ := $(OBJ_DIR)/sigreturn_trampoline.o

$(SIGRETURN_OBJ): sigreturn_trampoline.S
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(CFLAGS) -MMD -MP -c $< -o $@

# ── Link rules (pattern: each .c → .elf linked with crt0) ─
# init.c → $(OBJ_DIR)/init.o → $(BUILD_DIR)/init.elf
$(BUILD_DIR)/%.elf: $(OBJ_DIR)/%.o $(CRT0_OBJ) $(SIGRETURN_OBJ) linker.ld
	@mkdir -p $(dir $@)
	$(TARGET_LD) $(USER_RAW_LDFLAGS) -o $@ $(CRT0_OBJ) $(SIGRETURN_OBJ) $< -lc
	$(OBJ_CPY) --strip-debug $@

# ══════════════════════════════════════════════════════════════
#  terminal.elf — needs embedded font (override pattern rule)
# ══════════════════════════════════════════════════════════════
TERMINAL_FONT_OBJ := $(OBJ_DIR)/terminal_font.o

$(BUILD_DIR)/terminal.elf: $(OBJ_DIR)/terminal.o $(OBJ_DIR)/terminal_core.o $(CRT0_OBJ) $(SIGRETURN_OBJ) $(TERMINAL_FONT_OBJ) linker.ld
	@mkdir -p $(dir $@)
	$(TARGET_LD) $(USER_RAW_LDFLAGS) -o $@ $(CRT0_OBJ) $(SIGRETURN_OBJ) $(OBJ_DIR)/terminal.o $(OBJ_DIR)/terminal_core.o $(TERMINAL_FONT_OBJ) -lc
	$(OBJ_CPY) --strip-debug $@

$(TERMINAL_FONT_OBJ): terminal_font.psf
	@mkdir -p $(dir $@)
	$(OBJ_CPY) -B i386 -I binary -O elf64-x86-64 terminal_font.psf $@

# ══════════════════════════════════════════════════════════════
#  tetris.elf — needs tetris_logic.o (override pattern rule)
# ══════════════════════════════════════════════════════════════
$(BUILD_DIR)/tetris.elf: $(OBJ_DIR)/tetris.o $(OBJ_DIR)/tetris_logic.o $(CRT0_OBJ) $(SIGRETURN_OBJ) linker.ld
	@mkdir -p $(dir $@)
	$(TARGET_LD) $(USER_RAW_LDFLAGS) -o $@ $(CRT0_OBJ) $(SIGRETURN_OBJ) $(OBJ_DIR)/tetris.o $(OBJ_DIR)/tetris_logic.o -lc
	$(OBJ_CPY) --strip-debug $@

# ── Clean ──────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR) *.elf

# ── Dependency tracking ────────────────────────────────────
C_DEPS := $(patsubst %.c,$(OBJ_DIR)/%.d,$(C_SOURCES))
ASM_DEPS := $(patsubst %.S,$(OBJ_DIR)/%.d,$(ASM_SOURCES))
-include $(C_DEPS) $(ASM_DEPS)
