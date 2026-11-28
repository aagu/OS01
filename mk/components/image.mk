# ── Image / rootfs contract ───────────────────────────────────
# image.mk only consumes artifacts — it never calls kernel/libc/user builds
# (spec: component boundary). The x86 disk image is built from the
# version-controlled rootfs manifest (config/rootfs.mk) into a fresh staging
# tree ($(ROOTFS_STAGING)); mkdisk then fills a GPT dual-partition image from
# the generated file/symlink manifest. The aarch64 image (uefi capability) is
# the same mkdisk GPT dual-partition layout: ESP FAT32 (BOOTAA64.EFI +
# kernel.elf) → /boot and an ext2 rootfs → / — since 2026-11-21 the product
# variant ships the product userspace (/bin/init, /bin/terminal, /bin/busybox
# with a real /bin/sh symlink, /bin/test_fp, /etc/inittab) through an
# aarch64-specific small manifest (no mbedTLS/GUI apps/rootfs.mk manifest on
# this arch); before that date it was a whole-disk FAT32 superfloppy.
# AARCH64_UEFI_DISK
# is defined only by mk/targets/aarch64.mk (aarch64-clang profile), so the
# `ifdef AARCH64_UEFI_DISK` gates below keep aarch64-only rules out of
# x86_64-clang now that both profiles share the `uefi` capability.

# ── UEFI firmware acquisition (shared: x86 OVMF + aarch64 QEMU_EFI) ──
# One canned recipe for both per-arch firmware rules (this file's aarch64
# rule below and the x86 OVMF rule in mk/components/run.mk). The contract:
# the *_SOURCE variable accepts ONLY an https:// URL (wget to "$@.tmp", then
# an atomic rename) or an existing absolute local file path (content-guarded
# copy). Every other value — relative path, bad scheme, missing file — is
# rejected with a clear error BEFORE any download/copy. wget's exit status
# is checked via set -e, so a failed download never leaves a half-written
# file (and never reaches the mv). Never calls boot/uefi for firmware.
#   $(1) = NAME of the *_SOURCE variable (indirect, so the error messages
#          spell the variable the user actually set); the target path is $@
# Every newline in the body is backslash-escaped, so the expansion stays one
# logical shell line (same shape as os01_submake_cmd); call sites keep a
# single @-prefixed recipe line. Capability gates stay at the call sites —
# on a profile whose firmware variable is undefined an unguarded rule would
# expand to an empty-target rule with a recipe (a make-version-sensitive
# parse hazard).
define firmware_fetch_cmd
set -e; \
mkdir -p "$(dir $@)"; \
case "$($(1))" in \
https://*) \
  wget -q -O "$@.tmp" "$($(1))"; \
  mv "$@.tmp" "$@";; \
/*) \
  test -f "$($(1))" || { \
    echo "ERROR: $(1) '$($(1))' is not an existing file" >&2; \
    exit 1; }; \
  cmp -s "$($(1))" "$@" || cp "$($(1))" "$@";; \
*) \
  echo "ERROR: $(1) '$($(1))' must be an https:// URL or an existing absolute local file path" >&2; \
  exit 1;; \
esac
endef

# aarch64-only companion: QEMU `virt` machine cfi.pflash01 requires the
# block backend to be exactly 64 MiB (67108864 bytes) — edk2-nightly
# distributes the combined CODE+VARS image either pre-padded or code-only
# (~2-3 MiB), and any source file smaller than 64 MiB causes qemu to abort
# with `requires 67108864 bytes, pflash0 block backend provides N bytes`.
# Normalize the size after every fetch/copy so the QEMU topology in
# mk/components/run.mk works regardless of upstream packaging. The x86 OVMF
# rule does not need this step.
define firmware_size_normalize_64m
sz="$$(stat -c '%s' "$@")"; \
if [ "$$sz" != "67108864" ]; then \
  echo "  [aarch64-uefi] QEMU_EFI.fd is $$sz bytes, truncating/padding to 64 MiB for cfi.pflash01"; \
  truncate -s 64M "$@"; \
fi
endef

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
#   canary-selftest → build/<profile>/image/canary-selftest/
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
# dest=source:mode (files) and dest=target (BusyBox applet symlinks). This
# rule stages a FRESH tree (rm -rf of rootfs.next) so entries removed from
# the manifest never linger, then emits the tab-separated manifest that
# mkdisk consumes:
#   file<TAB>dest<TAB>staged-source<TAB>mode
#   symlink<TAB>dest<TAB>target
include $(OS01_ROOT)/config/rootfs.mk

# ── D05: Doom asset artifact rule ─────────────────────────────
DOOM_ASSET_DIR   := $(BUILD_DIR)/doom-assets
DOOM_ASSET_STAMP := $(DOOM_ASSET_DIR)/.doom_assets.stamp
DOOM_ASSET_KEY   := $(DOOM_ASSET_DIR)/.doom_assets.key

$(DOOM_ASSET_KEY): export OS01_DOOM_ASSET_IWAD = $(value DOOM_IWAD)
$(DOOM_ASSET_KEY): export OS01_DOOM_ASSET_SHA256 = $(value DOOM_IWAD_SHA256)
$(DOOM_ASSET_KEY): export OS01_DOOM_ASSET_LICENSE = $(value DOOM_IWAD_LICENSE)
$(DOOM_ASSET_KEY): export OS01_DOOM_ASSET_SOURCE = $(value DOOM_IWAD_SOURCE)
$(DOOM_ASSET_KEY): FORCE
	@mkdir -p $(dir $@)
	@python3 -c 'import os, sys; [sys.stdout.write(k.lower() + "=" + os.environ.get("OS01_DOOM_ASSET_" + k, "") + "\n") for k in ["IWAD", "SHA256", "LICENSE", "SOURCE"]]' > $@.new
	@cmp -s $@.new $@ && rm -f $@.new || mv $@.new $@

$(DOOM_ASSET_STAMP): export OS01_DOOM_ASSET_IWAD = $(value DOOM_IWAD)
$(DOOM_ASSET_STAMP): export OS01_DOOM_ASSET_SHA256 = $(value DOOM_IWAD_SHA256)
$(DOOM_ASSET_STAMP): export OS01_DOOM_ASSET_LICENSE = $(value DOOM_IWAD_LICENSE)
$(DOOM_ASSET_STAMP): export OS01_DOOM_ASSET_SOURCE = $(value DOOM_IWAD_SOURCE)

DOOM_ASSET_REAL_DEPS := $(if $(filter 4,$(DOOM_ASSET_COUNT)),$(strip $(value DOOM_IWAD)) $(strip $(value DOOM_IWAD_LICENSE)))

$(DOOM_ASSET_STAMP): $(DOOM_ASSET_KEY) $(OS01_ROOT)/scripts/prepare_doom_assets.py $(DOOM_ASSET_REAL_DEPS)
ifeq ($(DOOM_ASSET_COUNT),4)
	@mkdir -p $(dir $@)
	python3 $(OS01_ROOT)/scripts/prepare_doom_assets.py --from-env --output-dir $(DOOM_ASSET_DIR)
else
	@echo "ERROR: doom-assets target requires all four variables (DOOM_IWAD, DOOM_IWAD_SHA256, DOOM_IWAD_LICENSE, DOOM_IWAD_SOURCE)" >&2
	@exit 1
endif

.PHONY: doom-assets
doom-assets: $(DOOM_ASSET_STAMP)

# ── D06: Doom engine notice artifact rule ─────────────────────
DOOM_NOTICE_DIR   := $(BUILD_DIR)/doom-notice
DOOM_NOTICE_STAMP := $(DOOM_NOTICE_DIR)/.doom_notice.stamp
DOOM_ENGINE_LICENSE := $(OS01_ROOT)/thirdpart/doomgeneric/LICENSE
DOOM_ENGINE_MANIFEST := $(OS01_ROOT)/thirdpart/doomgeneric.manifest
DOOM_ENGINE_PATCH_DIR := $(OS01_ROOT)/thirdpart/patches/doomgeneric
-include $(DOOM_ENGINE_MANIFEST)
DOOM_ENGINE_PATCH_FILES := $(addprefix $(DOOM_ENGINE_PATCH_DIR)/,$(DOOMGENERIC_PATCHES))

$(DOOM_NOTICE_STAMP): $(DOOM_ENGINE_LICENSE) $(DOOM_ENGINE_MANIFEST) $(DOOM_ENGINE_PATCH_FILES) $(OS01_ROOT)/scripts/prepare_doom_engine_notice.py
	@mkdir -p $(dir $@)
	python3 $(OS01_ROOT)/scripts/prepare_doom_engine_notice.py \
		--manifest $(DOOM_ENGINE_MANIFEST) \
		--license $(DOOM_ENGINE_LICENSE) \
		--patch-dir $(DOOM_ENGINE_PATCH_DIR) \
		--output-dir $(DOOM_NOTICE_DIR)

.PHONY: doom-notice
doom-notice: $(DOOM_NOTICE_STAMP)

# Dynamic discovery must invalidate staging on removals as well as additions.
# Preserve mtime when the expanded input list is unchanged.
ROOTFS_INPUTS_STAMP := $(IMAGE_DIR)/rootfs.inputs
$(ROOTFS_INPUTS_STAMP): FORCE
	@mkdir -p $(dir $@)
	@printf '%s\n' $(ROOTFS_FILES) $(ROOTFS_SYMLINKS) > $@.tmp
	@cmp -s $@.tmp $@ && rm -f $@.tmp || mv $@.tmp $@

ROOTFS_MANIFEST_DEPS := $(ROOTFS_INPUTS_STAMP) $(OS01_ROOT)/config/rootfs.mk \
		$(USER_ARTIFACTS) $(USER_ASLR_B_EXEC_FIXTURE) $(USER_PTLS_FIXTURES) $(USER_INTERP_FIXTURES) $(USER_ARTIFACT_DIR)/busybox.elf \
		$(KERNEL_ARTIFACT) $(INITTAB_FILE) \
		$(if $(filter 4,$(DOOM_ASSET_COUNT)),$(DOOM_ASSET_STAMP)) \
		$(if $(filter doom,$(ROOTFS_USER_PROGRAMS)),$(DOOM_NOTICE_STAMP))

$(ROOTFS_MANIFEST): $(ROOTFS_MANIFEST_DEPS)
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
	  printf 'symlink\t%s\t%s\n' "$$dest" "$$target" >> $@.tmp; \
	done
	@test -s $@.tmp
	@mv $@.tmp $@

$(DISK_IMG): $(ROOTFS_MANIFEST) $(UEFI_EFI) $(HOST_MKDISK) $(KERNEL_ARTIFACT)
	@$(MAKE) -C tools check-deps HOST_TOOLS_DIR=$(BUILD_DIR)/host-tools
	@mkdir -p $(dir $@) $(dir $@)tmp
	@$(HOST_MKDISK) --output $@ --efi $(UEFI_EFI) \
	  --temp-dir $(dir $@)tmp --rootfs-manifest $(ROOTFS_MANIFEST) \
	  --esp-kernel-src $(KERNEL_ARTIFACT)

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

ifdef AARCH64_UEFI_DISK

# ── aarch64 libk.a (AAGU-29) ─────────────────────────────────
# The aarch64 kernel now links libk.a (kernel/arch/aarch64/make.config:
# ARCH_LIBS = -nostdlib -lk). The aarch64-clang profile has no `userland`
# capability (mk/profiles/aarch64-clang.mk), so mk/components/sysroot.mk's
# libc staging + sysroot generation path doesn't run for this profile —
# x86_64 reaches libk.a through the immutable sysroot generation, but
# aarch64 needs its own directly. Sub-make into libc/ with ARCH=aarch64
# overrides libc/Makefile's default HOSTARCH=x86_64; the build is
# incremental (skips already-built *.o / *.libk.o).
#
# The wildcard prereq on the libc source tree below is the
# load-bearing incremental trigger: a libc-only edit (e.g. touching
# libc/stdio/vsprintf.c) must rebuild libk.a and (via the kernel
# prereqs added in kernel/Makefile) the kernel too, without an explicit
# make clean.  Without the prereq make has no way to know the libc
# source tree changed and silently skips the sub-make.
$(LIBC_BUILD_DIR)/lib/libk.a: $(wildcard libc/stdio/*.c libc/string/*.c libc/ctype/*.c libc/errno/*.c libc/list/*.c libc/rbtree/*.c libc/stdlib/*.c libc/ssp/*.c) libc/include/stdio.h
	@mkdir -p $(dir $@)
	@$(call os01_submake,libc,all ARCH=aarch64 $(OS01_SUBMAKE_ARGS))

# ── aarch64 kernel artifact ─────────────────────────────────
# No sysroot, no lease: aarch64 does not consume the userland sysroot.
# The sub-make is incremental; the artifact copy is content-guarded.
# libk.a is built first via the rule above (AAGU-29 build-order prerequisite)
# so the kernel link resolves -lk against $(LIBC_BUILD_DIR)/lib/libk.a.
$(BUILD_DIR)/artifacts/kernel.elf: $(LIBC_BUILD_DIR)/lib/libk.a FORCE
	@mkdir -p $(dir $@)
	@$(call os01_submake,kernel,all ARCH=aarch64 \
	  $(if $(DRY_RUN),OS01_KERNEL_SUBMAKE_DRY=1) $(OS01_SUBMAKE_ARGS))
	@cmp -s $(KERNEL_BUILD_DIR)/kernel.elf $@ || cp $(KERNEL_BUILD_DIR)/kernel.elf $@

# ── aarch64 UEFI firmware (profile build dir) ───────────────
# Same acquisition contract as the x86 OVMF rule (mk/components/run.mk) —
# both expand the firmware_fetch_cmd canned recipe above, and this rule
# appends the 64 MiB cfi.pflash01 size normalization. Capability-gated on
# uefi: the aarch64-only AARCH64_UEFI_FIRMWARE variable is undefined for
# x86_64-clang, so without the guard this rule expands to an empty-target
# rule with a recipe — a make-version-sensitive parse hazard.
ifdef AARCH64_UEFI_DISK
$(AARCH64_UEFI_FIRMWARE):
	@$(call firmware_fetch_cmd,AARCH64_UEFI_FIRMWARE_SOURCE)
	@$(call firmware_size_normalize_64m)

# $(AARCH64_UEFI_FIRMWARE) is absolute (BUILD_DIR is profile-absolute), so the
# same on-disk file is also reachable through its relative spelling. This
# alias mirrors the x86 `build/<profile>/firmware/OVMF.fd` rule.
build/$(PROFILE)/firmware/QEMU_EFI.fd: $(AARCH64_UEFI_FIRMWARE)

# Backward-compatibility alias for scripts expecting QEMU_EFI.fd under image/
$(BUILD_DIR)/image/QEMU_EFI.fd: $(AARCH64_UEFI_FIRMWARE)
	@mkdir -p "$(dir $@)"
	@ln -sf ../firmware/QEMU_EFI.fd "$@"
endif

# ── aarch64 UEFI image (GPT dual-partition, both variants) ──
# AAGU-5.6: target path uses $(AARCH64_UEFI_DISK) so variant builds
# (weak-selftest, selftest) get isolated image dirs and never clobber
# each other. Recipe body unchanged — $@ expands to the variant-aware path.
# Both variants are the mkdisk GPT dual-partition layout (ESP FAT32 with
# BOOTAA64.EFI + kernel.elf → /boot, ext2 rootfs → /); they differ only in
# the ext2 manifest: the selftest variant carries the VFS selftest marker
# files, the product variant carries the product userspace — every
# unlocked aarch64 user program (mk/profiles/aarch64-clang.mk
# USER_EXCLUDED_PROGRAMS is the build-side gate; the image-side filter
# below mirrors the x86 ROOTFS_USER_PROGRAMS contract).  See mkdisk
# --esp-kernel for why the ESP kernel is named kernel.elf on aarch64.
# (2026-11-21: the product variant moved from the whole-disk FAT32
# superfloppy to this layout — the whole-disk FAT32 fallback mount path
# lost its aarch64 product coverage; see docs/changelog.md.)

# Product rootfs program list: USER_PROGRAMS minus the x86-parity image
# filter.  sh is dropped because /bin/sh is the busybox applet symlink
# (sh.c stays buildable for development, exactly like x86); the stress /
# probe programs are dropped from the NORMAL image on x86 too
# (config/rootfs.mk applies the same filter).  Requires userland: user.mk
# defines USER_PROGRAMS and is included before this file.
AARCH64_ROOTFS_PROGRAMS := $(filter-out sh test_arch10_capacity test_arch6_fsstress usb_boot_probe usb_hid_probe rp1_irq_probe,$(USER_PROGRAMS))
AARCH64_ROOTFS_PROGRAM_ELFS := $(addprefix $(USER_ARTIFACT_DIR)/,$(addsuffix .elf,$(AARCH64_ROOTFS_PROGRAMS)))
AARCH64_SELFTEST_MARKER := $(BUILD_DIR)/image$(if $(IMAGE_VARIANT),/$(IMAGE_VARIANT))/vfs-selftest.txt

$(AARCH64_SELFTEST_MARKER): FORCE
	@mkdir -p $(dir $@)
	@printf 'OS01-AARCH64-VFS-SELFTEST-MARKER\nline2-second-marker-content\n' > $@.tmp
	@mv $@.tmp $@

$(AARCH64_UEFI_DISK): $(BUILD_DIR)/artifacts/uefi/BOOTAA64.EFI \
		$(BUILD_DIR)/artifacts/kernel.elf \
		$(if $(filter qemu-virt,$(AARCH64_PLATFORM)),$(AARCH64_UEFI_FIRMWARE)) \
		$(AARCH64_ESP_CONFIG) \
		$(HOST_MKDISK) $(INITTAB_FILE) \
		$(if $(filter userland,$(PROFILE_CAPABILITIES)),$(AARCH64_ROOTFS_PROGRAM_ELFS) $(USER_ARTIFACT_DIR)/busybox.elf) \
		$(if $(filter selftest,$(IMAGE_VARIANT)),$(AARCH64_SELFTEST_MARKER))
ifeq ($(IMAGE_VARIANT),selftest)
	@$(MAKE) -C tools check-deps HOST_TOOLS_DIR=$(BUILD_DIR)/host-tools
	@mkdir -p $(dir $@) $(dir $@)tmp
	@rm -f $@.tmp
	@printf 'file\t/etc/inittab\t$(OS01_ROOT)/config/inittab\t0644\n' > $@.tmp
	@printf 'file\t/etc/vfs-selftest.txt\t$(AARCH64_SELFTEST_MARKER)\t0644\n' >> $@.tmp
	@mv $@.tmp $(dir $@)rootfs.selftest.manifest
	@$(HOST_MKDISK) --output $@ --efi $(BUILD_DIR)/artifacts/uefi/BOOTAA64.EFI \
	  --temp-dir $(dir $@)tmp --rootfs-manifest $(dir $@)rootfs.selftest.manifest \
	  --esp-kernel kernel.elf --esp-kernel-src $(KERNEL_BUILD_DIR)/kernel.elf \
	  --esp-config $(AARCH64_ESP_CONFIG)
else
	@$(MAKE) -C tools check-deps HOST_TOOLS_DIR=$(BUILD_DIR)/host-tools
	@mkdir -p $(dir $@) $(dir $@)tmp
	@rm -f $@.tmp
	$(eval include $(OS01_ROOT)/config/rootfs.mk)
	@# Product manifest (2026-11-21): the full unlocked aarch64 product
	@# userspace on the ext2 rootfs.  The ESP kernel is supplied via
	@# --esp-kernel-src so /kernel.elf is not duplicated in ext2 rootfs.
	@printf 'file\t/etc/inittab\t$(INITTAB_FILE)\t0644\n' > $@.tmp
	@printf 'file\t/bin/busybox\t$(USER_ARTIFACT_DIR)/busybox.elf\t0755\n' >> $@.tmp
	@for p in $(AARCH64_ROOTFS_PROGRAMS); do \
	  printf 'file\t/bin/%s\t$(USER_ARTIFACT_DIR)/%s.elf\t0755\n' "$$p" "$$p" >> $@.tmp; \
	done
	@# ext2 enables the real /bin/sh→busybox symlink (FAT had to carry a
	@# copy); busybox dispatches on argv[0], terminal.elf's /bin/sh
	@# fallback exec resolves through the link.
	@for item in $(ROOTFS_SYMLINKS); do \
	  dest=$${item%%=*}; target=$${item#*=}; \
	  printf 'symlink\t%s\t%s\n' "$$dest" "$$target" >> $@.tmp; \
	done
	@mv $@.tmp $(dir $@)rootfs.product.manifest
	@$(HOST_MKDISK) --output $@ --efi $(BUILD_DIR)/artifacts/uefi/BOOTAA64.EFI \
	  --temp-dir $(dir $@)tmp --rootfs-manifest $(dir $@)rootfs.product.manifest \
	  --esp-kernel kernel.elf --esp-kernel-src $(BUILD_DIR)/artifacts/kernel.elf \
	  --esp-config $(AARCH64_ESP_CONFIG)
endif

endif
