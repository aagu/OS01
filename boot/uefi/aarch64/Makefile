ROOT := $(abspath ../../..)
BUILD_DIR ?= $(ROOT)/build/aarch64
RUNTIME_DIR := $(BUILD_DIR)/uefi-runtime
OUTDIR := $(BUILD_DIR)/uefi
TARGET := BOOTAA64.EFI
APP_CFLAGS ?=
AARCH64_UEFI_CFLAGS := -DUEFI_NO_UTF8 $(APP_CFLAGS)

NEGATIVE_DIR := $(ROOT)/build/aarch64/uefi-negative
NEGATIVE_BAD_BUILD := $(NEGATIVE_DIR)/bad-elf-build
NEGATIVE_OVERFLOW_BUILD := $(NEGATIVE_DIR)/overflow-build
NEGATIVE_BAD_DISK := $(NEGATIVE_DIR)/bad-elf.img
NEGATIVE_RANGE_DISK := $(NEGATIVE_DIR)/handoff-crossing.img
NEGATIVE_OVERFLOW_DISK := $(NEGATIVE_DIR)/overflow.img
NEGATIVE_KERNEL := $(ROOT)/build/aarch64/kernel/kernel.elf
NEGATIVE_FIRMWARE_SOURCE ?= /usr/share/edk2/aarch64/QEMU_EFI.fd
NEGATIVE_QEMU ?= qemu-system-aarch64
NEGATIVE_TIMEOUT ?= 20s

# This worktree intentionally leaves thirdpart/posix-uefi uninitialized.
# The source checkout is read-only; each build stages only its UEFI runtime.
UEFI_RUNTIME_SOURCE ?= /home/aagu/OS01/thirdpart/posix-uefi/uefi

SRCS := $(ROOT)/boot/uefi/aarch64/main.c \
        $(ROOT)/boot/uefi/aarch64/elf.c \
        $(ROOT)/boot/uefi/aarch64/handoff.S

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
	sed -i '/^CFLAGS += -fshort-wchar/i CFLAGS += $$(APP_CFLAGS)' $(RUNTIME_DIR)/Makefile
	sed -i '/^CFLAGS += -fshort-wchar/i CFLAGS += $$(APP_CFLAGS)' \
		$(RUNTIME_DIR)/uefi/Makefile
	@$(foreach source,$(SRCS),mkdir -p $(OUTDIR)$(dir $(source));)
	$(MAKE) -C $(RUNTIME_DIR) ARCH=aarch64 TARGET=$(TARGET) \
		OUTDIR=$(OUTDIR)/ SRCS="$(SRCS)" \
		APP_CFLAGS="$(AARCH64_UEFI_CFLAGS)"

.PHONY: test-negative
test-negative:
	rm -rf $(NEGATIVE_DIR)
	mkdir -p $(NEGATIVE_DIR)
	$(MAKE) -C $(ROOT)/kernel ARCH=aarch64
	$(MAKE) -B BUILD_DIR=$(NEGATIVE_BAD_BUILD)
	dd if=/dev/zero of=$(NEGATIVE_DIR)/bad-kernel.elf bs=64 count=1 status=none
	truncate -s 64M $(NEGATIVE_BAD_DISK)
	mkfs.fat -F 32 $(NEGATIVE_BAD_DISK)
	mmd -i $(NEGATIVE_BAD_DISK) ::/EFI ::/EFI/BOOT
	mcopy -i $(NEGATIVE_BAD_DISK) $(NEGATIVE_BAD_BUILD)/uefi/$(TARGET) ::/EFI/BOOT/$(TARGET)
	mcopy -i $(NEGATIVE_BAD_DISK) $(NEGATIVE_DIR)/bad-kernel.elf ::/kernel.elf
	cp $(NEGATIVE_FIRMWARE_SOURCE) $(NEGATIVE_DIR)/bad-elf-QEMU_EFI.fd
	timeout $(NEGATIVE_TIMEOUT) $(NEGATIVE_QEMU) \
		-M virt,gic-version=2 -cpu cortex-a53 -smp 1 -m 512M \
		-drive if=pflash,format=raw,file=$(NEGATIVE_DIR)/bad-elf-QEMU_EFI.fd \
		-drive if=none,file=$(NEGATIVE_BAD_DISK),format=raw,id=disk \
		-device virtio-blk-device,drive=disk -serial stdio -display none \
		-no-reboot -no-shutdown >$(NEGATIVE_DIR)/bad-elf.log 2>&1 || \
		{ rc=$$?; test $$rc -eq 124; }
	grep -F 'UEFI-A64: bad ELF' $(NEGATIVE_DIR)/bad-elf.log
	! grep -F 'OS01 aarch64 phase1 boot ok' $(NEGATIVE_DIR)/bad-elf.log
	! grep -F 'UEFI-A64: handoff success' $(NEGATIVE_DIR)/bad-elf.log
	cp $(NEGATIVE_KERNEL) $(NEGATIVE_DIR)/handoff-crossing-kernel.elf
	printf '\360\377\035\100\000\000\000\000' | dd \
		of=$(NEGATIVE_DIR)/handoff-crossing-kernel.elf bs=1 seek=200 \
		conv=notrunc status=none
	truncate -s 64M $(NEGATIVE_RANGE_DISK)
	mkfs.fat -F 32 $(NEGATIVE_RANGE_DISK)
	mmd -i $(NEGATIVE_RANGE_DISK) ::/EFI ::/EFI/BOOT
	mcopy -i $(NEGATIVE_RANGE_DISK) $(NEGATIVE_BAD_BUILD)/uefi/$(TARGET) ::/EFI/BOOT/$(TARGET)
	mcopy -i $(NEGATIVE_RANGE_DISK) $(NEGATIVE_DIR)/handoff-crossing-kernel.elf ::/kernel.elf
	cp $(NEGATIVE_FIRMWARE_SOURCE) $(NEGATIVE_DIR)/handoff-crossing-QEMU_EFI.fd
	timeout $(NEGATIVE_TIMEOUT) $(NEGATIVE_QEMU) \
		-M virt,gic-version=2 -cpu cortex-a53 -smp 1 -m 512M \
		-drive if=pflash,format=raw,file=$(NEGATIVE_DIR)/handoff-crossing-QEMU_EFI.fd \
		-drive if=none,file=$(NEGATIVE_RANGE_DISK),format=raw,id=disk \
		-device virtio-blk-device,drive=disk -serial stdio -display none \
		-no-reboot -no-shutdown >$(NEGATIVE_DIR)/handoff-crossing.log 2>&1 || \
		{ rc=$$?; test $$rc -eq 124; }
	grep -F 'UEFI-A64: bad ELF' $(NEGATIVE_DIR)/handoff-crossing.log
	! grep -F 'OS01 aarch64 phase1 boot ok' $(NEGATIVE_DIR)/handoff-crossing.log
	! grep -F 'UEFI-A64: handoff success' $(NEGATIVE_DIR)/handoff-crossing.log
	$(MAKE) -B BUILD_DIR=$(NEGATIVE_OVERFLOW_BUILD) \
		APP_CFLAGS=-DAARCH64_TEST_FORCE_HANDOFF_OVERFLOW=1
	truncate -s 64M $(NEGATIVE_OVERFLOW_DISK)
	mkfs.fat -F 32 $(NEGATIVE_OVERFLOW_DISK)
	mmd -i $(NEGATIVE_OVERFLOW_DISK) ::/EFI ::/EFI/BOOT
	mcopy -i $(NEGATIVE_OVERFLOW_DISK) $(NEGATIVE_OVERFLOW_BUILD)/uefi/$(TARGET) ::/EFI/BOOT/$(TARGET)
	mcopy -i $(NEGATIVE_OVERFLOW_DISK) $(NEGATIVE_KERNEL) ::/kernel.elf
	cp $(NEGATIVE_FIRMWARE_SOURCE) $(NEGATIVE_DIR)/overflow-QEMU_EFI.fd
	timeout $(NEGATIVE_TIMEOUT) $(NEGATIVE_QEMU) \
		-M virt,gic-version=2 -cpu cortex-a53 -smp 1 -m 512M \
		-drive if=pflash,format=raw,file=$(NEGATIVE_DIR)/overflow-QEMU_EFI.fd \
		-drive if=none,file=$(NEGATIVE_OVERFLOW_DISK),format=raw,id=disk \
		-device virtio-blk-device,drive=disk -serial stdio -display none \
		-no-reboot -no-shutdown >$(NEGATIVE_DIR)/overflow.log 2>&1 || \
		{ rc=$$?; test $$rc -eq 124; }
	grep -F 'UEFI-A64: handoff overflow' $(NEGATIVE_DIR)/overflow.log
	! grep -F 'OS01 aarch64 phase1 boot ok' $(NEGATIVE_DIR)/overflow.log
	! grep -F 'UEFI-A64: handoff success' $(NEGATIVE_DIR)/overflow.log
	! grep -F 'UEFI-A64: load allocation failed' $(NEGATIVE_DIR)/overflow.log
