# boot/uefi/Makefile — unified UEFI bootloader build wrapper.
#
# Builds both EFI applications from the shared main.c + per-arch hooks:
#   make -C boot/uefi ARCH=x86_64    -> build/x86_64/uefi/BOOTX64.EFI
#   make -C boot/uefi ARCH=aarch64   -> build/aarch64/uefi/BOOTAA64.EFI
#
# Each ARCH gets a private copy of the posix-uefi runtime
# (build/<arch>/uefi-runtime/) compiled with UEFI_NO_UTF8, so objects
# never cross-pollinate between architectures.

ARCH ?= x86_64
ROOT := $(abspath ../..)
# require_program is a GNU make `define` in toolchain.mk — such defines are
# never exported to child makes, so include toolchain.mk here (it is
# location-independent) for the UEFI Clang contract below. x86_64 only:
# the aarch64 path must stay free of x86 toolchain variables.
ifeq ($(ARCH),x86_64)
include $(ROOT)/toolchain.mk
endif
BUILD_DIR := $(ROOT)/build/$(ARCH)
RUNTIME_DIR := $(BUILD_DIR)/uefi-runtime
OUTDIR := $(BUILD_DIR)/uefi

UEFI_RUNTIME_SOURCE ?= $(ROOT)/thirdpart/posix-uefi/uefi

ifeq ($(ARCH),x86_64)
TARGET := BOOTX64.EFI
SRCS := $(ROOT)/boot/uefi/main.c \
        $(ROOT)/boot/uefi/arch/x86_64/boot.c
else ifeq ($(ARCH),aarch64)
TARGET := BOOTAA64.EFI
SRCS := $(ROOT)/boot/uefi/main.c \
        $(ROOT)/boot/uefi/arch/aarch64/boot.c \
        $(ROOT)/boot/uefi/arch/aarch64/elf.c \
        $(ROOT)/boot/uefi/arch/aarch64/handoff.S
else
$(error "unsupported ARCH: $(ARCH)")
endif

# ── UEFI Clang/COFF contract (spec §6) ────────────────────────
# x86_64 owns UEFI Clang/LD validation and the override invocation; aarch64
# deliberately keeps the plain recursive call (the copied runtime
# auto-detects clang + `lld -flavor link`). Parse-time directives live at
# top level: GNU make rejects variable assignments interleaved with recipe
# lines, and these must fire even under `make -n`.
ifeq ($(ARCH),x86_64)
UEFI_CLANG ?= $(CLANG)
UEFI_CLANG_ID := $(shell $(UEFI_CLANG) --version 2>/dev/null | head -1)
ifeq ($(findstring clang,$(UEFI_CLANG_ID)),)
$(error UEFI_CLANG='$(UEFI_CLANG)' is not Clang)
endif
UEFI_CLANG_RESOURCE_DIR := $(shell $(UEFI_CLANG) -print-resource-dir 2>/dev/null)
ifeq ($(strip $(UEFI_CLANG_RESOURCE_DIR)),)
$(error UEFI_CLANG='$(UEFI_CLANG)' is unavailable or lacks -print-resource-dir)
endif
UEFI_LD_TOOL ?= $(TARGET_LD)
$(eval $(call require_program,UEFI_LD_TOOL))
UEFI_LD := $(UEFI_LD_TOOL) -flavor link
# Block outer ELF CFLAGS/LDFLAGS from the environment so the inner `+=`
# flags (--target=x86_64-pc-win32-coff and the COFF linker flags at
# uefi/Makefile:63-64) survive: a literal CFLAGS= on the command line would
# wipe them instead (GNU make ignores += appends to command-line variables).
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
else
UEFI_MAKE := $(MAKE) -j1 -C $(RUNTIME_DIR) ARCH=$(ARCH) TARGET=$(TARGET) OUTDIR=$(OUTDIR)/ SRCS="$(SRCS)"
endif

.PHONY: all clean clean-all
all: $(OUTDIR)/$(TARGET)

$(OUTDIR)/$(TARGET): $(SRCS) $(ROOT)/boot/uefi/arch/arch.h \
		$(ROOT)/kernel/include/kernel/bootinfo.h
	@test -f $(UEFI_RUNTIME_SOURCE)/Makefile || { \
		echo "ERROR: posix-uefi runtime is unavailable at $(UEFI_RUNTIME_SOURCE)"; \
		false; \
	}
	rm -rf $(RUNTIME_DIR) $(OUTDIR)
	mkdir -p $(RUNTIME_DIR) $(OUTDIR)
	cp -a $(UEFI_RUNTIME_SOURCE) $(RUNTIME_DIR)/uefi
	cp $(RUNTIME_DIR)/uefi/Makefile $(RUNTIME_DIR)/Makefile
	rm -f $(RUNTIME_DIR)/uefi/*.o $(RUNTIME_DIR)/uefi/*.a
	# Clang's int8_t is signed char; posix-uefi's legacy plain-char
	# fallback is a distinct C type and conflicts when bootinfo.h includes
	# Clang's <stdint.h> (notably on Termux Clang 21).
	sed -i 's/typedef char[[:space:]]*int8_t;/typedef signed char         int8_t;/' $(RUNTIME_DIR)/uefi/uefi.h
	sed -i 's/ARCH=$$(ARCH)/ARCH=$$(ARCH) OUTDIR=/' $(RUNTIME_DIR)/Makefile
	sed -i '/^CFLAGS += -fshort-wchar/i CFLAGS += -DUEFI_NO_UTF8' $(RUNTIME_DIR)/Makefile
	sed -i '/^CFLAGS += -fshort-wchar/i CFLAGS += -DUEFI_NO_UTF8' $(RUNTIME_DIR)/uefi/Makefile
	# The old x86 DEBUG=1 gate (the types[] memory-map print) was
	# intentionally not carried into the new tree — there is no -DDEBUG
	# consumer here, so no DEBUG injection. The deep mkdir paths below are
	# harmless: they mirror where the inner make places per-source objects
	# (OUTDIR + absolute source path), the same proven pattern the old
	# aarch64 wrapper used.
	@$(foreach source,$(SRCS),mkdir -p $(OUTDIR)$(dir $(source));)
	$(UEFI_MAKE)

OVMF.fd:
	wget -c https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd -O $@

clean:
	rm -rf $(RUNTIME_DIR) $(OUTDIR)

clean-all:
	rm -rf $(RUNTIME_DIR) $(OUTDIR)
	rm -f OVMF.fd
