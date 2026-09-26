# ── Kernel artifact contract ─────────────────────────────────
# Cross-component owner of the kernel artifact (spec: kernel.mk). Consumes
# the sysroot generation stamp published by sysroot.mk plus the profile
# configuration, and produces $(KERNEL_ARTIFACT) (= artifacts/kernel.bin for
# profiles that publish a sysroot).
#
# The artifact recipe ALWAYS runs (FORCE): in ONE shell line it takes the
# publish lock, verifies the $(SYSROOT) symlink is live, creates a
# generation read lease (the publisher waits out leases, so headers cannot
# be swapped mid-build), [test-only OS01_BUILD_HOLD sleeps here], releases
# the lock, then invokes kernel/Makefile under the sanitized env. The
# kernel compiles and links against the STABLE $(SYSROOT) symlink — header
# changes propagate as ordinary timestamp deps (header-level incremental
# rebuild), so no generation-id -B hammer exists anymore. A shell trap
# removes the lease on exit; the recipe then verifies the ELF machine and
# publishes the binary.

# Only profiles that publish a sysroot (userland capability) AND declare a
# kernel artifact path build one.
ifeq ($(filter userland,$(PROFILE_CAPABILITIES)),userland)
ifdef KERNEL_ARTIFACT

$(KERNEL_ARTIFACT): $(SYSROOT_STAMP) $(KERNEL_RUNTIME_PREREQ) FORCE
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
	  if [ -z "$$gen" ]; then echo "ERROR: sysroot symlink is missing ($(SYSROOT)) — cannot resolve an immutable generation"; exit 1; fi; \
	  lease="$(LEASES_DIR)/$$$$.kernel"; \
	  mkdir -p "$(LEASES_DIR)"; \
	  mkdir "$$lease"; \
	  if [ -n "$(OS01_BUILD_HOLD)" ]; then sleep "$(OS01_BUILD_HOLD)"; fi; \
	  rm -f "$(LOCK_DIR)/owner"; \
	  rmdir "$(LOCK_DIR)" 2>/dev/null || true; \
	  trap "rmdir \"$$lease\" 2>/dev/null || true" EXIT; \
	  if [ ! -f "$(KERNEL_RUNTIME_LINK_RECEIPT)" ]; then force="-B"; else force=""; fi; \
	  env -i PATH="$(PATH)" HOME="$(HOME)" TMPDIR="$(TMPDIR)" \
	    MAKEFLAGS="$(OS01_SUBMAKEFLAGS)" $(MAKE) MAKEOVERRIDES= \
	    -C kernel OS01_PROFILE_FILE="$(OS01_PROFILE_FILE)" PROFILE="$(PROFILE)" \
	    ARCH=x86_64 \
	    kernel.bin $$force \
	    KERNEL_RUNTIME_INPUTS="$(KERNEL_RUNTIME_INPUTS)" \
	    KERNEL_RUNTIME_PREREQ="$(KERNEL_RUNTIME_PREREQ)" \
	    KERNEL_RUNTIME_LINK_RECEIPT="$(KERNEL_RUNTIME_LINK_RECEIPT)" \
	    $(OS01_SUBMAKE_ARGS); \
	'
	@$(LLVM_READOBJ) --file-headers $(KERNEL_BUILD_DIR)/kernel.elf | grep -qF 'EM_X86_64'
	@cmp -s $(KERNEL_BUILD_DIR)/kernel.bin $@ || cp $(KERNEL_BUILD_DIR)/kernel.bin $@

endif
endif
