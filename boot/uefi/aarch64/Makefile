ROOT := $(abspath ../../..)
BUILD_DIR := $(ROOT)/build/aarch64
RUNTIME_DIR := $(BUILD_DIR)/uefi-runtime
OUTDIR := $(BUILD_DIR)/uefi
TARGET := BOOTAA64.EFI

# This worktree intentionally leaves thirdpart/posix-uefi uninitialized.
# The source checkout is read-only; each build stages only its UEFI runtime.
UEFI_RUNTIME_SOURCE ?= /home/aagu/OS01/thirdpart/posix-uefi/uefi

# Task 4 adds handoff.S when it implements the actual handoff transaction.
SRCS := $(ROOT)/boot/uefi/aarch64/main.c \
        $(ROOT)/boot/uefi/aarch64/elf.c

.PHONY: all
all: $(OUTDIR)/$(TARGET)

$(OUTDIR)/$(TARGET): $(SRCS) $(ROOT)/boot/uefi/aarch64/loader.h
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
	@$(foreach source,$(SRCS),mkdir -p $(OUTDIR)$(dir $(source));)
	$(MAKE) -C $(RUNTIME_DIR) ARCH=aarch64 TARGET=$(TARGET) \
		OUTDIR=$(OUTDIR)/ SRCS="$(SRCS)"
