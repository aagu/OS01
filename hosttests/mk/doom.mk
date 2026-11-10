# Doom engine host tests (D03)
#
# The Doom group drives the PATCHED, upstream-patched engine sources on
# the host: the configuration round-trip (T3) and the exit behaviour (T2)
# run against the REAL patched m_config.c / i_system.c, not against a
# re-implementation.
#
# The engine sources under test are the PROFILE-PRIVATE patched copy the
# user build produces.  The host fixture therefore depends on the same
# private copy the archive is built from, so the test always observes
# the code that is actually shipped.
#
# The group is registered in the host test inventory (hosttests/mk/
# registry.mk) and is reached from the root through
# `make test-doom-host DOOM_CASE=build|config|all`.

DOOM_HOST_DIR := $(TEST_BLD)/doom
DOOM_PRIVATE_DIR := $(BUILD_DIR)/doom-upstream
DOOM_ENGINE_SRC := $(DOOM_PRIVATE_DIR)/doomgeneric

# The private copy only exists after the user build prepared it.  The
# fixture builds it on demand through the same user sub-make the root
# uses, so the host test never depends on a stale or hand-made tree.
.PHONY: doom-engine-private
doom-engine-private:
	@$(MAKE) --no-print-directory -C $(TESTS_DIR)/user \
	    OS01_PROFILE_FILE="$(OS01_PROFILE_FILE)" PROFILE="$(PROFILE)" \
	    SYSROOT_DIR="$(SYSROOT_DIR)" doom-engine-archive

$(DOOM_HOST_DIR)/.private.stamp: doom-engine-private
	@mkdir -p $(dir $@)
	@test -f "$(DOOM_ENGINE_SRC)/m_config.c" || { \
	    echo "ERROR: the private doomgeneric copy is missing $(DOOM_ENGINE_SRC)"; \
	    exit 1; }
	@touch $@

# The engine sources are compiled for the HOST with the same feature
# state as the shipped archive (no ORIGCODE, no sound, 320x200) so the
# host fixture observes the production code paths.
DOOM_HOST_CFLAGS := $(HOST_CFLAGS) -DOS01_HOST_TEST \
    -I$(DOOM_ENGINE_SRC) \
    -I$(TESTS_DIR)/thirdpart/doomgeneric \
    -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE \
    -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 \
    -DOS01_DOOM \
    -iquote $(TEST_INC) -iquote $(TEST_MOCK)

-include $(DOOM_HOST_DIR)/*.d
