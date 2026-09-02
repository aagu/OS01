# ── aarch64 link / UEFI / run parameters ─────────────────────────
# These paths follow the profile-private layout (build/<profile>). The EFI
# app artifact and the bring-up image are produced by mk/components/uefi.mk
# and mk/components/image.mk respectively.
AARCH64_UEFI_APP      := $(BUILD_DIR)/artifacts/uefi/BOOTAA64.EFI
AARCH64_UEFI_DISK     := $(BUILD_DIR)/image/aarch64-uefi.img
AARCH64_UEFI_FIRMWARE := $(BUILD_DIR)/image/QEMU_EFI.fd
AARCH64_KERNEL_ELF    := $(KERNEL_BUILD_DIR)/kernel.elf
AARCH64_HEAD_OBJECT   := $(KERNEL_BUILD_DIR)/arch/aarch64/head.o
