# ── Doom engine archive (D03) ─────────────────────────────────────
#
# The Doom engine is NOT an application in this stage: there is no
# app.mk, no ELF and no /bin/doom (that is D04).  D03 delivers the
# STATIC ARCHIVE only, built from a PINNED upstream submodule.
#
# The upstream sources are never built in place and never written to.
# The build copies them into a PROFILE-PRIVATE directory, applies the
# OS01 patch list there and compiles the patched copy; the submodule
# worktree stays untouched.
#
# This file is loaded by the USER SUB-BUILD only.  The root publishing
# component (mk/components/user.mk) includes user/apps.mk for discovery
# and must never trigger the copy, the patches or the compile.
#
# Gates:
#   * userland capability -- aarch64 has none, so the engine is never
#     built there (the D03 contract asserts this).
#   * the upstream revision must equal the manifest pin, and every
#     patch must apply cleanly; a missing submodule, a wrong revision
#     or a failing patch is a HARD error with a locating message.  The
#     build never clones from the network.

ifeq ($(filter userland,$(PROFILE_CAPABILITIES)),userland)

# The archive lives under the USER build directory, exactly as the D03
# spec fixes it: $(USER_BUILD_DIR)/obj/doom-engine/libdoom-engine.a.
DOOM_ENGINE_DIR   := $(USER_BUILD_DIR)/obj/doom-engine
DOOM_PRIVATE_DIR  := $(USER_BUILD_DIR)/doom-upstream
DOOM_ARCHIVE      := $(DOOM_ENGINE_DIR)/libdoom-engine.a
DOOM_OBJ_DIR      := $(DOOM_ENGINE_DIR)/obj
DOOM_SOURCE_STAMP := $(DOOM_ENGINE_DIR)/.doom-source.stamp

DOOM_MANIFEST   := $(OS01_ROOT)/thirdpart/doomgeneric.manifest
DOOM_SOURCES_MK := $(OS01_ROOT)/thirdpart/doomgeneric-sources.mk
DOOM_PATCH_DIR  := $(OS01_ROOT)/thirdpart/patches/doomgeneric
DOOM_UPSTREAM   := $(OS01_ROOT)/thirdpart/doomgeneric

# The manifest carries the pinned revision (it is the same data-file
# contract as thirdpart/lvgl.manifest).
include $(DOOM_MANIFEST)

# The patch list is EXPLICIT and ORDERED.  It is owned by the manifest,
# so the whole pinned input set is described in ONE place.  Its contents
# are part of the engine's input digest: adding, removing or reordering
# a patch rebuilds the archive.  T2 and T3 extend the manifest list.
DOOM_PATCHES := $(addprefix $(DOOM_PATCH_DIR)/,$(DOOMGENERIC_PATCHES))

# The digest rule must run on every invocation: the recipe decides
# whether the private copy actually changes.  FORCE lives in the root
# project makefile, which the user sub-build does not include.
.PHONY: FORCE
FORCE:

include $(DOOM_SOURCES_MK)

# The unit list is fixed by contract: exactly DOOMGENERIC_UNIT_COUNT
# translation units, no wildcards, no platform entry point.
ifeq ($(words $(DOOMGENERIC_UNITS)),$(DOOMGENERIC_UNIT_COUNT))
else
$(error the doomgeneric unit list must contain exactly $(DOOMGENERIC_UNIT_COUNT) units, got $(words $(DOOMGENERIC_UNITS)))
endif

# The feature state of the archive.  ORIGCODE, CMAP256, SNDSERV and
# FEATURE_SOUND are deliberately NOT defined: the engine must not pull
# in SDL, the original X11/Windows paths or the sound backend.
DOOM_ENGINE_DEFS := -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE \
                    -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 \
                    -DOS01_DOOM

ifneq ($(filter 1,$(DOOM_TEST))$(filter doom-diag,$(USER_VARIANT)),)
DOOM_ENGINE_DEFS += -DOS01_DOOM_DIAG
endif

# The engine compiles against the LEASED immutable generation
# ($(SYSROOT_DIR) is the generation the root user component leased) with
# the profile's target compiler and the project's warning policy.  The
# third-party warning suppression is not global: it is limited to the
# engine rules.
DOOM_ENGINE_CFLAGS := --sysroot=$(SYSROOT_DIR) \
    -ffreestanding -fno-builtin -fstack-protector-strong \
    -fno-asynchronous-unwind-tables -fno-pic -fno-pie -mno-red-zone \
    -I$(DOOM_PRIVATE_DIR)/doomgeneric \
    -I$(TARGET_INCDIR) \
    -Wall -Wextra -O2 \
    $(DOOM_ENGINE_DEFS)

ifneq ($(filter 1,$(DOOM_TEST))$(filter doom-diag,$(USER_VARIANT)),)
DOOM_ENGINE_CFLAGS += -I$(OS01_ROOT)/user/doom
endif

# ── The private source copy ──────────────────────────────────────
# The digest covers the ACTUAL upstream content, the upstream commit,
# the manifest, the unit list, the patch contents and their order, the
# toolchain identity and every compile macro.  The stamp's content is
# that digest and its mtime only moves when the digest changes, so an
# unchanged engine does not rebuild.
$(DOOM_SOURCE_STAMP): $(DOOM_MANIFEST) $(DOOM_SOURCES_MK) $(DOOM_PATCHES) FORCE
	@mkdir -p $(dir $@)
	@$(SHELL) -ec '\
	  upstream="$(DOOM_UPSTREAM)"; \
	  private="$(DOOM_PRIVATE_DIR)"; \
	  test -e "$$upstream/.git" || { \
	    echo "ERROR: the doomgeneric submodule is not initialized ($$upstream)"; \
	    echo "       Run: git submodule update --init thirdpart/doomgeneric"; \
	    exit 1; }; \
	  head=$$(git -C "$$upstream" rev-parse --verify HEAD 2>/dev/null) || { \
	    echo "ERROR: cannot read the doomgeneric revision in $$upstream"; \
	    exit 1; }; \
	  if [ "$$head" != "$(DOOMGENERIC_REVISION)" ]; then \
	    echo "ERROR: the doomgeneric submodule is at $$head"; \
	    echo "       but thirdpart/doomgeneric.manifest pins $(DOOMGENERIC_REVISION)"; \
	    echo "       Changing the pin requires re-reviewing the unit list."; \
	    exit 1; \
	  fi; \
	  for p in $(DOOM_PATCHES); do \
	    test -f "$$p" || { echo "ERROR: the doomgeneric patch is missing: $$p"; exit 1; }; \
	  done; \
	  digest=$$( { \
	    find "$$upstream" -type f -not -path "*/.git/*" -exec sha256sum {} + 2>/dev/null | sort; \
	    sha256sum $(DOOM_MANIFEST) $(DOOM_SOURCES_MK); \
	    for p in $(DOOM_PATCHES); do sha256sum "$$p"; done; \
	    printf "revision: %s\n" "$$head"; \
	    printf "defs: %s\n" "$(DOOM_ENGINE_DEFS)"; \
	    printf "units: %s\n" "$(DOOMGENERIC_UNITS)"; \
	    printf "cflags: %s\n" "$(DOOM_ENGINE_CFLAGS)"; \
	    printf "toolchain: %s\n" "$$($(EFFECTIVE_CC) --version 2>/dev/null | head -1)"; \
	  } | sha256sum | cut -d" " -f1 ); \
	  old=""; \
	  if [ -f "$@" ]; then old=$$(cat "$@"); fi; \
	  if [ "$$digest" = "$$old" ] && [ -f "$$private/doomgeneric/i_system.c" ]; then \
	    rm -f "$@.new"; \
	  else \
	    echo "  [doom] engine inputs changed, refreshing the private copy"; \
	    mkdir -p "$$private"; \
	    cp -a "$$upstream/." "$$private/"; \
	    rm -rf "$$private/.git"; \
	    for p in $(DOOM_PATCHES); do \
	      echo "  [doom] applying $$(basename $$p)"; \
	      patch -p1 -d "$$private" --batch --forward --fuzz=0 < "$$p" || { \
	        echo "ERROR: the doomgeneric patch failed to apply: $$p"; \
	        echo "       The patch must apply cleanly against the pinned revision."; \
	        exit 1; }; \
	    done; \
	    printf "%s\n" "$$digest" > "$@.new"; \
	    mv -f "$@.new" "$@"; \
	  fi; \
	'

# ── The engine objects and the archive ───────────────────────────
# The objects depend on the CONTENT DIGEST (the stamp), not on the
# private source files: the digest covers the upstream content, the
# patch list and the compile macros, so any input change moves the
# stamp and rebuilds the objects.  The dependency files (.d) carry the
# header dependencies on top of that.
DOOM_OBJECTS := $(patsubst %.c,$(DOOM_OBJ_DIR)/%.o,$(DOOMGENERIC_UNITS))

# The digest stamp comes FIRST: it refreshes the private copy, so the
# source file exists when make looks for it.  The source is a real
# prerequisite (not an order-only one) so the .d machinery resolves the
# unit to the right translation unit instead of inventing a "<unit>.d.o"
# intermediate.
#
# The exact unit rules below make the sources *makeable*.  Without them a
# parallel make (-j) evaluates the object patterns before the stamp has
# run, sees a prerequisite with no rule, and fails the whole archive with
# "No rule to make target .../<unit>.c".  .SECONDARY keeps make from
# deleting the materialized sources as intermediate files when it exits.
DOOM_SOURCES := $(patsubst %,$(DOOM_PRIVATE_DIR)/doomgeneric/%,$(DOOMGENERIC_UNITS))

$(DOOM_SOURCES): $(DOOM_SOURCE_STAMP)
	@test -f $@ || { \
	    echo "ERROR: the doomgeneric private copy is missing $@"; \
	    exit 1; }

.SECONDARY: $(DOOM_SOURCES)

$(DOOM_OBJ_DIR)/%.o: $(DOOM_SOURCE_STAMP) $(DOOM_PRIVATE_DIR)/doomgeneric/%.c
	@mkdir -p $(dir $@)
	$(EFFECTIVE_CC) $(DOOM_ENGINE_CFLAGS) -MMD -MP -c $(DOOM_PRIVATE_DIR)/doomgeneric/$*.c -o $@

$(DOOM_ARCHIVE): $(DOOM_SOURCE_STAMP) $(DOOM_OBJECTS)
	@mkdir -p $(dir $@)
	@rm -f $@
	$(TARGET_AR) rcs $@ $(DOOM_OBJECTS)

-include $(DOOM_OBJECTS:.o=.d)

.PHONY: doom-engine-archive
doom-engine-archive: $(DOOM_ARCHIVE)

endif
