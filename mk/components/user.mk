# ── Userland component contract ─────────────────────────────────
# Cross-component owner of every user ELF plus the BusyBox adapter
# (spec: user.mk). Consumes the sysroot generation stamp published by
# sysroot.mk, builds the user programs and BusyBox against the leased
# immutable generation, and publishes verified copies under
# $(USER_ARTIFACT_DIR) (= $(BUILD_DIR)/artifacts/user).
#
# Both artifact recipes ALWAYS run (FORCE): in ONE shell line they take the
# publish lock, resolve the $(SYSROOT) symlink to the immutable generation,
# create a generation read lease, release the lock, then consume the fixed
# absolute SYSROOT_GENERATION_DIR; a shell trap removes the lease on exit.
# When the published generation id differs from the one the previous build
# used (recorded in $(USER_BUILD_DIR)/.sysroot-generation), the user sub-make
# is forced with -B so stale objects are rebuilt against the new generation.
# The BusyBox adapter is digest-gated: it rebuilds only when the source
# worktree, config, overlay, toolchain identity or generation id changed.
#
# Only profiles with the userland capability own these rules.

ifeq ($(filter userland,$(PROFILE_CAPABILITIES)),userland)

# ── User programs ────────────────────────────────────────────
include $(OS01_ROOT)/user/apps.mk
USER_ARTIFACTS := $(addprefix $(USER_ARTIFACT_DIR)/,$(addsuffix .elf,$(USER_PROGRAMS)))
# User runtime consumer link probe (monitor plan Task 5): built by the user
# sub-make from runtime/selftest/user_udivti3_link_probe.c and published
# beside the other user artifacts.  test-monitor-runtime consumes it.
USER_RUNTIME_PROBE := $(USER_ARTIFACT_DIR)/user_udivti3_link_probe.elf

# One grouped rule (GNU make &:, runs once per invocation): under the
# generation lease the sub-make builds every program into $(USER_BUILD_DIR),
# then each program ELF is copied into the artifact dir and verified. The
# copy/verify lines are separate recipe lines (not part of the + shell), so a
# dry run (-n) never touches the artifacts.
$(USER_ARTIFACTS) $(USER_RUNTIME_PROBE)&: $(SYSROOT_STAMP) $(USER_RUNTIME_PREREQ) FORCE
	@mkdir -p $(dir $(firstword $(USER_ARTIFACTS))) $(USER_BUILD_DIR)
	@+$(SHELL) -ec '\
	  if [ -n "$(DRY_RUN)" ]; then \
	    if [ -f "$(SYSROOT)/usr/lib/libc.a" ]; then \
	      env -i PATH="$(PATH)" HOME="$(HOME)" TMPDIR="$(TMPDIR)" \
	        $(if $(COMPILER_LAUNCHER),COMPILER_LAUNCHER="$(COMPILER_LAUNCHER)") \
	        $(if $(CCACHE_DIR),CCACHE_DIR="$(CCACHE_DIR)") \
	        $(if $(CCACHE_BASEDIR),CCACHE_BASEDIR="$(CCACHE_BASEDIR)") \
	        $(if $(CCACHE_COMPILERCHECK),CCACHE_COMPILERCHECK="$(CCACHE_COMPILERCHECK)") \
	        MAKEFLAGS="$(OS01_SUBMAKEFLAGS)" $(MAKE) MAKEOVERRIDES= \
	        -C user OS01_PROFILE_FILE="$(OS01_PROFILE_FILE)" PROFILE="$(PROFILE)" \
	        USER_RUNTIME_ARCHIVE="$(USER_RUNTIME_ARCHIVE)" \
	        all SYSROOT_GENERATION_DIR="$(SYSROOT)" $(OS01_SUBMAKE_ARGS); \
	    else \
	      echo "  [dry-run] user programs: skipped — bare tree lacks the sysroot a real build would provide"; \
	    fi; \
	    exit 0; \
	  fi; \
	  $(call os01_acquire_publish_lock) \
	  gen=$$(readlink "$(SYSROOT)" 2>/dev/null) || gen=""; \
	  if [ -z "$$gen" ]; then echo "ERROR: sysroot symlink is missing ($(SYSROOT)) — cannot resolve an immutable generation"; exit 1; fi; \
	  genid=$${gen#sysroot-generations/}; \
	  lease="$(LEASES_DIR)/$${genid}.$$$$.user"; \
	  mkdir -p "$(LEASES_DIR)"; \
	  mkdir "$$lease"; \
	  rm -f "$(LOCK_DIR)/owner"; \
	  rmdir "$(LOCK_DIR)" 2>/dev/null || true; \
	  trap "rmdir \"$$lease\" 2>/dev/null || true" EXIT; \
	  env -i PATH="$(PATH)" HOME="$(HOME)" TMPDIR="$(TMPDIR)" \
	    $(if $(COMPILER_LAUNCHER),COMPILER_LAUNCHER="$(COMPILER_LAUNCHER)") \
	    $(if $(CCACHE_DIR),CCACHE_DIR="$(CCACHE_DIR)") \
	    $(if $(CCACHE_BASEDIR),CCACHE_BASEDIR="$(CCACHE_BASEDIR)") \
	    $(if $(CCACHE_COMPILERCHECK),CCACHE_COMPILERCHECK="$(CCACHE_COMPILERCHECK)") \
	    MAKEFLAGS="$(OS01_SUBMAKEFLAGS)" $(MAKE) MAKEOVERRIDES= \
	    -C user OS01_PROFILE_FILE="$(OS01_PROFILE_FILE)" PROFILE="$(PROFILE)" \
	    USER_RUNTIME_ARCHIVE="$(USER_RUNTIME_ARCHIVE)" \
	    all SYSROOT_GENERATION_DIR="$(SYSROOT)" $(OS01_SUBMAKE_ARGS); \
	  for p in $(USER_PROGRAMS); do \
	    cmp -s "$(USER_BUILD_DIR)/$$p.elf" "$(USER_ARTIFACT_DIR)/$$p.elf" || cp "$(USER_BUILD_DIR)/$$p.elf" "$(USER_ARTIFACT_DIR)/$$p.elf"; \
	    test -f "$(USER_ARTIFACT_DIR)/$$p.elf" || { echo "ERROR: missing user artifact $$p.elf"; exit 1; }; \
	  done; \
	  cmp -s "$(USER_BUILD_DIR)/user_udivti3_link_probe.elf" "$(USER_RUNTIME_PROBE)" || cp "$(USER_BUILD_DIR)/user_udivti3_link_probe.elf" "$(USER_RUNTIME_PROBE)"; \
	  test -f "$(USER_RUNTIME_PROBE)" || { echo "ERROR: missing user runtime probe artifact $(USER_RUNTIME_PROBE)"; exit 1; }; \
	  test -f "$(USER_BUILD_DIR)/obj/doom-engine/libdoom-engine.a" || { \
	    echo "ERROR: missing the doom engine archive (D03)"; \
	    echo "       Expected: $(USER_BUILD_DIR)/obj/doom-engine/libdoom-engine.a"; \
	    exit 1; }; \
	'

# ── BusyBox adapter ───────────────────────────────────────────
# Never writes thirdpart/busybox-1.36.1/. The manifest records the source
# path and expected revision; the input digest covers the ACTUAL worktree
# (so local uncommitted modifications invalidate the adapter even though the
# git revision is unchanged), config/busybox.config.in, the manifest, all
# tracked overlay files, libc/arch/$(BUSYBOX_ARCH_DIR)/crt0.S (the startup
# object comes from the sysroot, so a source-only crt0 change must force a
# relink) + libc/arch/$(BUSYBOX_ARCH_DIR)/user.ld (the link consumes the
# linker script), the toolchain identity and the CURRENT generation id (the
# .config embeds the generation paths). The digest is written to $(BUILD_DIR)/receipts/busybox.input.digest
# and compared with $(BUILD_DIR)/receipts/busybox.stamp.receipt; only a
# difference (or a missing private binary) triggers a re-copy + rebuild. The
# private copy lives in $(BUILD_DIR)/thirdparty/busybox; the tracked overlay
# is applied there (applets/Kbuild.src only — crt0.o comes from the sysroot
# and the sigreturn trampoline is a libc.a member) and .config is generated
# from config/busybox.config.in with the generation paths. The relative
# -I../../libc/include -I../../kernel/include flags in the config template
# are dropped (the generation usr/include is a superset of both, and the
# relative paths would point at nothing at this build depth) and the linker
# script placeholder is made absolute against $(OS01_ROOT). The native
# busybox Make is invoked under the controlled environment (env -i
# with the whitelisted MAKEFLAGS), same as every other sub-make, so ambient
# CFLAGS/LDFLAGS/CPPFLAGS cannot contaminate the third-party build. Depends
# on $(SYSROOT_STAMP), which transitively includes the compat-libs stamp:
# BusyBox never creates libm.a / librt.a itself.

# The BusyBox adapter covers every userland-capable profile (2026-11-21,
# aarch64 enablement): the linker plumbing (crt0.o from the profile sysroot,
# the sigreturn trampoline as a libc.a member) and the applet set follow
# $(RUNTIME_TARGET_user). The per-arch startup source and linker script live
# in libc/arch/<arch>/ (installed by libc install-libs); the overlay's
# Kbuild.src is arch-neutral.
#
# aarch64 quirks (2026-11-21):
# - busybox's native Makefile invokes $(LD) directly for the intermediate
#   `ld -r` links and drives the final link through $(CC) (scripts/trylink).
#   With clang targeting aarch64 both roles would fall back to the HOST GNU
#   ld, which has no aarch64 emulation (the x86_64 build only works because
#   the host ld accepts elf_x86_64) — pin the LLVM linker through the
#   clang driver.
# - the clang driver's default compiler-rt builtins path for
#   aarch64-none-elf does not exist on the host; -nostdlib suppresses the
#   implicit runtime injection (crt0.o and -lc stay explicit in the config
#   template).
# - busybox's libbb.h fixes the pagesize only for x86/32-bit-ARM and would
#   otherwise call getpagesize(), which the OS01 libc does not provide;
#   OS01 EL0 uses a fixed 4K granule, so pin BB_ARCH_FIXED_PAGESIZE=4096.
BUSYBOX_ARCH_DIR := $(if $(filter aarch64-none-elf,$(RUNTIME_TARGET_user)),aarch64,x86_64)
BUSYBOX_A64      := $(filter aarch64-none-elf,$(RUNTIME_TARGET_user))
BUSYBOX_LINK     := $(TARGET_CCLD) $(if $(BUSYBOX_A64),-fuse-ld=lld)
# Arch-conditional .config patches, resolved at parse time — a multi-line
# $(if) inside the single-quoted recipe shell string collides with GNU
# make's recipe backslash-newline handling (the expansion truncates the
# recipe), so the recipe only references this pre-expanded variable.
BUSYBOX_SED_A64  := $(if $(BUSYBOX_A64),-e "s|-fstack-protector-strong|-fstack-protector-strong -DBB_ARCH_FIXED_PAGESIZE=4096|" -e "s|-static -nostartfiles|-static -nostdlib -nostartfiles|")
ifneq ($(strip $(RUNTIME_TARGET_user)),)

BUSYBOX_MANIFEST  := $(OS01_ROOT)/thirdpart/busybox.manifest
BUSYBOX_PRIVATE   := $(BUILD_DIR)/thirdparty/busybox
BUSYBOX_RECEIPT   := $(BUILD_DIR)/receipts/busybox.stamp.receipt
BUSYBOX_INPUT_DG  := $(BUILD_DIR)/receipts/busybox.input.digest
BUSYBOX_OVERLAY   := $(OS01_ROOT)/config/busybox.overlay

$(USER_ARTIFACT_DIR)/busybox.elf: $(SYSROOT_STAMP) FORCE
	@mkdir -p $(dir $@) $(BUILD_DIR)/receipts $(BUILD_DIR)/thirdparty
	@+$(SHELL) -ec '\
	  if [ -n "$(DRY_RUN)" ]; then \
	    echo "  [busybox] dry-run: not rebuilding"; \
	    exit 0; \
	  fi; \
	  $(call os01_acquire_publish_lock) \
	  gen=$$(readlink "$(SYSROOT)" 2>/dev/null) || gen=""; \
	  if [ -z "$$gen" ]; then echo "ERROR: sysroot symlink is missing ($(SYSROOT)) — cannot resolve an immutable generation"; exit 1; fi; \
	  genid=$${gen#sysroot-generations/}; \
	  genabs="$(BUILD_DIR)/$$gen"; \
	  lease="$(LEASES_DIR)/$${genid}.$$$$.busybox"; \
	  mkdir -p "$(LEASES_DIR)"; \
	  mkdir "$$lease"; \
	  rm -f "$(LOCK_DIR)/owner"; \
	  rmdir "$(LOCK_DIR)" 2>/dev/null || true; \
	  trap "rmdir \"$$lease\" 2>/dev/null || true" EXIT; \
	  test -f "$(OS01_ROOT)/thirdpart/busybox-1.36.1/Makefile" || { \
	    echo "ERROR: busybox submodule not initialized"; \
	    echo "Run: git submodule update --init"; exit 1; }; \
	  if [ -n "$(DRY_RUN)" ]; then \
	    echo "  [busybox] dry-run: not rebuilding"; \
	  else \
	    digest=$$( { cd "$(OS01_ROOT)" && \
	      find thirdpart/busybox-1.36.1 -type f -exec sha256sum {} + 2>/dev/null | sort; \
	      sha256sum config/busybox.config.in thirdpart/busybox.manifest; \
	      find config/busybox.overlay -type f -exec sha256sum {} + 2>/dev/null | sort; \
	      sha256sum libc/arch/$(BUSYBOX_ARCH_DIR)/crt0.S libc/arch/$(BUSYBOX_ARCH_DIR)/user.ld; \
	      printf "toolchain: %s\n" "$$($(CLANG) --version 2>/dev/null | head -1)"; \
	      printf "sysroot generation: %s\n" "$$gen"; \
	    } | sha256sum | cut -d" " -f1 ); \
	    old=""; \
	    if [ -f "$(BUSYBOX_RECEIPT)" ]; then old=$$(cat "$(BUSYBOX_RECEIPT)"); fi; \
	    if [ "$$digest" = "$$old" ] && [ -f "$(BUSYBOX_PRIVATE)/busybox" ]; then \
	      echo "  [busybox] config unchanged, checking incremental build"; \
	      if [ -f "$(SYSROOT)/usr/lib/libc.a" ] && [ "$(SYSROOT)/usr/lib/libc.a" -nt "$(BUSYBOX_PRIVATE)/busybox" ]; then \
	        rm -f "$(BUSYBOX_PRIVATE)/busybox_unstripped" "$(BUSYBOX_PRIVATE)/busybox"; \
	      fi; \
	      env -i PATH="$(PATH)" HOME="$(HOME)" TMPDIR="$(TMPDIR)" \
	        MAKEFLAGS="$(OS01_SUBMAKEFLAGS)" SKIP_STRIP=y $(MAKE) MAKEOVERRIDES= \
	        -C "$(BUSYBOX_PRIVATE)" CC="$(BUSYBOX_LINK)" LD="$(BUSYBOX_LINK)"; \
	    else \
	      echo "  [busybox] config changed or unbuilt, rebuilding in $(BUSYBOX_PRIVATE)"; \
	      printf "%s\n" "$$digest" > "$(BUSYBOX_INPUT_DG)"; \
	      rm -rf "$(BUSYBOX_PRIVATE)"; \
	      cp -a "$(OS01_ROOT)/thirdpart/busybox-1.36.1" "$(BUSYBOX_PRIVATE)"; \
	      [ -f "$(BUSYBOX_PRIVATE)/Makefile" ] || { echo "ERROR: private busybox copy incomplete"; exit 1; }; \
	      cp "$(BUSYBOX_OVERLAY)/applets/Kbuild.src" "$(BUSYBOX_PRIVATE)/applets/Kbuild.src"; \
	      sed -e "s|@TARGET_INCLUDEDIR@|$(SYSROOT)/usr/include|g" \
	          -e "s|@TARGET_LIBDIR@|$(SYSROOT)/usr/lib|g" \
	          -e "s|@TARGET_LINKERSCRIPT@|$(OS01_ROOT)/libc/arch/$(BUSYBOX_ARCH_DIR)/user.ld|g" \
	          -e "s|@CLANG_RESOURCE_DIR@|$(CLANG_RESOURCE_DIR)|g" \
	          -e "s|-I../../libc/include -I../../kernel/include ||" \
	          $(BUSYBOX_SED_A64) \
	          "$(OS01_ROOT)/config/busybox.config.in" > "$(BUSYBOX_PRIVATE)/.config"; \
	      env -i PATH="$(PATH)" HOME="$(HOME)" TMPDIR="$(TMPDIR)" \
	        MAKEFLAGS="$(OS01_SUBMAKEFLAGS)" SKIP_STRIP=y $(MAKE) MAKEOVERRIDES= \
	        -C "$(BUSYBOX_PRIVATE)" silentoldconfig CC="$(BUSYBOX_LINK)" LD="$(BUSYBOX_LINK)" 2>/dev/null || \
	      yes "" | env -i PATH="$(PATH)" HOME="$(HOME)" TMPDIR="$(TMPDIR)" \
	        MAKEFLAGS="$(OS01_SUBMAKEFLAGS)" SKIP_STRIP=y $(MAKE) MAKEOVERRIDES= \
	        -C "$(BUSYBOX_PRIVATE)" oldconfig CC="$(BUSYBOX_LINK)" LD="$(BUSYBOX_LINK)"; \
	      env -i PATH="$(PATH)" HOME="$(HOME)" TMPDIR="$(TMPDIR)" \
	        MAKEFLAGS="$(OS01_SUBMAKEFLAGS)" SKIP_STRIP=y $(MAKE) MAKEOVERRIDES= \
	        -C "$(BUSYBOX_PRIVATE)" CC="$(BUSYBOX_LINK)" LD="$(BUSYBOX_LINK)"; \
	      [ -f "$(BUSYBOX_PRIVATE)/busybox" ] || { echo "ERROR: busybox build produced no binary"; exit 1; }; \
	      printf "%s\n" "$$digest" > "$(BUSYBOX_RECEIPT)"; \
	    fi; \
	    cmp -s "$(BUSYBOX_PRIVATE)/busybox" "$(USER_ARTIFACT_DIR)/busybox.elf" || cp "$(BUSYBOX_PRIVATE)/busybox" "$(USER_ARTIFACT_DIR)/busybox.elf"; \
	    test -f "$(USER_ARTIFACT_DIR)/busybox.elf"; \
	  fi; \
'

endif # RUNTIME_TARGET_user set (BusyBox adapter covers every userland profile)

endif
