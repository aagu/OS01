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

# ── Variant-scoped image dirs ─────────────────────────────────
# IMAGE_VARIANT (mk/project.mk) is the slug that isolates the image/manifest
# dirs so a variant build never touches the normal image:
#   (none)         → build/<profile>/image/            (the normal image)
#   systest        → build/<profile>/image/systest/
#   nettest        → build/<profile>/image/nettest/
#   inittab-test   → build/<profile>/image/inittab-test/
# NORMAL_IMAGE is the fixed normal-image path — the project-root disk.img
# compat copy and the test-integrity before/after checks always use it.
IMAGE_DIR       := $(BUILD_DIR)/image$(if $(IMAGE_VARIANT),/$(IMAGE_VARIANT))
NORMAL_IMAGE    := $(BUILD_DIR)/image/disk.img
NORMAL_IMAGE_DIR := $(BUILD_DIR)/image
DISK_IMG        := $(IMAGE_DIR)/disk.img
ROOTFS_STAGING  := $(IMAGE_DIR)/rootfs.next
ROOTFS_MANIFEST := $(IMAGE_DIR)/rootfs.manifest

# ── Rootfs manifest (x86) ───────────────────────────────────
# config/rootfs.mk is version-controlled and lists every image input as
# dest=source:mode (files) and dest=target (BusyBox applet entries). This
# rule stages a FRESH tree (rm -rf of rootfs.next) so entries removed from
# the manifest never linger, then emits the tab-separated manifest that
# mkdisk consumes:
#   file<TAB>dest<TAB>staged-source<TAB>mode
#
# PLAN DEVIATION (busybox copies instead of symlinks): the OS01 kernel has
# no symlink support in path lookup / exec (vfs_lookup returns the last
# component as-is; no follow_symlink exists), so an execve of a symlink
# fails with ENOEXEC. The pre-refactor build masked this by writing full
# busybox copies for every applet. Each ROOTFS_SYMLINKS entry is therefore
# staged as a regular-file COPY of the busybox artifact (same content,
# applet dispatched by argv[0]), and the manifest emits a `file` row. See
# the ledger ruling + docs/roadmap.md (kernel exec-symlink gap).
include $(OS01_ROOT)/config/rootfs.mk

$(ROOTFS_MANIFEST): $(OS01_ROOT)/config/rootfs.mk \
		$(USER_ARTIFACTS) $(USER_ARTIFACT_DIR)/busybox.elf \
		$(KERNEL_ARTIFACT) $(INITTAB_FILE)
	@rm -rf $(ROOTFS_STAGING)
	@mkdir -p $(ROOTFS_STAGING) $(dir $@)
	@rm -f $@.tmp
	@for item in $(ROOTFS_FILES); do \
	  dest=$${item%%=*}; \
	  rest=$${item#*=}; \
	  src=$${rest%%:*}; \
	  mode=$${rest#*:}; \
	  mkdir -p "$(ROOTFS_STAGING)/$$(dirname "$$dest")"; \
	  cp "$$src" "$(ROOTFS_STAGING)/$$dest"; \
	  chmod "$$mode" "$(ROOTFS_STAGING)/$$dest"; \
	  printf 'file\t%s\t%s\t%s\n' "$$dest" "$(ROOTFS_STAGING)/$$dest" "$$mode" >> $@.tmp; \
	done
	@for item in $(ROOTFS_SYMLINKS); do \
	  dest=$${item%%=*}; \
	  target=$${item#*=}; \
	  mkdir -p "$(ROOTFS_STAGING)/$$(dirname "$$dest")"; \
	  cp "$(USER_ARTIFACT_DIR)/busybox.elf" "$(ROOTFS_STAGING)/$$dest"; \
	  chmod 0755 "$(ROOTFS_STAGING)/$$dest"; \
	  printf 'file\t%s\t%s\t%s\n' "$$dest" "$(ROOTFS_STAGING)/$$dest" "0755" >> $@.tmp; \
	done
	@test -s $@.tmp
	@mv $@.tmp $@

$(DISK_IMG): $(ROOTFS_MANIFEST) $(UEFI_EFI) $(HOST_MKDISK)
	@$(MAKE) -C tools check-deps HOST_TOOLS_DIR=$(BUILD_DIR)/host-tools
	@mkdir -p $(dir $@) $(dir $@)tmp
	@$(HOST_MKDISK) --output $@ --efi $(UEFI_EFI) \
	  --temp-dir $(dir $@)tmp --rootfs-manifest $(ROOTFS_MANIFEST)

# Project-root disk.img is a one-way, content-guarded copy of the NORMAL
# image (legacy entry point) — never the variant image, regardless of
# variant switches. Only the DEFAULT_PROFILE owns this real compat copy;
# for any other rootfs profile `disk.img` is a PHONY alias that merely
# builds $(NORMAL_IMAGE) — it never creates or updates the project-root
# disk.img, and can never be satisfied by a pre-existing root disk.img's
# mtime (phony targets are always remade, and there is no copy recipe).
ifeq ($(PROFILE),$(DEFAULT_PROFILE))
disk.img: $(NORMAL_IMAGE)
	@cmp -s $(NORMAL_IMAGE) $@ || cp $(NORMAL_IMAGE) $@
else
.PHONY: disk.img
disk.img: $(NORMAL_IMAGE)
endif

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
# Mirrors the x86 OVMF contract (mk/components/run.mk):
#   AARCH64_UEFI_FIRMWARE_SOURCE accepts ONLY an https:// URL
#   (wget to "$@.tmp", then atomic rename) or an existing absolute local
#   path (content-guarded copy). Every other value is rejected with a clear
#   error before any download/copy, so a missing or misspelled source
#   never reaches QEMU and never leaves a half-written file (and never
#   reaches the mv). Capability-gated on uefi-bringup: the aarch64-only
#   AARCH64_UEFI_FIRMWARE variable is undefined for x86_64-clang, so
#   without the guard this rule expands to an empty-target rule with a
#   recipe — a make-version-sensitive parse hazard.
ifeq ($(filter uefi-bringup,$(PROFILE_CAPABILITIES)),uefi-bringup)
$(AARCH64_UEFI_FIRMWARE):
	@set -e; \
	mkdir -p "$(dir $@)"; \
	case "$(AARCH64_UEFI_FIRMWARE_SOURCE)" in \
	https://*) \
	  wget -q -O "$@.tmp" "$(AARCH64_UEFI_FIRMWARE_SOURCE)"; \
	  mv "$@.tmp" "$@";; \
	/*) \
	  test -f "$(AARCH64_UEFI_FIRMWARE_SOURCE)" || { \
	    echo "ERROR: AARCH64_UEFI_FIRMWARE_SOURCE '$(AARCH64_UEFI_FIRMWARE_SOURCE)' is not an existing file" >&2; \
	    exit 1; }; \
	  cmp -s "$(AARCH64_UEFI_FIRMWARE_SOURCE)" "$@" || cp "$(AARCH64_UEFI_FIRMWARE_SOURCE)" "$@";; \
	*) \
	  echo "ERROR: AARCH64_UEFI_FIRMWARE_SOURCE '$(AARCH64_UEFI_FIRMWARE_SOURCE)' must be an https:// URL or an existing absolute local file path" >&2; \
	  exit 1;; \
	esac

# $(AARCH64_UEFI_FIRMWARE) is absolute (BUILD_DIR is profile-absolute), so the
# same on-disk file is also reachable through its relative spelling. This
# alias mirrors the x86 `build/<profile>/firmware/OVMF.fd` rule.
build/$(PROFILE)/firmware/QEMU_EFI.fd: $(AARCH64_UEFI_FIRMWARE)
endif

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
