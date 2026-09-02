# ── Image / rootfs contract ───────────────────────────────────
# image.mk only consumes artifacts — it never calls kernel/libc/user builds
# (spec: component boundary). The x86 disk image is built from the
# version-controlled rootfs manifest (config/rootfs.mk) into a fresh staging
# tree ($(ROOTFS_STAGING)); mkdisk then fills a GPT dual-partition image from
# the generated file/symlink manifest. The aarch64 bring-up image
# (uefi-bringup capability) is a plain 64 MiB FAT carrying exactly
# BOOTAA64.EFI + kernel.elf + firmware — no BusyBox/mbedTLS/user/rootfs.

# ── Host mkdisk binary (profile-private: build/<profile>/host-tools) ──
HOST_MKDISK := $(BUILD_DIR)/host-tools/mkdisk

$(HOST_MKDISK): tools/mkdisk.c tools/Makefile
	@mkdir -p $(dir $@)
	@$(MAKE) -C tools HOST_TOOLS_DIR=$(BUILD_DIR)/host-tools

ifeq ($(filter rootfs,$(PROFILE_CAPABILITIES)),rootfs)

# ── Rootfs manifest (x86) ───────────────────────────────────
# config/rootfs.mk is version-controlled and lists every image input as
# dest=source:mode (files) and dest=target (BusyBox applet symlinks). This
# rule stages a FRESH tree (rm -rf of rootfs.next) so entries removed from
# the manifest never linger, then emits the tab-separated manifest that
# mkdisk consumes:
#   file<TAB>dest<TAB>staged-source<TAB>mode
#   symlink<TAB>dest<TAB>target
include $(OS01_ROOT)/config/rootfs.mk

ROOTFS_STAGING  := $(BUILD_DIR)/image/rootfs.next
ROOTFS_MANIFEST := $(BUILD_DIR)/image/rootfs.manifest

$(ROOTFS_MANIFEST): $(OS01_ROOT)/config/rootfs.mk \
		$(USER_ARTIFACTS) $(USER_ARTIFACT_DIR)/busybox.elf \
		$(KERNEL_ARTIFACT) $(INITTAB_FILE)
	@rm -rf $(ROOTFS_STAGING)
	@mkdir -p $(ROOTFS_STAGING) $(dir $@)
	@rm -f $@
	@for item in $(ROOTFS_FILES); do \
	  dest=$${item%%=*}; \
	  rest=$${item#*=}; \
	  src=$${rest%%:*}; \
	  mode=$${rest#*:}; \
	  mkdir -p "$(ROOTFS_STAGING)/$$(dirname "$$dest")"; \
	  cp "$$src" "$(ROOTFS_STAGING)/$$dest"; \
	  chmod "$$mode" "$(ROOTFS_STAGING)/$$dest"; \
	  printf 'file\t%s\t%s\t%s\n' "$$dest" "$(ROOTFS_STAGING)/$$dest" "$$mode" >> $@; \
	done
	@for item in $(ROOTFS_SYMLINKS); do \
	  dest=$${item%%=*}; \
	  target=$${item#*=}; \
	  printf 'symlink\t%s\t%s\n' "$$dest" "$$target" >> $@; \
	done
	@test -s $@

$(BUILD_DIR)/image/disk.img: $(ROOTFS_MANIFEST) $(UEFI_EFI) $(HOST_MKDISK)
	@mkdir -p $(dir $@) $(BUILD_DIR)/image/tmp
	@$(HOST_MKDISK) --output $@ --efi $(UEFI_EFI) \
	  --temp-dir $(BUILD_DIR)/image/tmp --rootfs-manifest $(ROOTFS_MANIFEST)

# Project-root disk.img is a one-way, content-guarded copy of the profile's
# disk artifact (legacy entry point; Task 7 moves the run/test recipes).
disk.img: $(BUILD_DIR)/image/disk.img
	@cmp -s $(BUILD_DIR)/image/disk.img $@ || cp $(BUILD_DIR)/image/disk.img $@

endif

ifeq ($(filter uefi-bringup,$(PROFILE_CAPABILITIES)),uefi-bringup)

# ── aarch64 kernel artifact ─────────────────────────────────
# No sysroot, no lease, no `lib` dependency: aarch64 does not consume the
# sysroot. The sub-make is incremental; the artifact copy is content-guarded.
$(BUILD_DIR)/artifacts/kernel.elf: FORCE
	@mkdir -p $(dir $@)
	@$(call os01_submake,kernel,all ARCH=aarch64)
	@cmp -s $(KERNEL_BUILD_DIR)/kernel.elf $@ || cp $(KERNEL_BUILD_DIR)/kernel.elf $@

# ── aarch64 UEFI firmware (profile build dir) ───────────────
$(AARCH64_UEFI_FIRMWARE): $(AARCH64_UEFI_FIRMWARE_SOURCE)
	@mkdir -p $(dir $@)
	@cmp -s $< $@ || cp $< $@

# ── aarch64 UEFI bring-up image (64 MiB FAT) ────────────────
$(BUILD_DIR)/image/aarch64-uefi.img: $(BUILD_DIR)/artifacts/uefi/BOOTAA64.EFI \
		$(BUILD_DIR)/artifacts/kernel.elf $(AARCH64_UEFI_FIRMWARE)
	@mkdir -p $(dir $@)
	@rm -f $@
	@truncate -s 64M $@
	@mkfs.fat -F 32 $@
	@mmd -i $@ ::/EFI ::/EFI/BOOT
	@mcopy -i $@ $(BUILD_DIR)/artifacts/uefi/BOOTAA64.EFI ::/EFI/BOOT/BOOTAA64.EFI
	@mcopy -i $@ $(BUILD_DIR)/artifacts/kernel.elf ::/kernel.elf

endif
