# ── aarch64 link / UEFI / run parameters ─────────────────────────
# These paths follow the profile-private layout (build/<profile>). The
# legacy root recipes still define their own AARCH64_* paths over these
# for one release (the root Makefile is parsed after the profile and its
# immediate := assignments win); later tasks rewire the aarch64 targets
# onto this contract.
AARCH64_UEFI_APP      := $(UEFI_BUILD_DIR)/BOOTAA64.EFI
AARCH64_UEFI_DISK     := $(BUILD_DIR)/image/aarch64-uefi.img
AARCH64_UEFI_FIRMWARE := $(BUILD_DIR)/QEMU_EFI.fd
AARCH64_KERNEL_ELF    := $(KERNEL_BUILD_DIR)/kernel.elf
AARCH64_HEAD_OBJECT   := $(KERNEL_BUILD_DIR)/arch/aarch64/head.o
