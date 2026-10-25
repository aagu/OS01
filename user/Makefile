# ── Configuration ──────────────────────────────────────────
LOCAL_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))

# ── Profile-only contract ───────────────────────────────────
# The root Makefile is the only supported entry; component Makefiles
# require OS01_PROFILE_FILE at parse time. Build dir and toolchain come
# from the selected profile (USER_BUILD_DIR).
ifeq ($(strip $(OS01_PROFILE_FILE)),)
$(error invoke from the repository root: make PROFILE=x86_64-clang <target>)
endif
ifeq ($(wildcard $(OS01_PROFILE_FILE)),)
$(error OS01_PROFILE_FILE='$(OS01_PROFILE_FILE)' does not exist; invoke from the repository root)
endif
include $(OS01_PROFILE_FILE)
BUILD_DIR := $(USER_BUILD_DIR)
# Consumes the immutable generation the root artifact rule
# (mk/components/user.mk) leased and passed as SYSROOT_GENERATION_DIR — never
# the $(SYSROOT) symlink.
SYSROOT_DIR   := $(SYSROOT_GENERATION_DIR)
TARGET_INCDIR := $(SYSROOT_GENERATION_DIR)/usr/include
TARGET_LIBDIR_LOCAL := $(SYSROOT_GENERATION_DIR)/usr/lib

OBJ_DIR   := $(BUILD_DIR)/obj

CFLAGS   := --sysroot=$(SYSROOT_DIR) \
            -ffreestanding -fstack-protector-strong -fno-asynchronous-unwind-tables \
            -fno-pic -fno-pie -mno-red-zone \
            -I$(TARGET_INCDIR) \
            -Icommon -Wall -Wextra -O2

# ── Syscall test mode ──────────────────────────────────────
# When OS01_SYSTEST=1, init.c spawns /systest.elf instead of /busybox.elf sh
ifeq ($(OS01_SYSTEST),1)
override CFLAGS += -DOS01_SYSTEST
endif

# ── FB_RESOLUTION_TEST (QEMU resolution switcher Task 9) ────
# Test config only: compiles the terminal_display.c fbtest ENOMEM hook.
# test_resolution.c (the guest fault helper) is discovered below
# only for this variant.
ifeq ($(FB_RESOLUTION_TEST),1)
override CFLAGS += -DFB_RESOLUTION_TEST
endif

# ── USER_DEBUG (user-space debug log) ────────────────────────
# Default OFF: the user-space log_debug() facade (user/common/user_log.h)
# is compiled out, so no user program writes debug records to the serial
# port.  `make USER_DEBUG=1 ...` compiles the debug records in (the
# terminal's per-second [term-present] / [term-probe] lines).
ifeq ($(USER_DEBUG),1)
override CFLAGS += -DUSER_DEBUG=1
endif

USER_RAW_LDFLAGS := -m elf_x86_64 -static -no-pie -T linker.ld -L$(TARGET_LIBDIR_LOCAL)

# ── Program discovery and app manifests ─────────────────────
.DEFAULT_GOAL := all
USER_BUILD_CONTEXT := 1
include $(OS01_ROOT)/user/apps.mk
PROGRAMS := $(addprefix $(BUILD_DIR)/,$(addsuffix .elf,$(USER_PROGRAMS)))
CRT0_OBJ := $(OBJ_DIR)/crt0.o
SIGRETURN_OBJ := $(OBJ_DIR)/sigreturn_trampoline.o
COMMON_LINK_DEPS := $(CRT0_OBJ) $(SIGRETURN_OBJ) linker.ld Makefile apps.mk

# ── User runtime consumer link probe (monitor plan Task 5) ────
# The root user component passes the consumer=user builtins archive as
# USER_RUNTIME_ARCHIVE (build/<profile>/runtime/user/<provider>-<digest>/
# libos01-builtins.a).  The probe is a real user program: it links crt0 +
# the probe object + the archive (in the app-LDLIBS position, after the
# objects, before -lc) and must therefore fail with an undefined __udivti3
# until the user runtime consumer exists.  The archive is an ELF
# prerequisite so an archive-only update still relinks the probe.
RUNTIME_PROBE_NAME := user_udivti3_link_probe
RUNTIME_PROBE_ELF  := $(BUILD_DIR)/$(RUNTIME_PROBE_NAME).elf
RUNTIME_PROBE_OBJ  := $(OBJ_DIR)/$(RUNTIME_PROBE_NAME).o

$(RUNTIME_PROBE_OBJ): $(OS01_ROOT)/runtime/selftest/$(RUNTIME_PROBE_NAME).c \
                      $(OS01_ROOT)/runtime/selftest/udivti3_vectors.h
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(CFLAGS) -I$(OS01_ROOT)/runtime/selftest \
		-MMD -MP -c $< -o $@

$(RUNTIME_PROBE_ELF): $(RUNTIME_PROBE_OBJ) $(CRT0_OBJ) $(SIGRETURN_OBJ) linker.ld \
                      $(USER_RUNTIME_ARCHIVE)
	@mkdir -p $(dir $@)
	$(TARGET_LD) $(USER_RAW_LDFLAGS) -o $@ $(CRT0_OBJ) $(SIGRETURN_OBJ) \
		$(RUNTIME_PROBE_OBJ) $(USER_RUNTIME_ARCHIVE) -lc
	$(OBJ_CPY) --strip-debug $@

-include $(RUNTIME_PROBE_OBJ:.o=.d)

.PHONY: all clean
all: $(PROGRAMS) $(RUNTIME_PROBE_ELF)

$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(OBJ_DIR)/%.o: %.S
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(CFLAGS) -MMD -MP -c $< -o $@

# Compile shared sources once with global flags. App flags affect local sources.
user_objects = $(addprefix $(OBJ_DIR)/,$(addsuffix .o,$(basename $(1))))
USER_OBJECTS := $(call user_objects,$(USER_FLAT_SOURCES)) $(CRT0_OBJ) $(SIGRETURN_OBJ)

define flat_program
$(BUILD_DIR)/$(1).elf: $(OBJ_DIR)/$(1).o $(COMMON_LINK_DEPS)
	@mkdir -p $$(dir $$@)
	$$(TARGET_LD) $$(USER_RAW_LDFLAGS) -o $$@ $$(CRT0_OBJ) $$(SIGRETURN_OBJ) $(OBJ_DIR)/$(1).o -lc
	$$(OBJ_CPY) --strip-debug $$@
endef
$(foreach app,$(USER_FLAT_PROGRAMS),$(eval $(call flat_program,$(app))))

define directory_program
USER_APP_$(1)_OBJECTS := $(call user_objects,$(USER_APP_$(1)_SOURCES)) $(USER_APP_$(1)_EXTRA_OBJS)
USER_OBJECTS += $$(USER_APP_$(1)_OBJECTS)
$(call user_objects,$(filter $(1)/%,$(USER_APP_$(1)_SOURCES))): override CFLAGS += $(USER_APP_$(1)_CFLAGS)
$(call user_objects,$(USER_APP_$(1)_SOURCES)): $(1)/app.mk
$(BUILD_DIR)/$(1).elf: $$(USER_APP_$(1)_OBJECTS) $(COMMON_LINK_DEPS) $(1)/app.mk $$(filter %.a,$$(USER_APP_$(1)_LDLIBS))
	@mkdir -p $$(dir $$@)
	$$(TARGET_LD) $$(USER_RAW_LDFLAGS) $(USER_APP_$(1)_LDFLAGS) -o $$@ $$(CRT0_OBJ) $$(SIGRETURN_OBJ) $$(USER_APP_$(1)_OBJECTS) $(USER_APP_$(1)_LDLIBS) -lc
	$$(OBJ_CPY) --strip-debug $$@
endef
$(foreach app,$(USER_DIRECTORY_PROGRAMS),$(eval $(call directory_program,$(app))))

.SECONDARY: $(USER_OBJECTS)
-include $(USER_OBJECTS:.o=.d)

clean:
	rm -rf $(BUILD_DIR)
