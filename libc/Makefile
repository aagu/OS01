# ── Architecture ────────────────────────────────────────────
HOSTARCH  := x86_64
ARCHDIR   := arch/$(HOSTARCH)
BUILD_DIR := ../build/$(HOSTARCH)/libc

# ── Compiler / Linker (inherited from root Makefile exports) ─
CC     ?= clang -target x86_64-unknown-none
LD     ?= ld.lld -m elf_x86_64
AR     ?= llvm-ar

# ── Load architecture config ────────────────────────────────
include $(ARCHDIR)/make.config

# ── Compile flags ──────────────────────────────────────────
CFLAGS := $(CFLAGS) -ffreestanding -Wall -Wextra -D__is_libc -Iinclude -fno-stack-protector $(ARCH_CFLAGS)
LIBK_CFLAGS := $(CFLAGS) -D__is_libk

# ── Source discovery (wildcard — no manual lists!) ──────────
C_SOURCES := \
    $(wildcard ctype/*.c) \
    $(wildcard dirent/*.c) \
    $(wildcard errno/*.c) \
    $(wildcard list/*.c) \
    $(wildcard stdio/*.c) \
    $(wildcard stdlib/*.c) \
    $(wildcard string/*.c) \
    $(wildcard time/*.c) \
    $(wildcard unistd/*.c)

# Remove duplicates: free.c conflicts with malloc.c, chown.c with uid_stubs.c
C_SOURCES := $(filter-out stdlib/free.c unistd/chown.c, $(C_SOURCES))

# Object files (inside BUILD_DIR)
# string/memcmp.c → build/x86_64/libc/string/memcmp.o     (libc)
#                  → build/x86_64/libc/string/memcmp.libk.o (libk)
LIBC_OBJS := $(addprefix $(BUILD_DIR)/, $(C_SOURCES:.c=.o))
LIBK_OBJS := $(addprefix $(BUILD_DIR)/, $(C_SOURCES:.c=.libk.o))

# Arch-specific objects (if any)
LIBC_ARCH_OBJS := $(addprefix $(BUILD_DIR)/, $(ARCH_FREEOBJS))
LIBK_ARCH_OBJS := $(addprefix $(BUILD_DIR)/, $(ARCH_FREEOBJS:.o=.libk.o))
LIBC_HOSTED_OBJS := $(addprefix $(BUILD_DIR)/, $(ARCH_HOSTEDOBJS))

LIBC_ALL_OBJS := $(LIBC_OBJS) $(LIBC_ARCH_OBJS) $(LIBC_HOSTED_OBJS)
LIBK_ALL_OBJS := $(LIBK_OBJS) $(LIBK_ARCH_OBJS)

# ── Targets ────────────────────────────────────────────────
.PHONY: all clean install install-headers install-libs

all: libc.a libk.a

libc.a: $(LIBC_ALL_OBJS)
	$(AR) rcs $@ $(LIBC_ALL_OBJS)

libk.a: $(LIBK_ALL_OBJS)
	$(AR) rcs $@ $(LIBK_ALL_OBJS)

# ── Compile rules ──────────────────────────────────────────
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.libk.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(LIBK_CFLAGS) -MMD -MP -c $< -o $@

# ── Installation (into sysroot) ────────────────────────────
install: install-headers install-libs

install-headers:
	mkdir -p $(SYSROOT)$(INCLUDEDIR)
	cp -R --preserve=timestamps include/. $(SYSROOT)$(INCLUDEDIR)/.

install-libs: libc.a libk.a
	mkdir -p $(SYSROOT)$(LIBDIR)
	cp libc.a libk.a $(SYSROOT)$(LIBDIR)

# ── Clean ──────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR) libc.a libk.a

# ── Dependency tracking ────────────────────────────────────
ALL_DEPS := $(patsubst %.o,%.d,$(LIBC_ALL_OBJS)) $(patsubst %.o,%.d,$(LIBK_ALL_OBJS))
-include $(ALL_DEPS)
