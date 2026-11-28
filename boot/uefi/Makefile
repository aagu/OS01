# boot/uefi/Makefile — UEFI bootloader build wrapper.
#
# Builds the EFI applications from the shared main.c + per-arch hooks:
#   ARCH=x86_64   -> $(UEFI_BUILD_DIR)/BOOTX64.EFI
#   ARCH=aarch64  -> $(UEFI_BUILD_DIR)/BOOTAA64.EFI
#
# Profile-only: the root EFI artifact rule (mk/components/uefi.mk) is the only
# caller. The adapter stages the UEFI runtime at $(UEFI_RUNTIME_DIR) (copy +
# tracked patches); this Makefile only compiles the boot sources against it.
# ARCH is passed by the root rule — this Makefile never selects an
# architecture by default.

LOCAL_MAKEFILE := $(abspath $(lastword $(MAKEFILE_LIST)))
ROOT := $(abspath $(dir $(LOCAL_MAKEFILE))../..)

# ── Profile-only contract ───────────────────────────────────
# The root Makefile is the only supported entry; component Makefiles
# require OS01_PROFILE_FILE at parse time. OUTDIR / RUNTIME_DIR come from
# the selected profile (UEFI_BUILD_DIR / UEFI_RUNTIME_DIR).
ifeq ($(strip $(OS01_PROFILE_FILE)),)
$(error invoke from the repository root: make PROFILE=x86_64-clang <target>)
endif
ifeq ($(wildcard $(OS01_PROFILE_FILE)),)
$(error OS01_PROFILE_FILE='$(OS01_PROFILE_FILE)' does not exist; invoke from the repository root)
endif
include $(OS01_PROFILE_FILE)
OUTDIR := $(UEFI_BUILD_DIR)
RUNTIME_DIR := $(UEFI_RUNTIME_DIR)

ifeq ($(ARCH),x86_64)
TARGET := BOOTX64.EFI
SRCS := $(ROOT)/boot/uefi/main.c \
        $(ROOT)/boot/uefi/boot_config.c \
        $(ROOT)/boot/uefi/boot_device.c \
        $(ROOT)/boot/uefi/arch/x86_64/boot.c
else ifeq ($(ARCH),aarch64)
TARGET := BOOTAA64.EFI
SRCS := $(ROOT)/boot/uefi/main.c \
        $(ROOT)/boot/uefi/boot_config.c \
        $(ROOT)/boot/uefi/boot_device.c \
        $(ROOT)/boot/uefi/arch/aarch64/boot.c \
        $(ROOT)/boot/uefi/arch/aarch64/boot_memory.c \
        $(ROOT)/boot/uefi/arch/aarch64/elf.c \
        $(ROOT)/boot/uefi/arch/aarch64/fdt_early.c \
        $(ROOT)/boot/uefi/arch/aarch64/acpi_early.c \
        $(ROOT)/boot/uefi/arch/aarch64/rp1_pci.c \
        $(ROOT)/kernel/arch/aarch64/platform/acpi_parse.c \
        $(ROOT)/boot/uefi/arch/aarch64/handoff.S
# LMA Task 2: the boot block base travels into the posix-uefi compile
# via the environment CFLAGS contract. The inner make cannot see a
# command-line flag change, so the .aarch64_layout_cfg key below
# (cmp-guarded, deletes the inner objects on change) is what actually
# invalidates the app objects and the EFI.
UEFI_LAYOUT_HEADERS := $(ROOT)/kernel/include/arch/aarch64/boot_layout.h \
                       $(ROOT)/kernel/include/arch/aarch64/handoff_layout.h \
                       $(ROOT)/kernel/include/arch/aarch64/fdt_address.h \
                       $(ROOT)/kernel/include/arch/aarch64/uefi_memory_map.h \
                       $(ROOT)/boot/uefi/arch/aarch64/loader.h \
                       $(ROOT)/boot/uefi/arch/aarch64/rp1_pci.h
override UEFI_RUNTIME_CFLAGS += $(AARCH64_LAYOUT_CFLAGS)
else
$(error "unsupported ARCH: $(ARCH)")
endif

# ── Runtime environment contract (spec: "删除 runtime Makefile overlay") ──
# The root adapter (mk/components/uefi.mk) passes the FINAL values explicitly;
# these ?= defaults exist so a direct profile-internal invocation
# (`make -C boot/uefi OS01_PROFILE_FILE=...`) without the explicit args still
# behaves identically (standalone invocation without a profile is a parse
# error). The
# runtime invocation starts the outer posix-uefi Make with
# env CFLAGS="$(UEFI_RUNTIME_CFLAGS)" MAKE="$(UEFI_RUNTIME_MAKE)": upstream's
# `CFLAGS +=` (outer + uefi/ Makefiles) appends the UTF-8 macro to both
# compile classes, and upstream's recursive `$(MAKE)` for uefi/libuefi.a runs
# with OUTDIR= so runtime archive objects land in the runtime dir while the
# outer application invocation keeps OUTDIR=$(UEFI_BUILD_DIR)/.
# The default must mirror mk/components/uefi.mk exactly, otherwise the
# "behaves identically" contract above is false: posix-uefi's own
# -I/usr/include picks up glibc's stdint.h, which on Ubuntu's multiarch
# layout needs bits/libc-header-start.h from
# /usr/include/x86_64-linux-gnu/bits/ — a path clang does not search —
# so the build dies with "bits/libc-header-start.h file not found" unless
# OS01's freestanding headers are searched first.
UEFI_RUNTIME_CFLAGS ?= -DUEFI_NO_UTF8 -D__need_wchar_t -I$(ROOT)/libc/include
UEFI_RUNTIME_MAKE ?= $(MAKE) OUTDIR=

# ── UEFI Clang/COFF contract (spec §6) ────────────────────────
# Both x86_64 and aarch64 pass explicit CC=clang LD=lld -flavor link to the
# copied posix-uefi runtime, so the runtime never falls back to its
# USE_GCC=1 path (which on e.g. Termux — clang lives outside the runtime's
# hard-coded wildcards at uefi/Makefile:32 — would set CC=gcc and run the
# broken `--subsystem=10` objcopy step at uefi/Makefile:96 instead of the
# Clang + lld chain that already produces the EFI binary directly). Parse-time
# directives live at top level: GNU make rejects variable assignments
# interleaved with recipe lines, and these must fire even under `make -n`.
#
# CLANG comes from the profile (mk/profiles/x86_64-clang.mk includes
# mk/toolchains/clang.mk which sets CLANG; aarch64-clang.mk may set CC
# directly to 'clang -target ...' — extract the bare binary via
# $(firstword) so a CC like 'clang -target aarch64-none-elf' resolves to
# 'clang' for the UEFI_CLANG sanity check and the runtime invocation).
UEFI_CLANG ?= $(firstword $(CC))
UEFI_CLANG_ID := $(shell $(UEFI_CLANG) --version 2>/dev/null | head -1)
ifeq ($(findstring clang,$(UEFI_CLANG_ID)),)
$(error UEFI_CLANG='$(UEFI_CLANG)' (from CC='$(CC)') is not Clang)
endif
UEFI_CLANG_RESOURCE_DIR := $(shell $(UEFI_CLANG) -print-resource-dir 2>/dev/null)
ifeq ($(strip $(UEFI_CLANG_RESOURCE_DIR)),)
$(error UEFI_CLANG='$(UEFI_CLANG)' is unavailable or lacks -print-resource-dir)
endif
UEFI_LD_TOOL ?= $(TARGET_LD)
$(eval $(call require_program,UEFI_LD_TOOL))
UEFI_LD := $(UEFI_LD_TOOL) -flavor link
# Block outer ELF CFLAGS/LDFLAGS from the environment so the inner `+=`
# flags (--target=$(TARGET_TRIPLE) and the COFF linker flags at
# posix-uefi/uefi/Makefile:64) survive: a literal CFLAGS= on the command
# line would wipe them instead (GNU make ignores += appends to
# command-line variables).
unexport CFLAGS LDFLAGS
# Serial inner make (G6): the runtime's $(TARGET).so depends on
# OUTDIR-prefixed object paths that NO rule can build — they exist only as
# side effects of building the plain $(OBJS) targets first (their `%.o: %.c`
# recipes redirect output into OUTDIR). Serial order builds $(OBJS) before
# the .so's prerequisites are checked, so the side-effect files are present
# and need no rule. Under -j the .so prerequisite check races ahead of the
# side effects and dies with "No rule to make target". The UEFI app is a
# handful of files; -j1 costs nothing and keeps the outer build parallel.
UEFI_MAKE := $(MAKE) -j1 -C $(RUNTIME_DIR) ARCH=$(ARCH) TARGET=$(TARGET) OUTDIR=$(OUTDIR)/ SRCS="$(SRCS)" \
	MAKEOVERRIDES= USE_GCC= CC="$(UEFI_CLANG)" LD="$(UEFI_LD)"

.PHONY: all clean
all: $(OUTDIR)/$(TARGET)

# Layout key (aarch64 only): carries the effective boot block base.
# When the value changes, the inner objects under $(OUTDIR) are deleted
# so the inner make must recompile them with the new -D — re-touching an
# outer stamp alone would leave the old objects (and thus the old EFI)
# in place. Content-guarded like the kernel build's stamps: an A→B→A
# flip invalidates on every content change even though the file name
# never varies.
ifneq ($(strip $(UEFI_LAYOUT_HEADERS)),)
$(OUTDIR)/.aarch64_layout_cfg: FORCE
	@mkdir -p $(OUTDIR)
	@printf '%s\n' 'base=$(AARCH64_BOOT_BLOCK_BASE)' > $@.new
	@sha256sum $(UEFI_LAYOUT_HEADERS) >> $@.new
	@cmp -s $@.new $@ || { mv -f $@.new $@; find $(OUTDIR) -name '*.o' -delete; }
AARCH64_LAYOUT_KEY := $(OUTDIR)/.aarch64_layout_cfg
.PHONY: FORCE
FORCE:
endif

$(OUTDIR)/$(TARGET): $(SRCS) $(ROOT)/boot/uefi/arch/arch.h \
		$(ROOT)/boot/uefi/boot_config.h \
		$(ROOT)/boot/uefi/boot_device.h \
		$(ROOT)/kernel/include/core/bootinfo.h \
		$(UEFI_LAYOUT_HEADERS) \
		$(AARCH64_LAYOUT_KEY) \
		$(UEFI_RUNTIME_STAMP)
	# The runtime is already staged + patched by the uefi.mk adapter; never
	# copy or mutate it here.
	@test -d $(RUNTIME_DIR) || { \
		echo "ERROR: UEFI runtime not staged at $(RUNTIME_DIR) (run the uefi.mk adapter)"; \
		false; \
	}
	# The deep mkdir paths below are harmless: they mirror where the inner
	# make places per-source objects (OUTDIR + absolute source path), the
	# same proven pattern the aarch64 wrapper used.
	@$(foreach source,$(SRCS),mkdir -p $(OUTDIR)$(dir $(source));)
	env CFLAGS="$(UEFI_RUNTIME_CFLAGS)" MAKE="$(UEFI_RUNTIME_MAKE)" $(UEFI_MAKE)

clean:
	rm -rf $(RUNTIME_DIR) $(OUTDIR)
