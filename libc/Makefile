# ── Architecture ────────────────────────────────────────────
# Cross-compile default: when invoked from a profile component rule
# (e.g. mk/components/image.mk building an aarch64 libk.a), the caller
# passes ARCH=<arch> on the make command line; that overrides the
# HOSTARCH ?= default. x86_64 stays the default so the libc/install
# sysroot path (userland profiles only) keeps working unchanged.
HOSTARCH  ?= x86_64
ifeq ($(origin ARCH),command line)
HOSTARCH := $(ARCH)
endif
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
CFLAGS := $(CFLAGS) -ffreestanding -Wall -Wextra -D__is_libc -Iinclude -fstack-protector-strong -fomit-frame-pointer $(ARCH_CFLAGS)
LIBK_CFLAGS := $(CFLAGS) -D__is_libk -mno-red-zone -I../kernel/include -fno-stack-protector

# ── Source discovery (wildcard — no manual lists!) ──────────
# x86_64 builds the full userland libc (libc.a + libk.a are both installed
# to the immutable sysroot generation for kernel/user linking). aarch64
# historically only needed libk.a (kernel-link only); since the 2026-11-18
# userland capability it builds BOTH: libc.a uses the same full source list
# as x86 (libc/include/sys/syscall.h now carries an __aarch64__ SVC branch,
# so the historical x86-asm blocker is gone), while libk.a keeps the
# intentionally narrower kernel subset.
#
# `stdlib/realpath.c` is in the aarch64 libk list for the libk SURFACE only:
# under __is_libk the file compiles to the historical empty shell (it answers
# NULL), so the kernel archive exports the symbol without gaining an object
# that would issue user syscalls.  The user-space implementation is in the
# same file, behind `#ifndef __is_libk`.
ifeq ($(HOSTARCH),aarch64)
LIBK_C_SOURCES := \
    $(wildcard ctype/*.c) \
    $(wildcard errno/*.c) \
    $(wildcard list/*.c) \
    $(wildcard rbtree/*.c) \
    stdlib/calloc.c stdlib/malloc.c stdlib/reallocarray.c \
    stdlib/realpath.c \
    stdio/vsprintf.c stdio/vsnprintf.c stdio/sprintf.c \
    $(wildcard string/*.c) \
    $(filter-out ssp/ssp.c, $(wildcard ssp/*.c))
LIBC_C_SOURCES := \
    $(wildcard ctype/*.c) \
    $(wildcard errno/*.c) \
    $(wildcard list/*.c) \
    $(wildcard rbtree/*.c) \
    $(wildcard network/*.c) \
    $(wildcard stdio/*.c) \
    $(wildcard stdlib/*.c) \
    $(wildcard string/*.c) \
    $(wildcard time/*.c) \
    $(wildcard sys/stat/*.c) \
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
    $(wildcard random/*.c) \
    $(wildcard ssp/*.c)
# The float-parse core (floatscan.c) and its consumers (strtod.c,
# vsscanf.c, floatconv.c) are out for aarch64: floatscan/strtod/vsscanf
# are vendored for the x87 80-bit long double ABI and #error on the quad
# ABI, and floatconv.c needs `double` type support while the aarch64
# userland compiles with -mgeneral-regs-only (the kernel traps FPEN for
# EL0).  vsprintf.c emits literal text for %f/%e/%g on this arch, so
# nothing references floatconv_render.  Excluded until FP is enabled for
# EL0 or the libc-completion float tasks port these paths.
LIBC_C_SOURCES := $(filter-out stdlib/floatscan.c stdlib/strtod.c stdio/vsscanf.c stdio/floatconv.c,$(LIBC_C_SOURCES))
# x86 inline-asm holdouts: setjmp (jmp_buf layout) and pthread/mutex
# (xchg/cmpxchg) need an aarch64 port; nothing in the aarch64 userland
# (init/sh) references them yet.
LIBC_C_SOURCES := $(filter-out stdlib/setjmp.c pthread/mutex.c,$(LIBC_C_SOURCES))
else
LIBK_C_SOURCES := \
    $(wildcard ctype/*.c) \
    $(wildcard errno/*.c) \
    $(wildcard list/*.c) \
    $(wildcard rbtree/*.c) \
    $(wildcard network/*.c) \
    $(wildcard stdio/*.c) \
    $(wildcard stdlib/*.c) \
    $(wildcard string/*.c) \
    $(wildcard time/*.c) \
    $(wildcard sys/stat/*.c) \
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
    $(wildcard random/*.c) \
    $(wildcard ssp/*.c)
LIBC_C_SOURCES := $(LIBK_C_SOURCES)
endif

# Remove duplicates: free.c conflicts with malloc.c (chown.c conflict gone — uid_stubs.c deleted)
LIBK_C_SOURCES := $(filter-out stdlib/free.c, $(LIBK_C_SOURCES))
LIBC_C_SOURCES := $(filter-out stdlib/free.c, $(LIBC_C_SOURCES))

# Object files (inside BUILD_DIR)
# string/memcmp.c → build/x86_64/libc/string/memcmp.o     (libc)
#                  → build/x86_64/libc/string/memcmp.libk.o (libk)
LIBC_OBJS := $(addprefix $(BUILD_DIR)/, $(LIBC_C_SOURCES:.c=.o))
LIBK_OBJS := $(addprefix $(BUILD_DIR)/, $(LIBK_C_SOURCES:.c=.libk.o))

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
# aarch64 cross-compile (2026-11-18): libc.a is now built alongside libk.a
# — the userland capability stages both into the aarch64 sysroot.  The
# historical x86-only blocker (syscall.h inline asm) is resolved at the
# header with an __aarch64__ SVC branch.
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

# ── Compile keys ───────────────────────────────────────────
LIBC_COMPILE_KEY := $(BUILD_DIR)/.libc_compile.key
LIBK_COMPILE_KEY := $(BUILD_DIR)/.libk_compile.key

.PHONY: LIBC_KEY_FORCE LIBK_KEY_FORCE
LIBC_KEY_FORCE:
LIBK_KEY_FORCE:

$(LIBC_COMPILE_KEY): LIBC_KEY_FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' 'compiler=$(EFFECTIVE_CC) toolchain=$(shell $(EFFECTIVE_CC) --version 2>/dev/null | head -1) cflags=$(strip $(CFLAGS))' > $@.new
	@cmp -s $@.new $@ || mv -f $@.new $@

$(LIBK_COMPILE_KEY): LIBK_KEY_FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' 'compiler=$(EFFECTIVE_CC) toolchain=$(shell $(EFFECTIVE_CC) --version 2>/dev/null | head -1) cflags=$(strip $(LIBK_CFLAGS))' > $@.new
	@cmp -s $@.new $@ || mv -f $@.new $@

# ── Compile rules ──────────────────────────────────────────
$(BUILD_DIR)/%.o: %.c $(LIBC_COMPILE_KEY)
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(CFLAGS) -MMD -MP -c $< -o $@

# aarch64: difftime(time.c) is the one legitimate FP user in the userland
# libc — compile just this TU with FP enabled (default AAPCS), keeping the
# FP instructions confined to a function nothing on this arch calls.  The
# rest of the libc stays -mgeneral-regs-only.
ifeq ($(HOSTARCH),aarch64)
$(BUILD_DIR)/time/time.o: override CFLAGS := $(filter-out -mgeneral-regs-only,$(CFLAGS))
endif

$(BUILD_DIR)/%.libk.o: %.c $(LIBK_COMPILE_KEY)
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
	@# ARCH is forwarded explicitly: the controlled sub-make boundary runs
	@# with MAKEOVERRIDES= (mk/project.mk), so the inner make would lose
	@# the command-line ARCH and default HOSTARCH to x86_64.
	$(MAKE) install-headers install-libs ARCH=$(HOSTARCH)
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
