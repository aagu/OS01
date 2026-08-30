ROOT := $(abspath ../../..)
BUILD_DIR ?= $(ROOT)/build/aarch64
RUNTIME_DIR := $(BUILD_DIR)/uefi-runtime
OUTDIR := $(BUILD_DIR)/uefi
TARGET := BOOTAA64.EFI

# Default to this repository's submodule.  A worktree with that submodule left
# uninitialized can explicitly supply an external read-only runtime source:
# `make UEFI_RUNTIME_SOURCE=/absolute/path/to/posix-uefi/uefi ...`.
UEFI_RUNTIME_SOURCE ?= $(ROOT)/thirdpart/posix-uefi/uefi

SRCS := $(ROOT)/boot/uefi/aarch64/main.c \
        $(ROOT)/boot/uefi/aarch64/elf.c \
        $(ROOT)/boot/uefi/aarch64/handoff.S

.PHONY: all
all: $(OUTDIR)/$(TARGET)

$(OUTDIR)/$(TARGET): $(SRCS) $(ROOT)/boot/uefi/aarch64/loader.h \
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
	sed -i '/^CFLAGS += -fshort-wchar/i CFLAGS += -DUEFI_NO_UTF8' \
		$(RUNTIME_DIR)/uefi/Makefile
	@$(foreach source,$(SRCS),mkdir -p $(OUTDIR)$(dir $(source));)
	$(MAKE) -C $(RUNTIME_DIR) ARCH=aarch64 TARGET=$(TARGET) \
		OUTDIR=$(OUTDIR)/ SRCS="$(SRCS)"
