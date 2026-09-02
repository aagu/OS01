# ── Architecture ────────────────────────────────────────────
HOSTARCH  := x86_64
ARCHDIR   := arch/$(HOSTARCH)
ARCH ?= x86_64
LOCAL_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))

# ── Profile mode (root os01_submake) vs standalone legacy ──
# With OS01_PROFILE_FILE set, include the selected profile instead of the
# legacy toolchain.mk and take the build dir from the profile
# (LIBC_BUILD_DIR). The two paths are mutually exclusive.
ifdef OS01_PROFILE_FILE
include $(OS01_PROFILE_FILE)
BUILD_DIR := $(LIBC_BUILD_DIR)
else
BUILD_DIR := ../build/$(HOSTARCH)/libc
ifeq ($(ARCH),x86_64)
include $(dir $(LOCAL_MAKEFILE))../toolchain.mk
endif
endif

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
# Profile mode: archives live under the profile build dir (LIBC_BUILD_DIR),
# never in the libc/ source tree. Legacy standalone mode keeps them in CWD.
ifdef OS01_PROFILE_FILE
LIBC_ARCHIVE := $(LIBC_BUILD_DIR)/lib/libc.a
LIBK_ARCHIVE := $(LIBC_BUILD_DIR)/lib/libk.a
else
LIBC_ARCHIVE := libc.a
LIBK_ARCHIVE := libk.a
endif
# Archive tool: profile mode takes TARGET_AR from the profile (spec profile
# variable); legacy standalone mode falls back to toolchain.mk's AR.
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

# ── Installation (staging tree in profile mode, legacy sysroot otherwise) ──
# Profile mode REBUILDS the staging tree from source (rm -rf then re-install)
# so headers/archives removed from libc/ do not linger in staging (spec:
# deleted/renamed paths must vanish from the next generation). The two
# sub-installs run through an inner make AFTER the cleanup, so they never
# race with the rm -rf under -j.
install:
ifdef OS01_PROFILE_FILE
	@test -n "$(INSTALL_ROOT)" || { echo "ERROR: install (profile mode) requires INSTALL_ROOT (staging tree)"; exit 1; }
	rm -rf $(INSTALL_ROOT)
	$(MAKE) install-headers install-libs
	@find $(INSTALL_ROOT) -type f ! -name manifest | sed 's|^$(INSTALL_ROOT)/||' | sort > $(INSTALL_ROOT)/manifest
else
	$(MAKE) install-headers install-libs
endif

install-headers:
ifdef OS01_PROFILE_FILE
	@test -n "$(INSTALL_ROOT)" || { echo "ERROR: install (profile mode) requires INSTALL_ROOT (staging tree)"; exit 1; }
	mkdir -p $(INSTALL_ROOT)/usr/include
	cp -R --preserve=timestamps include/. $(INSTALL_ROOT)/usr/include/.
else
	mkdir -p $(TARGET_INCLUDEDIR)
	cp -R --preserve=timestamps include/. $(TARGET_INCLUDEDIR)/.
endif

install-libs: $(LIBC_ARCHIVE) $(LIBK_ARCHIVE)
ifdef OS01_PROFILE_FILE
	@test -n "$(INSTALL_ROOT)" || { echo "ERROR: install (profile mode) requires INSTALL_ROOT (staging tree)"; exit 1; }
	mkdir -p $(INSTALL_ROOT)/usr/lib
	cp $(LIBC_ARCHIVE) $(INSTALL_ROOT)/usr/lib/libc.a
	cp $(LIBK_ARCHIVE) $(INSTALL_ROOT)/usr/lib/libk.a
else
	mkdir -p $(TARGET_LIBDIR)
	@cmp -s $(LIBC_ARCHIVE) $(TARGET_LIBDIR)/libc.a || cp $(LIBC_ARCHIVE) $(TARGET_LIBDIR)/libc.a
	@cmp -s $(LIBK_ARCHIVE) $(TARGET_LIBDIR)/libk.a || cp $(LIBK_ARCHIVE) $(TARGET_LIBDIR)/libk.a
endif

# ── Clean ──────────────────────────────────────────────────
clean:
	rm -rf $(BUILD_DIR) libc.a libk.a

# ── Dependency tracking ────────────────────────────────────
ALL_DEPS := $(patsubst %.o,%.d,$(LIBC_ALL_OBJS)) $(patsubst %.o,%.d,$(LIBK_ALL_OBJS))
-include $(ALL_DEPS)
