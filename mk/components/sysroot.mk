# ── Sysroot generation publisher — the single writer ─────────────
# (spec: mk/components/sysroot.mk). Consumes the component staging trees
# (kernel-headers, libc, mbedtls, compat-libs) and assembles an immutable
# generation under $(SYSROOT_GENERATIONS_DIR)/<id>, then atomically re-points
# the $(SYSROOT) symlink at it. NO other module writes into a generation or
# the sysroot: component installs only ever write $(STAGING_DIR)/<component>.
#
# Only profiles with the userland capability publish a sysroot (the aarch64
# bring-up profile has none); these rules are their exclusive writer.

ifeq ($(filter userland,$(PROFILE_CAPABILITIES)),userland)

STAMPS_DIR   := $(BUILD_DIR)/stamps
RECEIPTS_DIR := $(BUILD_DIR)/receipts

# ── Content-gated staging stamps (R1) ─────────────────────────────
# Each staging stamp's recipe ALWAYS runs (FORCE prerequisite): it re-runs
# the component install into the private staging tree, then computes a digest
# of that tree (per-file sha256 + the manifest) and touches the stamp only
# when the content changed. Unchanged content leaves the stamp mtime alone,
# so the publish stamp is not re-triggered by a no-op build.

# staging_digest <tree> → one-line sha256 of the tree + its manifest.
define staging_digest
{ find $(1) -type f ! -name manifest -exec sha256sum {} + 2>/dev/null | sort; echo "---manifest---"; cat $(1)/manifest 2>/dev/null; } | sha256sum | cut -d' ' -f1
endef

# stamp_check <tree> — ONE recipe line (single-line expansion): compare the
# tree digest against the receipt beside the stamp, and touch the stamp only
# when the content changed. Each recipe line is expanded separately, so the
# digest line must not share a multi-line expansion with the + sub-make line
# (GNU make runs the tail of a multi-line expansion under -n).
define stamp_check
	@digest=$$($(call staging_digest,$(1))); \
	old=""; \
	if [ -f "$@.receipt" ]; then old=$$(cat "$@.receipt"); fi; \
	if [ "$$digest" != "$$old" ]; then \
	  echo "  [staging] $(1) content changed"; \
	  printf '%s\n' "$$digest" > "$@.receipt"; \
	  touch "$@"; \
	else \
	  echo "  [staging] $(1) content unchanged"; \
	fi
endef

$(STAMPS_DIR)/kernel-headers-install.stamp: FORCE
	@mkdir -p $(dir $@)
	$(call os01_submake,kernel,install-headers INSTALL_ROOT=$(STAGING_DIR)/kernel-headers ARCH=x86_64 $(OS01_SUBMAKE_ARGS))
	$(call stamp_check,$(STAGING_DIR)/kernel-headers)

$(STAMPS_DIR)/libc-install.stamp: FORCE
	@mkdir -p $(dir $@)
	$(call os01_submake,libc,install INSTALL_ROOT=$(STAGING_DIR)/libc $(OS01_SUBMAKE_ARGS))
	$(call stamp_check,$(STAGING_DIR)/libc)

# ── mbedTLS adapter (R7) ──────────────────────────────────────────
# No shared /tmp, no writes into the submodule, no final-sysroot writes.
# The FORCE-checked recipe computes the input digest (the mbedtls tree, the
# config overlay and the optional OS01 entropy source), and only re-copies /
# re-compiles when it differs from the receipt beside the stamp. The private
# copy lives in $(BUILD_DIR)/thirdparty/mbedtls, objects only in
# $(BUILD_DIR)/thirdparty/mbedtls-build, and the result stages into
# staging/mbedtls/ with a manifest and receipt.
MBEDTLS_PRIVATE   := $(BUILD_DIR)/thirdparty/mbedtls
MBEDTLS_BUILD_DIR := $(BUILD_DIR)/thirdparty/mbedtls-build
MBEDTLS_STAGING   := $(STAGING_DIR)/mbedtls
MBEDTLS_CC_FLAGS  := --sysroot=$(STAGING_DIR)/libc -isystem $(STAGING_DIR)/libc/usr/include \
                     -g -ffreestanding -fno-stack-protector \
                     -I$(MBEDTLS_PRIVATE)/include -I$(MBEDTLS_PRIVATE)/library \
                     -DMBEDTLS_USER_CONFIG_FILE='<mbedtls/os01_mbedtls_config.h>'

# Ordered AFTER the libc staging stamp: the mbedTLS compile consumes the libc
# headers (--sysroot=$(STAGING_DIR)/libc), so under -j the libc include tree
# must be complete before any mbedTLS file compiles.
$(STAMPS_DIR)/mbedtls-install.stamp: $(STAMPS_DIR)/libc-install.stamp FORCE
	@mkdir -p $(STAMPS_DIR) $(RECEIPTS_DIR)
	@digest=$$( { cd $(OS01_ROOT) && \
	  find thirdpart/mbedtls -type f -exec sha256sum {} + 2>/dev/null | sort; \
	  sha256sum config/mbedtls_config.h; \
	  if [ -f libc/network/entropy.c ]; then sha256sum libc/network/entropy.c; fi; \
	  printf 'toolchain: %s\n' "$(shell $(CLANG) --version 2>/dev/null | head -1)"; \
	} | sha256sum | cut -d' ' -f1 ); \
	old=""; \
	if [ -f "$@.receipt" ]; then old=$$(cat "$@.receipt"); fi; \
	if [ "$$digest" != "$$old" ]; then \
	  echo "  [mbedtls] input changed, rebuilding staging"; \
	  printf '%s\n' "$$digest" > "$(RECEIPTS_DIR)/mbedtls.input.digest"; \
	  rm -rf $(MBEDTLS_PRIVATE) $(MBEDTLS_BUILD_DIR) $(MBEDTLS_STAGING); \
	  mkdir -p $(MBEDTLS_PRIVATE) $(MBEDTLS_BUILD_DIR); \
	  cp -a $(OS01_ROOT)/thirdpart/mbedtls/. $(MBEDTLS_PRIVATE)/; \
	  [ -d "$(MBEDTLS_PRIVATE)/library" ] || { echo "ERROR: private mbedtls copy incomplete"; exit 1; }; \
	  mkdir -p $(MBEDTLS_PRIVATE)/include/mbedtls; \
	  cp $(OS01_ROOT)/config/mbedtls_config.h $(MBEDTLS_PRIVATE)/include/mbedtls/os01_mbedtls_config.h; \
	  ok=0; fail=0; \
	  for src in $(MBEDTLS_PRIVATE)/library/*.c; do \
	    name=$$(basename "$$src" .c); \
	    if $(TARGET_CC) $(MBEDTLS_CC_FLAGS) -c "$$src" -o $(MBEDTLS_BUILD_DIR)/$$name.o 2>/dev/null; then \
	      ok=$$((ok+1)); \
	    else \
	      fail=$$((fail+1)); \
	      if [ $$fail -le 3 ]; then \
	        echo "  [mbedtls] FAIL: $$name"; \
	        $(TARGET_CC) $(MBEDTLS_CC_FLAGS) -c "$$src" -o $(MBEDTLS_BUILD_DIR)/$$name.o 2>&1 | grep "error:" | head -1; \
	      fi; \
	    fi; \
	  done; \
	  if [ -f $(OS01_ROOT)/libc/network/entropy.c ]; then \
	    if $(TARGET_CC) $(MBEDTLS_CC_FLAGS) -c $(OS01_ROOT)/libc/network/entropy.c \
	        -o $(MBEDTLS_BUILD_DIR)/os01_entropy.o 2>/dev/null; then \
	      ok=$$((ok+1)); \
	    fi; \
	  fi; \
	  echo "  [mbedtls] $$ok compiled, $$fail failed"; \
	  mkdir -p $(MBEDTLS_STAGING)/usr/lib $(MBEDTLS_STAGING)/usr/include; \
	  $(LLVM_AR) rcs $(MBEDTLS_STAGING)/usr/lib/libmbedtls.a $(MBEDTLS_BUILD_DIR)/*.o; \
	  cp -R $(MBEDTLS_PRIVATE)/include/mbedtls $(MBEDTLS_STAGING)/usr/include/; \
	  find $(MBEDTLS_STAGING) -type f ! -name manifest | sed 's|^$(MBEDTLS_STAGING)/||' | sort > $(MBEDTLS_STAGING)/manifest; \
	  printf '%s\n' "$$digest" > "$@.receipt"; \
	  touch "$@"; \
	else \
	  echo "  [mbedtls] input unchanged"; \
	fi

# ── compat-libs staging (R8) ─────────────────────────────────────
# libm.a / librt.a stubs for the BusyBox link — staged ONLY here and
# assembled into the generation by the publisher; the BusyBox recipe no
# longer creates them. The stub archive is built from an empty TU inside
# $(BUILD_DIR) (no /tmp).
$(STAMPS_DIR)/compat-libs-install.stamp: FORCE
	@mkdir -p $(STAGING_DIR)/compat-libs/usr/lib
	@if [ ! -f $(STAGING_DIR)/compat-libs/usr/lib/libm.a ]; then \
	  printf '' > $(BUILD_DIR)/.compat-libs-stub.c; \
	  $(TARGET_CC) -c -x c $(BUILD_DIR)/.compat-libs-stub.c -o $(BUILD_DIR)/.compat-libs-stub.o 2>/dev/null; \
	  $(LLVM_AR) rcs $(STAGING_DIR)/compat-libs/usr/lib/libm.a $(BUILD_DIR)/.compat-libs-stub.o; \
	  $(LLVM_AR) rcs $(STAGING_DIR)/compat-libs/usr/lib/librt.a $(BUILD_DIR)/.compat-libs-stub.o; \
	  rm -f $(BUILD_DIR)/.compat-libs-stub.c $(BUILD_DIR)/.compat-libs-stub.o; \
	fi
	@find $(STAGING_DIR)/compat-libs -type f ! -name manifest | sed 's|^$(STAGING_DIR)/compat-libs/||' | sort > $(STAGING_DIR)/compat-libs/manifest
	@digest=$$($(call staging_digest,$(STAGING_DIR)/compat-libs)); \
	old=""; \
	if [ -f "$@.receipt" ]; then old=$$(cat "$@.receipt"); fi; \
	if [ "$$digest" != "$$old" ]; then \
	  echo "  [staging] compat-libs content changed"; \
	  printf '%s\n' "$$digest" > "$@.receipt"; \
	  touch "$@"; \
	else \
	  echo "  [staging] compat-libs content unchanged"; \
	fi

# ── Generation assembly + publish (R2/R3/R9) ─────────────────────
# While holding the publish lock: allocate the next generation id (atomic
# temp+rename counter), copy ONLY manifest-listed files from each staging
# tree into an empty generation dir (failing on duplicate destinations),
# verify every manifest path landed, then atomically re-point the $(SYSROOT)
# symlink and touch the stamp. A shell trap releases the lock in every path.
$(SYSROOT_STAMP): $(STAMPS_DIR)/kernel-headers-install.stamp \
                  $(STAMPS_DIR)/libc-install.stamp \
                  $(STAMPS_DIR)/mbedtls-install.stamp \
                  $(STAMPS_DIR)/compat-libs-install.stamp
	@mkdir -p $(dir $@) $(SYSROOT_GENERATIONS_DIR) $(LEASES_DIR)
	@set -e; \
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
	trap 'rm -f "$(LOCK_DIR)/owner"; rmdir "$(LOCK_DIR)" 2>/dev/null || true' EXIT; \
	echo "$$$$ $(MAKECMDGOALS) $$(date +%s)" > "$(LOCK_DIR)/owner"; \
	id=$$(cat "$(SYSROOT_GENERATIONS_DIR)/next-generation" 2>/dev/null || echo 0); \
	next=$$((id+1)); \
	printf '%s\n' "$$next" > "$(SYSROOT_GENERATIONS_DIR)/next-generation.tmp"; \
	mv -f "$(SYSROOT_GENERATIONS_DIR)/next-generation.tmp" "$(SYSROOT_GENERATIONS_DIR)/next-generation"; \
	gen="$(SYSROOT_GENERATIONS_DIR)/$$id"; \
	mkdir -p "$$gen"; \
	for comp in kernel-headers libc mbedtls compat-libs; do \
	  mf="$(STAGING_DIR)/$$comp/manifest"; \
	  if [ ! -f "$$mf" ]; then echo "ERROR: missing manifest $$mf"; exit 1; fi; \
	  while IFS= read -r rel; do \
	    [ -n "$$rel" ] || continue; \
	    dest="$$gen/$$rel"; \
	    if [ -e "$$dest" ]; then echo "ERROR: duplicate destination in generation: $$rel"; exit 1; fi; \
	    mkdir -p "$$(dirname "$$dest")"; \
	    cp "$(STAGING_DIR)/$$comp/$$rel" "$$dest"; \
	  done < "$$mf"; \
	  while IFS= read -r rel; do \
	    [ -n "$$rel" ] || continue; \
	    if [ ! -f "$$gen/$$rel" ]; then echo "ERROR: missing $$rel in generation $$id"; exit 1; fi; \
	  done < "$$mf"; \
	done; \
	rm -f "$(BUILD_DIR)/.sysroot.next"; \
	ln -s "sysroot-generations/$$id" "$(BUILD_DIR)/.sysroot.next"; \
	mv -Tf "$(BUILD_DIR)/.sysroot.next" "$(SYSROOT)"; \
	touch "$@"; \
	echo "  [sysroot] published generation $$id -> $(SYSROOT)"

endif
