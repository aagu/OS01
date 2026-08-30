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
	$(MAKE) -C $(RUNTIME_DIR) ARCH=$(ARCH) TARGET=$(TARGET) \
		OUTDIR=$(OUTDIR)/ SRCS="$(SRCS)"

OVMF.fd:
	wget -c https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd -O $@

clean:
	rm -rf $(RUNTIME_DIR) $(OUTDIR)

clean-all:
	rm -rf $(RUNTIME_DIR) $(OUTDIR)
	rm -f OVMF.fd
