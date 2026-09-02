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
# The exact set packaged into the disk image (disk.img consumes these
# artifacts; config/fsroot copies come from $(USER_ARTIFACT_DIR)).
USER_PROGRAMS := init spin sigtest poweroff halt reboot systest \
                 test_mmap test_fork_mmap test_cow terminal smp_stress \
                 socktest udptest ipaddr nettest tetris
USER_ARTIFACTS := $(addprefix $(USER_ARTIFACT_DIR)/,$(addsuffix .elf,$(USER_PROGRAMS)))

# One grouped rule (GNU make &:, runs once per invocation): under the
# generation lease the sub-make builds every program into $(USER_BUILD_DIR),
# then each program ELF is copied into the artifact dir and verified. The
# copy/verify lines are separate recipe lines (not part of the + shell), so a
# dry run (-n) never touches the artifacts.
$(USER_ARTIFACTS)&: $(SYSROOT_STAMP) FORCE
	@mkdir -p $(dir $(firstword $(USER_ARTIFACTS))) $(USER_BUILD_DIR)
	@+$(SHELL) -ec '\
	  mkdir -p "$(dir $(LOCK_DIR))"; \
	  i=0; \
	  while ! mkdir "$(LOCK_DIR)" 2>/dev/null; do \
	    i=$$((i+1)); \
	    if [ $$i -ge 600 ]; then \
	      echo "ERROR: publish lock $(LOCK_DIR) held by:"; \
	      cat "$(LOCK_DIR)/owner" 2>/dev/null || true; \
	      exit 1; \
	    fi; \
	    sleep 0.1; \
	  done; \
	  trap "rm -f \"$(LOCK_DIR)/owner\"; rmdir \"$(LOCK_DIR)\" 2>/dev/null || true" EXIT; \
	  echo "$$$$ $(MAKECMDGOALS) $$(date +%s)" > "$(LOCK_DIR)/owner"; \
	  gen=$$(readlink "$(SYSROOT)" 2>/dev/null) || gen=""; \
	  if [ -z "$$gen" ]; then exit 0; fi; \
	  genid=$${gen#sysroot-generations/}; \
	  genabs="$(BUILD_DIR)/$$gen"; \
	  lease="$(LEASES_DIR)/$${genid}.$$$$.user"; \
	  mkdir -p "$(LEASES_DIR)"; \
	  mkdir "$$lease"; \
	  rm -f "$(LOCK_DIR)/owner"; \
	  rmdir "$(LOCK_DIR)" 2>/dev/null || true; \
	  trap "rmdir \"$$lease\" 2>/dev/null || true" EXIT; \
	  prev=""; \
	  if [ -f "$(USER_BUILD_DIR)/.sysroot-generation" ]; then prev=$$(cat "$(USER_BUILD_DIR)/.sysroot-generation"); fi; \
	  if [ "$$prev" != "$$genid" ]; then force="-B"; else force=""; fi; \
	  env -i PATH="$(PATH)" HOME="$(HOME)" TMPDIR="$(TMPDIR)" \
	    MAKEFLAGS="$(OS01_SUBMAKEFLAGS)" $(MAKE) MAKEOVERRIDES= \
	    -C user OS01_PROFILE_FILE="$(OS01_PROFILE_FILE)" PROFILE="$(PROFILE)" \
	    all $$force SYSROOT_GENERATION_DIR="$$genabs" $(OS01_SUBMAKE_ARGS); \
	  if [ -z "$(DRY_RUN)" ]; then printf "%s\n" "$$genid" > "$(USER_BUILD_DIR)/.sysroot-generation"; fi; \
	'
	@for p in $(USER_PROGRAMS); do cmp -s $(USER_BUILD_DIR)/$$p.elf $(USER_ARTIFACT_DIR)/$$p.elf || cp $(USER_BUILD_DIR)/$$p.elf $(USER_ARTIFACT_DIR)/$$p.elf; done
	@for p in $(USER_PROGRAMS); do test -f $(USER_ARTIFACT_DIR)/$$p.elf || { echo "ERROR: missing user artifact $$p.elf"; exit 1; }; done

# ── BusyBox adapter ───────────────────────────────────────────
# Never writes thirdpart/busybox-1.36.1/. The manifest records the source
# path and expected revision; the input digest covers the ACTUAL worktree
# (so local uncommitted modifications invalidate the adapter even though the
# git revision is unchanged), config/busybox.config.in, the manifest, all
# tracked overlay files, user/crt0.S + user/sigreturn_trampoline.S +
# user/linker.ld (the link consumes the linker script), the toolchain
# identity and the CURRENT generation id (its .config embeds the generation
# paths). The digest is written to $(BUILD_DIR)/receipts/busybox.input.digest
# and compared with $(BUILD_DIR)/receipts/busybox.stamp.receipt; only a
# difference (or a missing private binary) triggers a re-copy + rebuild. The
# private copy lives in $(BUILD_DIR)/thirdparty/busybox; the tracked overlay
# is applied there (applets/Kbuild.src + crt0.S + sigreturn_trampoline.S) and
# .config is generated from config/busybox.config.in with the generation
# paths. The relative -I../../libc/include -I../../kernel/include flags in
# the config template are dropped (the generation usr/include is a superset
# of both, and the relative paths would point at nothing at this build depth)
# and the linker script path is made absolute against $(OS01_ROOT). The
# native busybox Make is invoked under the controlled environment (env -i
# with the whitelisted MAKEFLAGS), same as every other sub-make, so ambient
# CFLAGS/LDFLAGS/CPPFLAGS cannot contaminate the third-party build. Depends
# on $(SYSROOT_STAMP), which transitively includes the compat-libs stamp:
# BusyBox never creates libm.a / librt.a itself.

BUSYBOX_MANIFEST  := $(OS01_ROOT)/thirdpart/busybox.manifest
BUSYBOX_PRIVATE   := $(BUILD_DIR)/thirdparty/busybox
BUSYBOX_RECEIPT   := $(BUILD_DIR)/receipts/busybox.stamp.receipt
BUSYBOX_INPUT_DG  := $(BUILD_DIR)/receipts/busybox.input.digest
BUSYBOX_OVERLAY   := $(OS01_ROOT)/config/busybox.overlay

$(USER_ARTIFACT_DIR)/busybox.elf: $(SYSROOT_STAMP) FORCE
	@mkdir -p $(dir $@) $(BUILD_DIR)/receipts
	@+$(SHELL) -ec '\
	  mkdir -p "$(dir $(LOCK_DIR))"; \
	  i=0; \
	  while ! mkdir "$(LOCK_DIR)" 2>/dev/null; do \
	    i=$$((i+1)); \
	    if [ $$i -ge 600 ]; then \
	      echo "ERROR: publish lock $(LOCK_DIR) held by:"; \
	      cat "$(LOCK_DIR)/owner" 2>/dev/null || true; \
	      exit 1; \
	    fi; \
	    sleep 0.1; \
	  done; \
	  trap "rm -f \"$(LOCK_DIR)/owner\"; rmdir \"$(LOCK_DIR)\" 2>/dev/null || true" EXIT; \
	  echo "$$$$ $(MAKECMDGOALS) $$(date +%s)" > "$(LOCK_DIR)/owner"; \
	  gen=$$(readlink "$(SYSROOT)" 2>/dev/null) || gen=""; \
	  if [ -z "$$gen" ]; then exit 0; fi; \
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
	      sha256sum user/crt0.S user/sigreturn_trampoline.S user/linker.ld; \
	      printf "toolchain: %s\n" "$$($(CLANG) --version 2>/dev/null | head -1)"; \
	      printf "generation: %s\n" "$$genid"; \
	    } | sha256sum | cut -d" " -f1 ); \
	    old=""; \
	    if [ -f "$(BUSYBOX_RECEIPT)" ]; then old=$$(cat "$(BUSYBOX_RECEIPT)"); fi; \
	    if [ "$$digest" = "$$old" ] && [ -f "$(BUSYBOX_PRIVATE)/busybox" ]; then \
	      echo "  [busybox] input unchanged, reusing private copy"; \
	    else \
	      echo "  [busybox] input changed, rebuilding in $(BUSYBOX_PRIVATE)"; \
	      printf "%s\n" "$$digest" > "$(BUSYBOX_INPUT_DG)"; \
	      rm -rf "$(BUSYBOX_PRIVATE)"; \
	      cp -a "$(OS01_ROOT)/thirdpart/busybox-1.36.1" "$(BUSYBOX_PRIVATE)"; \
	      [ -f "$(BUSYBOX_PRIVATE)/Makefile" ] || { echo "ERROR: private busybox copy incomplete"; exit 1; }; \
	      cp "$(BUSYBOX_OVERLAY)/applets/crt0.S" "$(BUSYBOX_PRIVATE)/applets/crt0.S"; \
	      cp "$(BUSYBOX_OVERLAY)/applets/sigreturn_trampoline.S" "$(BUSYBOX_PRIVATE)/applets/sigreturn_trampoline.S"; \
	      cp "$(BUSYBOX_OVERLAY)/applets/Kbuild.src" "$(BUSYBOX_PRIVATE)/applets/Kbuild.src"; \
	      sed -e "s|@TARGET_INCLUDEDIR@|$$genabs/usr/include|g" \
	          -e "s|@TARGET_LIBDIR@|$$genabs/usr/lib|g" \
	          -e "s|@CLANG_RESOURCE_DIR@|$(CLANG_RESOURCE_DIR)|g" \
	          -e "s|-I../../libc/include -I../../kernel/include ||" \
	          -e "s|-Wl,-T,../../user/linker.ld|-Wl,-T,$(OS01_ROOT)/user/linker.ld|" \
	          "$(OS01_ROOT)/config/busybox.config.in" > "$(BUSYBOX_PRIVATE)/.config"; \
	      env -i PATH="$(PATH)" HOME="$(HOME)" TMPDIR="$(TMPDIR)" \
	        MAKEFLAGS="$(OS01_SUBMAKEFLAGS)" $(MAKE) MAKEOVERRIDES= \
	        -C "$(BUSYBOX_PRIVATE)" silentoldconfig CC="$(TARGET_CCLD)" LD="$(TARGET_CCLD)" 2>/dev/null || \
	      yes "" | env -i PATH="$(PATH)" HOME="$(HOME)" TMPDIR="$(TMPDIR)" \
	        MAKEFLAGS="$(OS01_SUBMAKEFLAGS)" $(MAKE) MAKEOVERRIDES= \
	        -C "$(BUSYBOX_PRIVATE)" oldconfig CC="$(TARGET_CCLD)" LD="$(TARGET_CCLD)"; \
	      env -i PATH="$(PATH)" HOME="$(HOME)" TMPDIR="$(TMPDIR)" \
	        MAKEFLAGS="$(OS01_SUBMAKEFLAGS)" $(MAKE) MAKEOVERRIDES= \
	        -C "$(BUSYBOX_PRIVATE)" CC="$(TARGET_CCLD)" LD="$(TARGET_CCLD)"; \
	      [ -f "$(BUSYBOX_PRIVATE)/busybox" ] || { echo "ERROR: busybox build produced no binary"; exit 1; }; \
	      printf "%s\n" "$$digest" > "$(BUSYBOX_RECEIPT)"; \
	    fi; \
	    cmp -s "$(BUSYBOX_PRIVATE)/busybox" "$(USER_ARTIFACT_DIR)/busybox.elf" || cp "$(BUSYBOX_PRIVATE)/busybox" "$(USER_ARTIFACT_DIR)/busybox.elf"; \
	    test -f "$(USER_ARTIFACT_DIR)/busybox.elf"; \
	  fi; \
	'

endif
