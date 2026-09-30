# ── libgfx Makefile (staging component — spec: single-writer sysroot)
#
# This Makefile is the build driver for the userland 2D graphics
# static library (libgfx.a).  Its install target stages the library
# and its public header into $(STAGING_DIR)/libgfx, NEVER into the
# final sysroot.  mk/components/sysroot.mk is the SINGLE WRITER of
# the sysroot generation; this Makefile only produces a manifest
# via find (mirroring libc/Makefile's pattern) and lets sysroot.mk
# consume it.
#
# Profile-only contract — the root Makefile is the only supported
# entry; OS01_PROFILE_FILE resolves the toolchain.
LOCAL_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))

ifeq ($(strip $(OS01_PROFILE_FILE)),)
$(error invoke from the repository root: make PROFILE=x86_64-clang <target>)
endif
ifeq ($(wildcard $(OS01_PROFILE_FILE)),)
$(error OS01_PROFILE_FILE='$(OS01_PROFILE_FILE)' does not exist; invoke from the repository root)
endif
include $(OS01_PROFILE_FILE)

BUILD_DIR := $(LIBGFX_BUILD_DIR)
LIBGFX_DIR := $(abspath $(dir $(LOCAL_MAKEFILE)))

# CFLAGS for the userland static lib: same freestanding flags as
# libc + the public gfx.h needs <uapi/gfx.h> from the kernel
# headers staging tree (libgfx-install.stamp depends on
# kernel-headers-install.stamp, so $(STAGING_DIR)/kernel-headers
# is guaranteed to contain usr/include/uapi/gfx.h by the time we
# run).  We point the compiler at that staging tree via
# `--sysroot=$(STAGING_DIR)/kernel-headers` (mirroring the mbedtls
# pattern in sysroot.mk) plus an explicit -I for the include dir
# and -I$(LIBGFX_DIR) so internal.h resolves from gfx.c.
#
# errno.h is owned by libc in userspace (kernel-headers explicitly
# removes its own errno.h from the staged tree to avoid a duplicate
# destination in the sysroot generation).  We pull libc's headers
# via -isystem so errno.h / fcntl.h / unistd.h / etc. resolve from
# the libc staging tree; uapi/ headers continue to resolve from the
# kernel-headers staging tree (libc/Makefile does NOT install uapi/,
# so the two staging trees are disjoint for the libgfx use case).
#
# We deliberately do NOT use the final sysroot generation here:
# the sysroot is published by the SYSROOT_STAMP recipe AFTER all
# component stamps complete; using it during component install
# would create a forward dependency that the publish protocol
# (single-writer sysroot) is designed to forbid.
CFLAGS := --sysroot=$(STAGING_DIR)/kernel-headers \
          -ffreestanding -fno-builtin -Wall -Wextra -O2 \
          -I$(STAGING_DIR)/kernel-headers/usr/include \
          -I$(LIBGFX_DIR) \
          -isystem $(STAGING_DIR)/libc/usr/include

# Sources — Task 3 ships ONLY gfx.c (open/close/get_info/set_clip/
# present).  Task 4 adds line.c / sprite.c; the wildcard picks them
# up automatically.
C_SOURCES := $(wildcard $(LIBGFX_DIR)/*.c)
OBJS := $(addprefix $(BUILD_DIR)/, $(notdir $(C_SOURCES:.c=.o)))

ARCHIVE := $(BUILD_DIR)/lib/libgfx.a

TARGET_AR ?= $(AR)

.PHONY: all clean install

all: $(ARCHIVE)

$(ARCHIVE): $(OBJS)
	@mkdir -p $(dir $@)
	$(TARGET_AR) rcs $@ $^

$(BUILD_DIR)/%.o: $(LIBGFX_DIR)/%.c \
		$(LIBGFX_DIR)/gfx.h \
		$(LIBGFX_DIR)/internal.h
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(CFLAGS) -MMD -MP -c $< -o $@

# ── Installation (staging tree — spec: single-writer sysroot) ──
# Rebuilds the staging tree from source (rm -rf then re-install)
# so removed files do not linger.  Installs ONLY the public header
# (usr/include/gfx.h) and the archive (usr/lib/libgfx.a); the
# UAPI types that <gfx.h> includes come from the kernel-headers
# install path (usr/include/uapi/gfx.h) and MUST NOT be duplicated
# here (sysroot.mk's manifest-uniqueness check would reject a
# duplicate destination).  internal.h is NEVER installed — it is
# shared private state between the libgfx source files.
#
# $(ARCHIVE) is a prerequisite so the install recipe only runs after
# libgfx.a has been built (the os01_submake wrapper invokes
# `install` directly, not `all`, so we must declare the build
# dependency here; otherwise the install recipe copies a
# not-yet-built archive and the cp fails).
install: $(ARCHIVE)
	@test -n "$(INSTALL_ROOT)" || { echo "ERROR: install requires INSTALL_ROOT (staging tree)"; exit 1; }
	rm -rf $(INSTALL_ROOT)
	mkdir -p $(INSTALL_ROOT)/usr/include $(INSTALL_ROOT)/usr/lib
	cp $(LIBGFX_DIR)/gfx.h $(INSTALL_ROOT)/usr/include/gfx.h
	cp $(ARCHIVE) $(INSTALL_ROOT)/usr/lib/libgfx.a
	@find $(INSTALL_ROOT) -type f ! -name manifest | sed 's|^$(INSTALL_ROOT)/||' | sort > $(INSTALL_ROOT)/manifest

# ── Clean (profile-scoped: only our own build dir) ──
clean:
	rm -rf $(BUILD_DIR)

# ── Dependency tracking ────────────────────────────────────────
ALL_DEPS := $(patsubst %.o,%.d,$(OBJS))
-include $(ALL_DEPS)
