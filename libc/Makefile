# ── Architecture ────────────────────────────────────────────
HOSTARCH  := x86_64
ARCHDIR   := arch/$(HOSTARCH)
LOCAL_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))

# ── Profile-only contract ───────────────────────────────────
# The root Makefile is the only supported entry; component Makefiles
# require OS01_PROFILE_FILE at parse time. Build dir and toolchain come
# from the selected profile (LIBC_BUILD_DIR).
ifeq ($(strip $(OS01_PROFILE_FILE)),)
$(error invoke from the repository root: make PROFILE=x86_64-clang <target>)
endif
ifeq ($(wildcard $(OS01_PROFILE_FILE)),)
$(error OS01_PROFILE_FILE='$(OS01_PROFILE_FILE)' does not exist; invoke from the repository root)
endif
include $(OS01_PROFILE_FILE)
BUILD_DIR := $(LIBC_BUILD_DIR)

# ── Load architecture config ────────────────────────────────
include $(ARCHDIR)/make.config

# ── Compile flags ──────────────────────────────────────────
CFLAGS := $(CFLAGS) -ffreestanding -Wall -Wextra -D__is_libc -Iinclude -fno-stack-protector -fomit-frame-pointer $(ARCH_CFLAGS)
LIBK_CFLAGS := $(CFLAGS) -D__is_libk -mno-red-zone -I../kernel/include

# ── Source discovery (wildcard — no manual lists!) ──────────
C_SOURCES := \
    $(wildcard ctype/*.c) \
    $(wildcard errno/*.c) \
    $(wildcard list/*.c) \
    $(wildcard rbtree/*.c) \
    $(wildcard network/*.c) \
    $(wildcard stdio/*.c) \
    $(wildcard stdlib/*.c) \
    $(wildcard string/*.c) \
    $(wildcard time/*.c) \
    $(wildcard unistd/*.c) \
    $(wildcard termios/*.c) \
    $(wildcard socket/*.c) \
    $(wildcard netdb/*.c) \
    $(wildcard sched/*.c) \
    $(wildcard xattr/*.c) \
    $(wildcard resource/*.c) \
    $(wildcard pwd/*.c) \
    $(wildcard grp/*.c) \
    $(wildcard csu/*.c) \
    $(wildcard signal/*.c) \
    $(wildcard arpa/*.c) \
    $(wildcard libgen/*.c) \
    $(wildcard pthread/*.c) \
    $(wildcard random/*.c)

# Remove duplicates: free.c conflicts with malloc.c (chown.c conflict gone — uid_stubs.c deleted)
C_SOURCES := $(filter-out stdlib/free.c, $(C_SOURCES))

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

# ── Archives ─────────────────────────────────────────────────
# Archives live under the profile build dir (LIBC_BUILD_DIR), never in the
# libc/ source tree.
LIBC_ARCHIVE := $(LIBC_BUILD_DIR)/lib/libc.a
LIBK_ARCHIVE := $(LIBC_BUILD_DIR)/lib/libk.a
# Archive tool: TARGET_AR comes from the profile (spec profile variable);
# a command-line override wins.
TARGET_AR ?= $(AR)

# ── Targets ────────────────────────────────────────────────
.PHONY: all clean install install-headers install-libs

all: $(LIBC_ARCHIVE) $(LIBK_ARCHIVE)

$(LIBC_ARCHIVE): $(LIBC_ALL_OBJS)
	@mkdir -p $(dir $@)
	$(TARGET_AR) rcs $@ $^

$(LIBK_ARCHIVE): $(LIBK_ALL_OBJS)
	@mkdir -p $(dir $@)
	$(TARGET_AR) rcs $@ $^

# ── Compile rules ──────────────────────────────────────────
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD_DIR)/%.libk.o: %.c
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(LIBK_CFLAGS) -MMD -MP -c $< -o $@

# ── Installation (staging tree) ─────────────────────────────
# REBUILDS the staging tree from source (rm -rf then re-install) so
# headers/archives removed from libc/ do not linger in staging (spec:
# deleted/renamed paths must vanish from the next generation). The two
# sub-installs run through an inner make AFTER the cleanup, so they never
# race with the rm -rf under -j.
install:
	@test -n "$(INSTALL_ROOT)" || { echo "ERROR: install (profile mode) requires INSTALL_ROOT (staging tree)"; exit 1; }
	rm -rf $(INSTALL_ROOT)
	$(MAKE) install-headers install-libs
	@find $(INSTALL_ROOT) -type f ! -name manifest | sed 's|^$(INSTALL_ROOT)/||' | sort > $(INSTALL_ROOT)/manifest

install-headers:
	@test -n "$(INSTALL_ROOT)" || { echo "ERROR: install (profile mode) requires INSTALL_ROOT (staging tree)"; exit 1; }
	mkdir -p $(INSTALL_ROOT)/usr/include
	cp -R --preserve=timestamps include/. $(INSTALL_ROOT)/usr/include/.

install-libs: $(LIBC_ARCHIVE) $(LIBK_ARCHIVE)
	@test -n "$(INSTALL_ROOT)" || { echo "ERROR: install (profile mode) requires INSTALL_ROOT (staging tree)"; exit 1; }
	mkdir -p $(INSTALL_ROOT)/usr/lib
	cp $(LIBC_ARCHIVE) $(INSTALL_ROOT)/usr/lib/libc.a
	cp $(LIBK_ARCHIVE) $(INSTALL_ROOT)/usr/lib/libk.a

# ── Clean ──────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR)

# ── Dependency tracking ────────────────────────────────────
ALL_DEPS := $(patsubst %.o,%.d,$(LIBC_ALL_OBJS)) $(patsubst %.o,%.d,$(LIBK_ALL_OBJS))
-include $(ALL_DEPS)
