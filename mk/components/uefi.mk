# ── UEFI runtime adapter + EFI artifact contract ───────────────
# uefi.mk wraps posix-uefi (spec: "UEFI runtime adapter"). The runtime
# identity is version-controlled: thirdpart/posix-uefi.manifest records the
# source path + fixed submodule revision, and config/posix-uefi/0001-*.patch
# holds the tracked modification (0001 fixes the fallback int8_t to signed
# char for aarch64 Clang). The two behaviors of the deleted 0002 overlay —
# `CFLAGS += -DUEFI_NO_UTF8` and the inner `make ... OUTDIR=` — are now an
# explicit environment contract: the root passes UEFI_RUNTIME_CFLAGS and
# UEFI_RUNTIME_MAKE to the boot wrapper, which starts the outer runtime Make
# with env CFLAGS=... MAKE=... (spec: "删除 runtime Makefile overlay"). The
# adapter verifies the submodule is initialized, its HEAD equals the gitlink
# SHA and its worktree is pristine; it copies the runtime to
# $(UEFI_RUNTIME_DIR), applies 0001 and stamps the result. x86 and aarch64
# keep separate per-profile runtime copies (never shared); the inner build
# stays -j1 (the G6 serial constraint lives inside the adapter).
#
# Only profiles that declare `uefi` (x86_64 and aarch64) own these
# rules. The `uefi` capability covers both architectures' UEFI needs
# (x86_64 with full rootfs; aarch64 with a 64 MiB bring-up FAT). The
# per-arch UEFI boot sources and EFI target names are picked below
# from the cap-x86 sub-branch; the aarch64 branch falls through and
# selects aarch64 sources by elimination.

ifeq ($(filter uefi,$(PROFILE_CAPABILITIES)),)
# No UEFI capability: nothing to define.
else

include $(OS01_ROOT)/thirdpart/posix-uefi.manifest

UEFI_RUNTIME_SOURCE  := $(OS01_ROOT)/$(POSIX_UEFI_SOURCE)
UEFI_RUNTIME_GITLINK := $(POSIX_UEFI_REVISION)
UEFI_PATCH_DIR       := $(OS01_ROOT)/config/posix-uefi
# 0001 only — the 0002 runtime overlay is deleted (its two behaviors are the
# env contract below). 0001 stays a receipt input so any change to the
# int8-tweak re-keys the profile-private runtime copy.
UEFI_PATCHES         := $(UEFI_PATCH_DIR)/0001-clang-int8.patch
# Runtime environment contract (spec: "删除 runtime Makefile overlay"). The
# root passes these two FINAL values explicitly to the boot wrapper
# (shell-safe single-quoted, UEFI_RUNTIME_MAKE contains a space); the wrapper
# starts the outer runtime Make with env CFLAGS="$(UEFI_RUNTIME_CFLAGS)"
# MAKE="$(UEFI_RUNTIME_MAKE)". Upstream's `CFLAGS +=` (outer + uefi/
# Makefiles) appends the UTF-8 macro to both compile classes; upstream's
# recursive `$(MAKE)` for uefi/libuefi.a runs with OUTDIR= so runtime archive
# objects land in the runtime dir while the outer application invocation
# keeps OUTDIR=$(BUILD_DIR)/uefi/. These values are printed into the receipt
# digest, so changing the contract rebuilds the runtime copy. The *_INPUT
# values are receipt-digest fixtures: production defaults name the real
# files that always execute, and the build-contract tests point them at
# fixture copies to prove digest invalidation.
# UEFI_RUNTIME_CFLAGS is prepended to posix-uefi's `CFLAGS +=` so that
# OS01's freestanding headers are searched BEFORE the host glibc ones.
# posix-uefi's -I/usr/include picks up glibc stdint.h, which on Ubuntu's
# multiarch layout needs bits/libc-header-start.h from
# /usr/include/x86_64-linux-gnu/bits/ — clang for --target=aarch64
# -pc-win32-coff does not search that multiarch path, so the build
# dies with "bits/libc-header-start.h file not found".
# -D__need_wchar_t makes clang's stddef.h define wchar_t (the EFI
# efibind.h does `typedef wchar_t CHAR16;`).
UEFI_RUNTIME_CFLAGS ?= -DUEFI_NO_UTF8 -D__need_wchar_t -I$(OS01_ROOT)/libc/include
UEFI_RUNTIME_MAKE ?= $(MAKE) OUTDIR=
UEFI_RUNTIME_ADAPTER_INPUT ?= $(OS01_ROOT)/mk/components/uefi.mk
UEFI_RUNTIME_WRAPPER_INPUT ?= $(OS01_ROOT)/boot/uefi/Makefile
UEFI_RUNTIME_STAMP   := $(BUILD_DIR)/stamps/uefi-runtime.stamp
UEFI_RUNTIME_RECEIPT := $(BUILD_DIR)/receipts/uefi-runtime.stamp.receipt
UEFI_RUNTIME_INPUT_DG := $(BUILD_DIR)/receipts/uefi-runtime.input.digest

# Per-arch UEFI boot sources (the SRCS the inner make compiles) and the UEFI
# compiler identity that goes into the digest: x86 uses the validated
# UEFI_CLANG; aarch64 uses the clang the copied runtime auto-detects.
# The dispatch keys on the target triple, NOT the userland capability —
# since the 2026-11-18 aarch64 userland capability the aarch64 profile
# also carries `userland` but must keep building the AArch64 loader.
# Resolved at parse time with $(if) (never inside a recipe — the busybox
# adapter comment in mk/components/user.mk records what multi-line recipe
# expansions do to backslash-newline handling).
UEFI_ARCH_FAMILY  := $(if $(filter x86_64-unknown-none,$(TARGET_TRIPLE)),x86_64,aarch64)
UEFI_TARGET_EFI   := $(if $(filter x86_64-unknown-none,$(TARGET_TRIPLE)),BOOTX64.EFI,BOOTAA64.EFI)
# x86 only: expands empty on other triples, which behaves like undefined
# for every consumer (OS01_SUBMAKE_ARGS skips empty whitelist entries, and
# the boot/uefi inner make self-derives UEFI_CLANG from its CC there).
UEFI_CLANG        ?= $(if $(filter x86_64-unknown-none,$(TARGET_TRIPLE)),$(CLANG))
UEFI_DIGEST_CLANG := $(if $(filter x86_64-unknown-none,$(TARGET_TRIPLE)),$(UEFI_CLANG),clang)
UEFI_BOOT_SRCS    := boot/uefi/main.c boot/uefi/boot_device.c boot/uefi/arch/$(UEFI_ARCH_FAMILY)/boot.c \
                     $(if $(filter aarch64,$(UEFI_ARCH_FAMILY)),boot/uefi/arch/aarch64/elf.c boot/uefi/arch/aarch64/fdt_early.c boot/uefi/arch/aarch64/handoff.S)

UEFI_EFI_ARTIFACT := $(BUILD_DIR)/artifacts/uefi/$(UEFI_TARGET_EFI)

# Receipt-digest input set: the SRCS the inner make compiles, the
# per-arch dispatch header, the shared boot_context ABI header, and
# (for aarch64) the shared handoff layout that the AArch64 loader
# pulls in via a relative path. Keep every x86 input present and add
# the AArch64-only headers conditionally so a header-only layout
# change still re-keys both the staged runtime receipt and the EFI
# artifact.
UEFI_BOOT_INPUTS := $(UEFI_BOOT_SRCS) boot/uefi/arch/arch.h \
                    boot/uefi/boot_device.h \
                    kernel/include/core/bootinfo.h
# LMA Task 2: the AArch64 loader consumes the shared layout constants
# through loader.h -> handoff_layout.h -> boot_layout.h, so every link
# in that chain (plus the configured boot block value itself) re-keys
# the runtime receipt and the EFI artifact.
UEFI_BOOT_INPUTS += $(if $(filter aarch64,$(UEFI_ARCH_FAMILY)),kernel/include/arch/aarch64/handoff_layout.h kernel/include/arch/aarch64/boot_layout.h boot/uefi/arch/aarch64/loader.h)

# ── Runtime adapter (FORCE: digest-gated, always checks) ──────────
# ONE shell line: verify submodule initialized + HEAD == gitlink + worktree
# pristine; compute the canonical input digest (submodule path, gitlink SHA,
# clean status, manifest, 0001, adapter + wrapper content, final
# UEFI_RUNTIME_CFLAGS/MAKE values, profile, UEFI compiler identity, runtime
# worktree mtimes, boot source hashes); compare it against the
# receipt beside the stamp; recopy + patch only when it differs; touch the
# stamp atomically only after the full copy+patch. Dry runs (-n) skip the
# recopy but still run the verification.
$(UEFI_RUNTIME_STAMP): FORCE
	@mkdir -p $(dir $@) $(BUILD_DIR)/receipts
	@+$(SHELL) -ec '\
	  test -f "$(UEFI_RUNTIME_SOURCE)/uefi/Makefile" || { \
	    echo "ERROR: posix-uefi submodule not initialized"; \
	    echo "Run: git submodule update --init"; exit 1; }; \
	  head=$$(git -C "$(UEFI_RUNTIME_SOURCE)" rev-parse HEAD 2>/dev/null); \
	  if [ "$$head" != "$(UEFI_RUNTIME_GITLINK)" ]; then \
	    echo "ERROR: posix-uefi HEAD $$head != gitlink $(UEFI_RUNTIME_GITLINK)"; \
	    echo "Run: git submodule update"; exit 1; \
	  fi; \
	  clean=$$(git -C "$(UEFI_RUNTIME_SOURCE)" status --porcelain); \
	  if [ -n "$$clean" ]; then \
	    echo "ERROR: posix-uefi worktree is not clean; run git -C thirdpart/posix-uefi clean -fd"; \
	    exit 1; \
	  fi; \
	  if [ -n "$(DRY_RUN)" ]; then \
	    echo "  [uefi] dry-run: not rebuilding runtime"; \
	  else \
	    test -f "$(UEFI_RUNTIME_ADAPTER_INPUT)" || { \
	      echo "ERROR: UEFI runtime adapter input '$(UEFI_RUNTIME_ADAPTER_INPUT)' does not exist"; \
	      exit 1; }; \
	    test -f "$(UEFI_RUNTIME_WRAPPER_INPUT)" || { \
	      echo "ERROR: UEFI runtime wrapper input '$(UEFI_RUNTIME_WRAPPER_INPUT)' does not exist"; \
	      exit 1; }; \
	    digest=$$( { cd "$(OS01_ROOT)" && \
	      printf "submodule: %s\n" "$(POSIX_UEFI_SOURCE)"; \
	      printf "gitlink: %s\n" "$(POSIX_UEFI_REVISION)"; \
	      printf "clean: %s\n" "$${clean:+dirty}"; \
	      sha256sum thirdpart/posix-uefi.manifest $(UEFI_PATCHES) $(UEFI_RUNTIME_ADAPTER_INPUT) $(UEFI_RUNTIME_WRAPPER_INPUT); \
	      printf "uefi-runtime-cflags: %s\n" "$(UEFI_RUNTIME_CFLAGS)"; \
	      printf "uefi-runtime-make: %s\n" "$(UEFI_RUNTIME_MAKE)"; \
	      printf "aarch64-boot-block-base: %s\n" "$(if $(filter aarch64,$(UEFI_ARCH_FAMILY)),$(AARCH64_BOOT_BLOCK_BASE),none)"; \
	      printf "profile: %s %s\n" "$(PROFILE)" "$(BUILD_DIR)"; \
	      printf "uefi-clang: %s\n" "$$($(UEFI_DIGEST_CLANG) --version 2>/dev/null | head -1)"; \
	      find thirdpart/posix-uefi -type f \
		  ! -path "thirdpart/posix-uefi/.git*" \
		  ! -name "*.o" ! -name "*.a" ! -name "*.lib" \
		  -exec stat -c "%y %n" {} + 2>/dev/null | sort; \
	      sha256sum $(UEFI_BOOT_INPUTS); \
	    } | sha256sum | cut -d" " -f1 ); \
	    old=""; \
	    if [ -f "$(UEFI_RUNTIME_RECEIPT)" ]; then old=$$(cat "$(UEFI_RUNTIME_RECEIPT)"); fi; \
	    if [ "$$digest" = "$$old" ] && [ -f "$(UEFI_RUNTIME_STAMP)" ]; then \
	      echo "  [uefi] runtime input unchanged, reusing $(UEFI_RUNTIME_DIR)"; \
	    else \
	      echo "  [uefi] runtime input changed, recopying + patching $(UEFI_RUNTIME_DIR)"; \
	      printf "%s\n" "$$digest" > "$(UEFI_RUNTIME_INPUT_DG)"; \
	      rm -rf "$(UEFI_RUNTIME_DIR)"; \
	      mkdir -p "$(UEFI_RUNTIME_DIR)"; \
	      cp -a "$(UEFI_RUNTIME_SOURCE)/." "$(UEFI_RUNTIME_DIR)/"; \
	      find "$(UEFI_RUNTIME_DIR)" \
		  \( -name "*.o" -o -name "*.a" -o -name "*.lib" \) -delete; \
	      rm -rf "$(UEFI_RUNTIME_DIR)/.git"; \
	      [ -f "$(UEFI_RUNTIME_DIR)/uefi/Makefile" ] || { echo "ERROR: posix-uefi copy incomplete"; exit 1; }; \
	      cp "$(UEFI_RUNTIME_DIR)/uefi/Makefile" "$(UEFI_RUNTIME_DIR)/Makefile"; \
	      cd "$(UEFI_RUNTIME_DIR)" && patch -p1 -f < "$(UEFI_PATCH_DIR)/0001-clang-int8.patch"; \
	      printf "%s\n" "$$digest" > "$(UEFI_RUNTIME_RECEIPT)"; \
	      touch "$(UEFI_RUNTIME_STAMP)"; \
	    fi; \
	  fi; \
	'

# ── EFI app artifact ──────────────────────────────────────────────
# Real-file rule over the runtime stamp + boot sources: the stamp is touched
# only when the runtime identity changed, so an up-to-date artifact does not
# rebuild on a no-op run. boot/uefi/Makefile compiles against the staged
# $(UEFI_RUNTIME_DIR); the artifact copy is content-guarded.
$(UEFI_EFI_ARTIFACT): $(UEFI_RUNTIME_STAMP) boot/uefi/Makefile \
		$(UEFI_BOOT_INPUTS)
	@mkdir -p $(dir $@)
	$(if $(DRY_RUN),\
	  @+if [ -f "$(UEFI_RUNTIME_STAMP)" ]; then \
	    $(call os01_submake_cmd,boot/uefi,ARCH=$(UEFI_ARCH_FAMILY) \
	      UEFI_RUNTIME_STAMP=$(UEFI_RUNTIME_STAMP) \
	      UEFI_RUNTIME_CFLAGS='$(UEFI_RUNTIME_CFLAGS)' \
	      UEFI_RUNTIME_MAKE='$(UEFI_RUNTIME_MAKE)' $(OS01_SUBMAKE_ARGS)); \
	  else \
	    echo "  [dry-run] uefi: skipped — bare tree lacks the uefi-runtime stamp"; \
	  fi,\
	  $(call os01_submake,boot/uefi,ARCH=$(UEFI_ARCH_FAMILY) \
	    UEFI_RUNTIME_STAMP=$(UEFI_RUNTIME_STAMP) \
	    UEFI_RUNTIME_CFLAGS='$(UEFI_RUNTIME_CFLAGS)' \
	    UEFI_RUNTIME_MAKE='$(UEFI_RUNTIME_MAKE)' $(OS01_SUBMAKE_ARGS)))
	@cmp -s $(UEFI_BUILD_DIR)/$(UEFI_TARGET_EFI) $@ || cp $(UEFI_BUILD_DIR)/$(UEFI_TARGET_EFI) $@

endif
