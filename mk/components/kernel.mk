# ── Kernel artifact contract ─────────────────────────────────
# Cross-component owner of the kernel artifact (spec: kernel.mk). Consumes
# the sysroot generation stamp published by sysroot.mk plus the profile
# configuration, and produces $(KERNEL_ARTIFACT) (= artifacts/kernel.bin for
# profiles that publish a sysroot).
#
# The artifact recipe ALWAYS runs (FORCE): in ONE shell line it takes the
# publish lock, resolves the $(SYSROOT) symlink to the immutable generation,
# creates a generation read lease, [test-only OS01_BUILD_HOLD sleeps here],
# releases the lock, then invokes kernel/Makefile under the sanitized env
# with the immutable SYSROOT_GENERATION_DIR; a shell trap removes the lease
# on exit. When the published generation id differs from the one the objects
# were built against (recorded in $(KERNEL_BUILD_DIR)/.sysroot-generation),
# the sub-make is forced with -B so stale objects are rebuilt against the
# new generation. The recipe then verifies the ELF machine and publishes the
# binary.

# Only profiles that publish a sysroot (userland capability) AND declare a
# kernel artifact path build one.
ifeq ($(filter userland,$(PROFILE_CAPABILITIES)),userland)
ifdef KERNEL_ARTIFACT

$(KERNEL_ARTIFACT): $(SYSROOT_STAMP) FORCE
	@mkdir -p $(dir $@)
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
	  lease="$(LEASES_DIR)/$${genid}.$$$$.kernel"; \
	  mkdir -p "$(LEASES_DIR)"; \
	  mkdir "$$lease"; \
	  if [ -n "$(OS01_BUILD_HOLD)" ]; then sleep "$(OS01_BUILD_HOLD)"; fi; \
	  rm -f "$(LOCK_DIR)/owner"; \
	  rmdir "$(LOCK_DIR)" 2>/dev/null || true; \
	  trap "rmdir \"$$lease\" 2>/dev/null || true" EXIT; \
	  prev=""; \
	  if [ -f "$(KERNEL_BUILD_DIR)/.sysroot-generation" ]; then prev=$$(cat "$(KERNEL_BUILD_DIR)/.sysroot-generation"); fi; \
	  if [ "$$prev" != "$$genid" ]; then force="-B"; else force=""; fi; \
	  env -i PATH="$(PATH)" HOME="$(HOME)" TMPDIR="$(TMPDIR)" \
	    MAKEFLAGS="$(OS01_SUBMAKEFLAGS)" $(MAKE) MAKEOVERRIDES= \
	    -C kernel OS01_PROFILE_FILE="$(OS01_PROFILE_FILE)" PROFILE="$(PROFILE)" \
	    kernel.bin $$force SYSROOT_GENERATION_DIR="$$genabs" $(OS01_SUBMAKE_ARGS); \
	  dry=0; \
	  for w in $(OS01_SUBMAKEFLAGS); do case $$w in *n*) dry=1;; esac; done; \
	  if [ $$dry -eq 0 ]; then printf "%s\n" "$$genid" > "$(KERNEL_BUILD_DIR)/.sysroot-generation"; fi; \
	'
	@$(LLVM_READOBJ) --file-headers $(KERNEL_BUILD_DIR)/kernel.elf | grep -qF 'EM_X86_64'
	@cp $(KERNEL_BUILD_DIR)/kernel.bin $@

endif
endif
