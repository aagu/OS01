# ── aarch64 link / UEFI / run parameters ─────────────────────────
# These paths follow the profile-private layout (build/<profile>). The EFI
# app artifact and the bring-up image are produced by mk/components/uefi.mk
# and mk/components/image.mk respectively.
# Image paths are variant-aware so variant builds land under their own
# image/<variant>/ directory without clobbering each other.
# Firmware is profile-private and shared across all variants (matching x86 OVMF).
AARCH64_UEFI_DISK     := $(BUILD_DIR)/image$(if $(KERNEL_VARIANT),/$(KERNEL_VARIANT))/aarch64-uefi.img
AARCH64_UEFI_FIRMWARE ?= $(BUILD_DIR)/firmware/QEMU_EFI.fd
AARCH64_KERNEL_ELF    := $(KERNEL_BUILD_DIR)/kernel.elf
AARCH64_HEAD_OBJECT   := $(KERNEL_BUILD_DIR)/arch/aarch64/head.o

# Helper functions for cross-variant artifact paths consumed by test runners
# in mk/components/run.mk when the outer make has no KERNEL_VARIANT set.
aarch64_variant_disk   = $(BUILD_DIR)/image/$(1)/aarch64-uefi.img
aarch64_variant_kernel = $(BUILD_DIR)/kernel/$(1)/kernel.elf
