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
SYSROOT_DIR   := $(if $(SYSROOT_DIR),$(SYSROOT_DIR),$(if $(SYSROOT_GENERATION_DIR),$(SYSROOT_GENERATION_DIR),$(SYSROOT)))
TARGET_INCDIR := $(SYSROOT_DIR)/usr/include
TARGET_LIBDIR_LOCAL := $(SYSROOT_DIR)/usr/lib
TARGET_LIBC   := $(TARGET_LIBDIR_LOCAL)/libc.a
user_lib_dep   = $(patsubst -l%,$(TARGET_LIBDIR_LOCAL)/lib%.a,$(filter -l%,$(1))) $(filter %.a,$(1))

OBJ_DIR   := $(BUILD_DIR)/obj

CFLAGS   := --sysroot=$(SYSROOT_DIR) \
            -ffreestanding -fstack-protector-strong -fno-asynchronous-unwind-tables \
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

# ── CAPACITY_TEST (capacity test variant) ──────────────────────
ifeq ($(CAPACITY_TEST),1)
override CFLAGS += -DOS01_NOFILE_CAPACITY=64 -DCAPACITY_TEST=1
endif

# ── Architecture dispatch (2026-11-18) ───────────────────────
# The profile's RUNTIME_TARGET_user names the user target; the crt0
# object (installed into the sysroot by libc's install-libs), the linker
# script (libc/arch/<arch>/user.ld), the compile flags and link emulation
# follow it.
ifeq ($(RUNTIME_TARGET_user),aarch64-none-elf)
USER_ARCH := aarch64
else
USER_ARCH := x86_64
endif

# Startup object ships with the sysroot; the sigreturn trampoline is a
# libc.a member pulled by sigaction()/signal() (or explicit sa_restorer users).
CRT0_OBJ := $(TARGET_LIBDIR_LOCAL)/crt0.o
ifeq ($(USER_ARCH),aarch64)
LINKER_SCRIPT  := $(OS01_ROOT)/libc/arch/aarch64/user.ld
override CFLAGS += -fno-pic -fno-pie
USER_RAW_LDFLAGS := -static -T $(LINKER_SCRIPT) -L$(TARGET_LIBDIR_LOCAL)
else
LINKER_SCRIPT  := $(OS01_ROOT)/libc/arch/x86_64/user-pie.ld
override CFLAGS += -fPIE -mcmodel=small -mno-red-zone
USER_RAW_LDFLAGS := -m elf_x86_64 -static -pie --no-dynamic-linker \
                    -z max-page-size=4096 -z text -z norelro --pack-dyn-relocs=none \
                    -T $(LINKER_SCRIPT) -L$(TARGET_LIBDIR_LOCAL)
endif

# ── Compile & link keys ──────────────────────────────────────
USER_COMPILE_KEY := $(OBJ_DIR)/.user_compile.key
USER_LINK_KEY    := $(BUILD_DIR)/.user_link.key

.PHONY: USER_COMPILE_KEY_FORCE USER_LINK_KEY_FORCE
USER_COMPILE_KEY_FORCE:
USER_LINK_KEY_FORCE:

$(USER_COMPILE_KEY): USER_COMPILE_KEY_FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' 'compiler=$(EFFECTIVE_CC) toolchain=$(shell $(EFFECTIVE_CC) --version 2>/dev/null | head -1) cflags=$(strip $(CFLAGS))' > $@.new
	@cmp -s $@.new $@ || mv -f $@.new $@

$(USER_LINK_KEY): USER_LINK_KEY_FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' 'linker=$(TARGET_LD) ldflags=$(strip $(USER_RAW_LDFLAGS))' > $@.new
	@cmp -s $@.new $@ || mv -f $@.new $@

# ── Program discovery and app manifests ─────────────────────
.DEFAULT_GOAL := all
USER_BUILD_CONTEXT := 1
# D04: Doom engine archive definitions (DOOM_ARCHIVE, DOOM_PRIVATE_DIR, DOOM_SOURCE_STAMP)
# must be available when user/doom/app.mk is evaluated by apps.mk.
include $(OS01_ROOT)/user/doom/engine.mk
include $(OS01_ROOT)/user/apps.mk
PROGRAMS := $(addprefix $(BUILD_DIR)/,$(addsuffix .elf,$(USER_PROGRAMS)))
COMMON_LINK_DEPS := $(CRT0_OBJ) $(LINKER_SCRIPT) Makefile apps.mk $(USER_LINK_KEY)

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
                      $(OS01_ROOT)/runtime/selftest/udivti3_vectors.h $(USER_COMPILE_KEY)
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(CFLAGS) -I$(OS01_ROOT)/runtime/selftest \
		-MD -MP -c $< -o $@

$(RUNTIME_PROBE_ELF): $(RUNTIME_PROBE_OBJ) $(CRT0_OBJ) $(LINKER_SCRIPT) $(USER_LINK_KEY) \
                      $(USER_RUNTIME_ARCHIVE) $(TARGET_LIBC)
	@mkdir -p $(dir $@)
	$(TARGET_LD) $(USER_RAW_LDFLAGS) -o $@ $(CRT0_OBJ) \
		$(RUNTIME_PROBE_OBJ) $(USER_RUNTIME_ARCHIVE) -lc
	$(OBJ_CPY) --strip-debug $@

-include $(RUNTIME_PROBE_OBJ:.o=.d)

ASLR_B_EXEC_ELF :=
ifeq ($(OS01_SYSTEST),1)
ifneq ($(USER_ARCH),aarch64)
ASLR_B_EXEC_ELF := $(BUILD_DIR)/aslr_b_exec.elf
ASLR_B_EXEC_OBJ := $(OBJ_DIR)/systest/fixtures/aslr_b_exec.o
$(ASLR_B_EXEC_OBJ): systest/fixtures/aslr_b_exec.S $(USER_COMPILE_KEY)
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) --target=x86_64-unknown-none -ffreestanding -fno-pic -fno-pie -c $< -o $@
$(ASLR_B_EXEC_ELF): $(ASLR_B_EXEC_OBJ) $(OS01_ROOT)/libc/arch/x86_64/user.ld
	@mkdir -p $(dir $@)
	$(TARGET_LD) -m elf_x86_64 -static -T $(OS01_ROOT)/libc/arch/x86_64/user.ld -o $@ $<
	$(OBJ_CPY) --strip-debug $@
endif
endif

# ── PT_TLS acceptance fixtures (x86_64 systest only) ──────────
# Freestanding probes: no libc, raw int $0x80.  ptls_exec is a
# TinyGo-shaped pure-.tbss ET_EXEC, ptls_pie exercises .tdata + .tbss
# inside the static-PIE whitelist, ptls_plain has no PT_TLS at all.
PTLS_FIXTURES :=
ifeq ($(OS01_SYSTEST),1)
ifneq ($(USER_ARCH),aarch64)
PTLS_DIR       := systest/fixtures
PTLS_ENTRY_OBJ := $(OBJ_DIR)/systest/fixtures/ptls_entry.o
PTLS_EXEC_OBJ  := $(OBJ_DIR)/systest/fixtures/ptls_exec.o
PTLS_PIE_OBJ   := $(OBJ_DIR)/systest/fixtures/ptls_pie.o
PTLS_PLAIN_OBJ := $(OBJ_DIR)/systest/fixtures/ptls_plain.o
PTLS_EXEC_ELF  := $(BUILD_DIR)/ptls_exec.elf
PTLS_PIE_ELF   := $(BUILD_DIR)/ptls_pie.elf
PTLS_PLAIN_ELF := $(BUILD_DIR)/ptls_plain.elf
PTLS_FIXTURES  := $(PTLS_EXEC_ELF) $(PTLS_PIE_ELF) $(PTLS_PLAIN_ELF)

PTLS_BASE_CFLAGS := --target=x86_64-unknown-none -ffreestanding \
                    -fno-stack-protector -fno-asynchronous-unwind-tables \
                    -ftls-model=local-exec -O2 -Wall -Wextra

$(PTLS_ENTRY_OBJ): $(PTLS_DIR)/ptls_entry.S $(USER_COMPILE_KEY)
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) --target=x86_64-unknown-none -ffreestanding \
		-fno-pic -fno-pie -c $< -o $@

$(PTLS_EXEC_OBJ): $(PTLS_DIR)/ptls_probe.c $(USER_COMPILE_KEY)
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(PTLS_BASE_CFLAGS) -fno-pic -fno-pie -DPTLS_EXEC -c $< -o $@

$(PTLS_PIE_OBJ): $(PTLS_DIR)/ptls_probe.c $(USER_COMPILE_KEY)
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(PTLS_BASE_CFLAGS) -fPIE -DPTLS_PIE -c $< -o $@

$(PTLS_PLAIN_OBJ): $(PTLS_DIR)/ptls_probe.c $(USER_COMPILE_KEY)
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(PTLS_BASE_CFLAGS) -fno-pic -fno-pie -DPTLS_PLAIN -c $< -o $@

$(PTLS_EXEC_ELF): $(PTLS_ENTRY_OBJ) $(PTLS_EXEC_OBJ) $(PTLS_DIR)/ptls_exec.ld \
                  $(USER_LINK_KEY)
	@mkdir -p $(dir $@)
	$(TARGET_LD) -m elf_x86_64 -static -T $(PTLS_DIR)/ptls_exec.ld \
		-o $@ $(PTLS_ENTRY_OBJ) $(PTLS_EXEC_OBJ)
	$(OBJ_CPY) --strip-debug $@

$(PTLS_PIE_ELF): $(PTLS_ENTRY_OBJ) $(PTLS_PIE_OBJ) $(PTLS_DIR)/ptls_pie.ld \
                 $(USER_LINK_KEY)
	@mkdir -p $(dir $@)
	$(TARGET_LD) -m elf_x86_64 -static -pie --no-dynamic-linker \
		-z max-page-size=4096 -z text -z norelro --pack-dyn-relocs=none \
		-T $(PTLS_DIR)/ptls_pie.ld -o $@ $(PTLS_ENTRY_OBJ) $(PTLS_PIE_OBJ)
	$(OBJ_CPY) --strip-debug $@

$(PTLS_PLAIN_ELF): $(PTLS_ENTRY_OBJ) $(PTLS_PLAIN_OBJ) $(OS01_ROOT)/libc/arch/x86_64/user.ld \
                   $(USER_LINK_KEY)
	@mkdir -p $(dir $@)
	$(TARGET_LD) -m elf_x86_64 -static -T $(OS01_ROOT)/libc/arch/x86_64/user.ld \
		-o $@ $(PTLS_ENTRY_OBJ) $(PTLS_PLAIN_OBJ)
	$(OBJ_CPY) --strip-debug $@

# Structural gate: the ELF shape the probe behaviour relies on.
PTLS_CHECK_STAMP := $(BUILD_DIR)/.ptls-fixtures.checked
$(PTLS_CHECK_STAMP): $(PTLS_FIXTURES) $(OS01_ROOT)/tools/tests/check_ptls_fixture.py
	python3 $(OS01_ROOT)/tools/tests/check_ptls_fixture.py \
		--readelf $(LLVM_READELF) --objdump $(LLVM_OBJDUMP) \
		--exec $(PTLS_EXEC_ELF) --pie $(PTLS_PIE_ELF) --plain $(PTLS_PLAIN_ELF)
	@touch $@
endif
endif

.PHONY: all clean
all: $(PROGRAMS) $(RUNTIME_PROBE_ELF) $(ASLR_B_EXEC_ELF) $(PTLS_FIXTURES) \
     $(PTLS_CHECK_STAMP) $(DOOM_ARCHIVE)

$(OBJ_DIR)/%.o: %.c $(USER_COMPILE_KEY)
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(CFLAGS) -MD -MP -c $< -o $@

$(OBJ_DIR)/%.o: %.S $(USER_COMPILE_KEY)
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(CFLAGS) -MD -MP -c $< -o $@

# Compile shared sources once with global flags. App flags affect local sources.
user_objects = $(addprefix $(OBJ_DIR)/,$(addsuffix .o,$(basename $(1))))
USER_OBJECTS := $(call user_objects,$(USER_FLAT_SOURCES))

define flat_program
$(BUILD_DIR)/$(1).elf: $(OBJ_DIR)/$(1).o $(COMMON_LINK_DEPS) $(TARGET_LIBC)
	@mkdir -p $$(dir $$@)
	$$(TARGET_LD) $$(USER_RAW_LDFLAGS) -o $$@ $$(CRT0_OBJ) $(OBJ_DIR)/$(1).o -lc
	$$(OBJ_CPY) --strip-debug $$@
endef
$(foreach app,$(USER_FLAT_PROGRAMS),$(eval $(call flat_program,$(app))))

define directory_program
USER_APP_$(1)_OBJECTS := $(call user_objects,$(USER_APP_$(1)_SOURCES)) $(USER_APP_$(1)_EXTRA_OBJS)
USER_OBJECTS += $$(filter %.o,$$(USER_APP_$(1)_OBJECTS))
$$(OBJ_DIR)/$(1)/.app_key: USER_COMPILE_KEY_FORCE
	@mkdir -p $$(dir $$@)
	@printf '%s\n' 'cflags=$(strip $$(USER_APP_$(1)_CFLAGS)) ldflags=$(strip $$(USER_APP_$(1)_LDFLAGS))' > $$@.new
	@cmp -s $$@.new $$@ || mv -f $$@.new $$@
$(call user_objects,$(filter $(1)/%,$(USER_APP_$(1)_SOURCES))): override CFLAGS += $(USER_APP_$(1)_CFLAGS)
$(call user_objects,$(USER_APP_$(1)_SOURCES)): $(1)/app.mk $$(OBJ_DIR)/$(1)/.app_key
$(BUILD_DIR)/$(1).elf: $$(USER_APP_$(1)_OBJECTS) $(COMMON_LINK_DEPS) $(1)/app.mk $$(call user_lib_dep,$$(USER_APP_$(1)_LDLIBS)) $$(OBJ_DIR)/$(1)/.app_key $(TARGET_LIBC)
	@mkdir -p $$(dir $$@)
	$$(TARGET_LD) $$(USER_RAW_LDFLAGS) $(USER_APP_$(1)_LDFLAGS) -o $$@ $$(CRT0_OBJ) $$(USER_APP_$(1)_OBJECTS) $(USER_APP_$(1)_LDLIBS) -lc
	$$(OBJ_CPY) --strip-debug $$@
endef
$(foreach app,$(USER_DIRECTORY_PROGRAMS),$(eval $(call directory_program,$(app))))

.SECONDARY: $(USER_OBJECTS)
-include $(filter %.d,$(USER_OBJECTS:.o=.d))

clean:
	rm -rf $(BUILD_DIR)
