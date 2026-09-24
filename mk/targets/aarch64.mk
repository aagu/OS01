# ── aarch64 link / UEFI / run parameters ─────────────────────────
# These paths follow the profile-private layout (build/<profile>). The EFI
# app artifact and the bring-up image are produced by mk/components/uefi.mk
# and mk/components/image.mk respectively.
# AAGU-5.6: image + firmware paths are variant-aware so WEAK build (variant=
# weak-selftest) lands under image/weak-selftest/ instead of clobbering the
# default STRONG/NONE image. Both rules in mk/components/image.mk now use
# these $(AARCH64_UEFI_*) variables as their target/dependency names.
AARCH64_UEFI_DISK     := $(BUILD_DIR)/image$(if $(KERNEL_VARIANT),/$(KERNEL_VARIANT))/aarch64-uefi.img
AARCH64_UEFI_FIRMWARE := $(BUILD_DIR)/image$(if $(KERNEL_VARIANT),/$(KERNEL_VARIANT))/QEMU_EFI.fd
AARCH64_KERNEL_ELF    := $(KERNEL_BUILD_DIR)/kernel.elf
AARCH64_HEAD_OBJECT   := $(KERNEL_BUILD_DIR)/arch/aarch64/head.o
