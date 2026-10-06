# ── liblvgl Makefile (staging component — spec: single-writer sysroot)
#
# This Makefile is the build driver for the userland LVGL static library
# (liblvgl.a). Its install target stages the library and public headers
# into $(STAGING_DIR)/lvgl, NEVER into the final sysroot.
# mk/components/sysroot.mk is the SINGLE WRITER of the sysroot generation;
# this Makefile only produces a manifest via find and lets sysroot.mk consume it.
LOCAL_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))

ifeq ($(strip $(OS01_PROFILE_FILE)),)
$(error invoke from the repository root: make PROFILE=x86_64-clang <target>)
endif
ifeq ($(wildcard $(OS01_PROFILE_FILE)),)
$(error OS01_PROFILE_FILE='$(OS01_PROFILE_FILE)' does not exist; invoke from the repository root)
endif
include $(OS01_PROFILE_FILE)

BUILD_DIR := $(LIBLVGL_BUILD_DIR)
LIBLVGL_DIR := $(abspath $(dir $(LOCAL_MAKEFILE)))
OS01_ROOT := $(abspath $(LIBLVGL_DIR)/..)

include $(OS01_ROOT)/thirdpart/lvgl.manifest

LVGL_DIR := $(OS01_ROOT)/$(LVGL_SOURCE)
LVGL_SRC_DIR := $(LVGL_DIR)/src

.PHONY: all clean install check-version

check-version:
	@test -e "$(OS01_ROOT)/$(LVGL_SOURCE)/.git" || { echo "ERROR: $(LVGL_SOURCE) not a git submodule (.git missing)"; exit 1; }
	@test -f "$(OS01_ROOT)/$(LVGL_SOURCE)/lvgl.h" || { echo "ERROR: $(LVGL_SOURCE) missing lvgl.h"; exit 1; }
	@head=$$(git -C "$(OS01_ROOT)/$(LVGL_SOURCE)" rev-parse --verify HEAD 2>/dev/null) || { echo "ERROR: failed to get git commit for $(LVGL_SOURCE)"; exit 1; }; \
	if [ "$$head" != "$(LVGL_REVISION)" ]; then \
	  echo "ERROR: $(LVGL_SOURCE) revision $$head does not match manifest $(LVGL_REVISION)"; exit 1; \
	fi

CFLAGS := --sysroot=$(STAGING_DIR)/kernel-headers \
          -ffreestanding -fno-builtin -Wall -Wextra -O2 \
          -fno-pic -fno-pie -mno-red-zone -fstack-protector-strong \
          -I$(STAGING_DIR)/kernel-headers/usr/include \
          -isystem $(STAGING_DIR)/libc/usr/include \
          -I$(LVGL_DIR) \
          -I$(OS01_ROOT)/config \
          -DLV_CONF_INCLUDE_SIMPLE

C_SOURCES := $(shell find $(LVGL_SRC_DIR) -name '*.c')
OBJS := $(patsubst $(LVGL_SRC_DIR)/%.c,$(BUILD_DIR)/obj/%.o,$(C_SOURCES))
ARCHIVE := $(BUILD_DIR)/lib/liblvgl.a
TARGET_AR ?= $(LLVM_AR)

all: $(ARCHIVE)

$(OBJS): | check-version

$(BUILD_DIR)/obj/%.o: $(LVGL_SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(ARCHIVE): $(OBJS)
	@mkdir -p $(dir $@)
	$(TARGET_AR) rcs $@ $^

install: $(ARCHIVE)
	@test -n "$(INSTALL_ROOT)" || { echo "ERROR: install requires INSTALL_ROOT (staging tree)"; exit 1; }
	@echo "$(INSTALL_ROOT)" | grep -q '/staging/lvgl$$' || { echo "ERROR: INSTALL_ROOT must point to staging/lvgl"; exit 1; }
	rm -rf $(INSTALL_ROOT)
	mkdir -p $(INSTALL_ROOT)/usr/include/lvgl $(INSTALL_ROOT)/usr/lib
	cp $(ARCHIVE) $(INSTALL_ROOT)/usr/lib/liblvgl.a
	cp $(OS01_ROOT)/config/lv_conf.h $(INSTALL_ROOT)/usr/include/lvgl/lv_conf.h
	cp $(LVGL_DIR)/lvgl.h $(LVGL_DIR)/lv_version.h $(LVGL_DIR)/LICENCE.txt $(INSTALL_ROOT)/usr/include/lvgl/
	cd $(LVGL_DIR) && find src -name '*.h' | cpio -pdm $(INSTALL_ROOT)/usr/include/lvgl
	@find $(INSTALL_ROOT) -type f ! -name manifest | sed 's|^$(INSTALL_ROOT)/||' | sort > $(INSTALL_ROOT)/manifest

clean:
	rm -rf $(BUILD_DIR)

ALL_DEPS := $(patsubst %.o,%.d,$(OBJS))
-include $(ALL_DEPS)
