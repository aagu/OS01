# ── UEFI runtime adapter + EFI artifact contract ───────────────
# uefi.mk wraps posix-uefi (spec: "UEFI runtime adapter"). The runtime
# identity is version-controlled: thirdpart/posix-uefi.manifest records the
# source path + fixed submodule revision, and config/posix-uefi/*.patch holds
# the tracked modifications (0001 fixes the fallback int8_t to signed char
# for aarch64 Clang; 0002 overlays OUTDIR= + -DUEFI_NO_UTF8 on the copied
# Makefiles — reproducing the legacy sed mutations). The adapter verifies the
# submodule is initialized, its HEAD equals the gitlink SHA and its worktree
# is pristine; it copies the runtime to $(UEFI_RUNTIME_DIR), applies the
# patches and stamps the result. x86 and aarch64 keep separate per-profile
# runtime copies (never shared); the inner build stays -j1 (the G6 serial
# constraint lives inside the adapter).
#
# Only profiles that declare `uefi` (x86_64) or `uefi-bringup` (aarch64) own
# these rules.

ifeq ($(filter uefi uefi-bringup,$(PROFILE_CAPABILITIES)),)
# No UEFI capability: nothing to define.
else

include $(OS01_ROOT)/thirdpart/posix-uefi.manifest

UEFI_RUNTIME_SOURCE  := $(OS01_ROOT)/$(POSIX_UEFI_SOURCE)
UEFI_RUNTIME_GITLINK := $(POSIX_UEFI_REVISION)
UEFI_PATCH_DIR       := $(OS01_ROOT)/config/posix-uefi
UEFI_PATCHES         := $(UEFI_PATCH_DIR)/0001-clang-int8.patch $(UEFI_PATCH_DIR)/0002-runtime-make-overlay.patch
UEFI_RUNTIME_STAMP   := $(BUILD_DIR)/stamps/uefi-runtime.stamp
UEFI_RUNTIME_RECEIPT := $(BUILD_DIR)/receipts/uefi-runtime.stamp.receipt
UEFI_RUNTIME_INPUT_DG := $(BUILD_DIR)/receipts/uefi-runtime.input.digest

# Per-arch UEFI boot sources (the SRCS the inner make compiles) and the UEFI
# compiler identity that goes into the digest: x86 uses the validated
# UEFI_CLANG; aarch64 uses the clang the copied runtime auto-detects.
ifeq ($(filter uefi,$(PROFILE_CAPABILITIES)),uefi)
UEFI_ARCH_FAMILY  := x86_64
UEFI_TARGET_EFI   := BOOTX64.EFI
UEFI_CLANG        ?= $(CLANG)
UEFI_DIGEST_CLANG := $(UEFI_CLANG)
UEFI_BOOT_SRCS    := boot/uefi/main.c boot/uefi/arch/x86_64/boot.c
else
UEFI_ARCH_FAMILY  := aarch64
UEFI_TARGET_EFI   := BOOTAA64.EFI
UEFI_DIGEST_CLANG := clang
UEFI_BOOT_SRCS    := boot/uefi/main.c boot/uefi/arch/aarch64/boot.c \
                     boot/uefi/arch/aarch64/elf.c boot/uefi/arch/aarch64/handoff.S
endif

UEFI_EFI_ARTIFACT := $(BUILD_DIR)/artifacts/uefi/$(UEFI_TARGET_EFI)

# ── Runtime adapter (FORCE: digest-gated, always checks) ──────────
# ONE shell line: verify submodule initialized + HEAD == gitlink + worktree
# pristine; compute the canonical input digest (submodule path, gitlink SHA,
# clean status, manifest, both patches, profile, UEFI compiler identity,
# runtime worktree mtimes, boot source hashes); compare it against the
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
	    digest=$$( { cd "$(OS01_ROOT)" && \
	      printf "submodule: %s\n" "$(POSIX_UEFI_SOURCE)"; \
	      printf "gitlink: %s\n" "$(POSIX_UEFI_REVISION)"; \
	      printf "clean: %s\n" "$${clean:+dirty}"; \
	      sha256sum thirdpart/posix-uefi.manifest $(UEFI_PATCHES); \
	      printf "profile: %s %s\n" "$(PROFILE)" "$(BUILD_DIR)"; \
	      printf "uefi-clang: %s\n" "$$($(UEFI_DIGEST_CLANG) --version 2>/dev/null | head -1)"; \
	      find thirdpart/posix-uefi -type f ! -path "thirdpart/posix-uefi/.git*" -exec stat -c "%y %n" {} + 2>/dev/null | sort; \
	      sha256sum $(UEFI_BOOT_SRCS); \
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
	      rm -rf "$(UEFI_RUNTIME_DIR)/.git"; \
	      [ -f "$(UEFI_RUNTIME_DIR)/uefi/Makefile" ] || { echo "ERROR: posix-uefi copy incomplete"; exit 1; }; \
	      cp "$(UEFI_RUNTIME_DIR)/uefi/Makefile" "$(UEFI_RUNTIME_DIR)/Makefile"; \
	      cd "$(UEFI_RUNTIME_DIR)" && patch -p1 -f < "$(UEFI_PATCH_DIR)/0001-clang-int8.patch"; \
	      cd "$(UEFI_RUNTIME_DIR)" && patch -p1 -f < "$(UEFI_PATCH_DIR)/0002-runtime-make-overlay.patch"; \
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
		$(UEFI_BOOT_SRCS) boot/uefi/arch/arch.h kernel/include/kernel/bootinfo.h
	@mkdir -p $(dir $@)
	@$(call os01_submake,boot/uefi,ARCH=$(UEFI_ARCH_FAMILY) UEFI_RUNTIME_STAMP=$(UEFI_RUNTIME_STAMP))
	@cmp -s $(UEFI_BUILD_DIR)/$(UEFI_TARGET_EFI) $@ || cp $(UEFI_BUILD_DIR)/$(UEFI_TARGET_EFI) $@

endif
