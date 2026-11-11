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
# The private patched copy lives under the USER build directory -- the very
# same directory user/doom/engine.mk writes into.
DOOM_PRIVATE_DIR := $(USER_BUILD_DIR)/doom-upstream
DOOM_ENGINE_SRC := $(DOOM_PRIVATE_DIR)/doomgeneric
# The shipped production objects (target build), used by the symbol-table
# assertions.
DOOM_PROD_OBJ_DIR := $(USER_BUILD_DIR)/obj/doom-engine/obj

# The private copy only exists after the user build prepared it.  The
# fixture builds it on demand through the same user sub-make the root
# uses, so the host test never depends on a stale or hand-made tree.
.PHONY: doom-engine-private
doom-engine-private:
	@$(MAKE) --no-print-directory -C $(TESTS_DIR)/user \
	    OS01_PROFILE_FILE="$(OS01_PROFILE_FILE)" PROFILE="$(PROFILE)" \
	    SYSROOT_DIR="$(SYSROOT)" doom-engine-archive

$(DOOM_HOST_DIR)/.private.stamp: doom-engine-private
	@mkdir -p $(dir $@)
	@test -f "$(DOOM_ENGINE_SRC)/m_config.c" || { \
	    echo "ERROR: the private doomgeneric copy is missing $(DOOM_ENGINE_SRC)"; \
	    exit 1; }
	@test -f "$(DOOM_ENGINE_SRC)/i_system.c" || { \
	    echo "ERROR: the private doomgeneric copy is missing $(DOOM_ENGINE_SRC)/i_system.c"; \
	    exit 1; }
	@touch $@

# The engine sources are compiled for the HOST with the same feature
# state as the shipped archive (no ORIGCODE, no sound, 320x200) so the
# host fixture observes the production code paths.
DOOM_HOST_CFLAGS := $(HOST_CFLAGS) -DOS01_HOST_TEST \
    -I$(DOOM_ENGINE_SRC) \
    -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE \
    -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 \
    -DOS01_DOOM \
    -iquote $(TEST_INC) -iquote $(TEST_MOCK)

# ── D03 T2: the exit behaviour ───────────────────────────────────────
#
# The fixture host-compiles the REAL patched i_system.c and intercepts the
# libc exit() at link time (--wrap=exit): the production source is never
# modified and no test entry point is installed in the engine.
$(eval $(call register_host_test,test_doom_engine_exit,doom,protocol-or-legacy))

TEST_BINS += $(TEST_BLD)/test_doom_engine_exit.elf

$(DOOM_HOST_DIR)/i_system.o: $(DOOM_ENGINE_SRC)/i_system.c $(DOOM_HOST_DIR)/.private.stamp
	@mkdir -p $(dir $@)
	$(HOST_CC) $(DOOM_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(TEST_BLD)/test_doom_engine_exit.o: $(TEST_CASES)/test_doom_engine_exit.c \
		$(DOOM_HOST_DIR)/.private.stamp
	@mkdir -p $(dir $@)
	$(HOST_CC) $(DOOM_HOST_CFLAGS) \
	    -DOS01_DOOM_NM='"$(NM_TOOL)"' \
	    -DOS01_DOOM_PROD_OBJECT='"$(DOOM_PROD_OBJ_DIR)/i_system.o"' \
	    -c $< -o $@

$(TEST_BLD)/test_doom_engine_exit.elf: $(TEST_BLD)/test_doom_engine_exit.o \
		$(DOOM_HOST_DIR)/i_system.o
	@mkdir -p $(dir $@)
	$(HOST_CC) -Wl,--wrap=exit -o $@ $^

.PHONY: test-doom-engine-exit
test-doom-engine-exit: $(TEST_BLD)/test_doom_engine_exit.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── D03 T3: the configuration round-trip ────────────────────────────────
#
# The fixture host-compiles the REAL patched m_config.c (the unit under
# test) together with the upstream m_misc.c it needs for
# M_StringJoin/M_MakeDirectory, and drives them through the public
# configuration interface only.
$(eval $(call register_host_test,test_doom_config,doom,protocol-or-legacy))

TEST_BINS += $(TEST_BLD)/test_doom_config.elf

$(DOOM_HOST_DIR)/m_config.o: $(DOOM_ENGINE_SRC)/m_config.c $(DOOM_HOST_DIR)/.private.stamp
	@mkdir -p $(dir $@)
	$(HOST_CC) $(DOOM_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(DOOM_HOST_DIR)/m_misc.o: $(DOOM_ENGINE_SRC)/m_misc.c $(DOOM_HOST_DIR)/.private.stamp
	@mkdir -p $(dir $@)
	$(HOST_CC) $(DOOM_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(TEST_BLD)/test_doom_config.o: $(TEST_CASES)/test_doom_config.c \
		$(DOOM_HOST_DIR)/.private.stamp
	@mkdir -p $(dir $@)
	$(HOST_CC) $(DOOM_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_doom_config.elf: $(TEST_BLD)/test_doom_config.o \
		$(DOOM_HOST_DIR)/m_config.o $(DOOM_HOST_DIR)/m_misc.o
	@mkdir -p $(dir $@)
	$(HOST_CC) -o $@ $^

.PHONY: test-doom-config
test-doom-config: $(TEST_BLD)/test_doom_config.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── D04 T1: video presentation & integer scaling ─────────────────────
$(eval $(call register_host_test,test_doom_video,doom,protocol-or-legacy))

TEST_BINS += $(TEST_BLD)/test_doom_video.elf

DOOM_VIDEO_HOST_CFLAGS := $(HOST_CFLAGS) -DOS01_HOST_TEST \
    -I$(TESTS_DIR)/user/doom \
    -I$(TESTS_DIR)/user \
    -I$(TESTS_DIR)/user/common \
    -I$(TESTS_DIR)/libgfx \
    -I$(TESTS_DIR)/kernel/include \
    -iquote $(TEST_INC) -iquote $(TEST_MOCK) \
    -D'errno=(*__errno_location())' \
    -include $(TEST_INC)/gfx_client_errno.h

$(DOOM_HOST_DIR)/doom_video.o: $(TESTS_DIR)/user/doom/doom_video.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(DOOM_VIDEO_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(DOOM_HOST_DIR)/gfx_client_policy.o: $(TESTS_DIR)/user/common/gfx_client_policy.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(DOOM_VIDEO_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(TEST_BLD)/test_doom_video.o: $(TEST_CASES)/test_doom_video.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(DOOM_VIDEO_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_doom_video.elf: $(TEST_BLD)/test_doom_video.o \
		$(DOOM_HOST_DIR)/doom_video.o \
		$(DOOM_HOST_DIR)/gfx_client_policy.o
	@mkdir -p $(dir $@)
	$(HOST_CC) -Wl,--wrap=open,--wrap=read,--wrap=close -o $@ $^

.PHONY: test-doom-video
test-doom-video: $(TEST_BLD)/test_doom_video.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── D04 T2: leased PS/2 input & Doom key edges ───────────────────────
$(eval $(call register_host_test,test_doom_input,doom,protocol-or-legacy))
$(eval $(call register_host_test,test_doom_input_engine,doom,protocol-or-legacy))

TEST_BINS += $(TEST_BLD)/test_doom_input.elf $(TEST_BLD)/test_doom_input_engine.elf

DOOM_INPUT_HOST_CFLAGS := $(HOST_CFLAGS) -DOS01_HOST_TEST \
    -I$(TESTS_DIR)/user/doom \
    -I$(TESTS_DIR)/user \
    -I$(TESTS_DIR)/libgui/include \
    -I$(TESTS_DIR)/libgfx \
    -I$(TESTS_DIR)/kernel/include \
    -I$(DOOM_ENGINE_SRC) \
    -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE \
    -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DOS01_DOOM \
    -iquote $(TEST_INC) -iquote $(TEST_MOCK) \
    -D'errno=(*__errno_location())' \
    -include $(TEST_INC)/gfx_client_errno.h

$(DOOM_HOST_DIR)/doom_input.o: $(TESTS_DIR)/user/doom/doom_input.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(DOOM_INPUT_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(DOOM_HOST_DIR)/input_ps2.o: $(TESTS_DIR)/libgui/src/input_ps2.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(DOOM_INPUT_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(DOOM_HOST_DIR)/i_input.o: $(DOOM_ENGINE_SRC)/i_input.c $(DOOM_HOST_DIR)/.private.stamp
	@mkdir -p $(dir $@)
	$(HOST_CC) $(DOOM_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(DOOM_HOST_DIR)/d_event.o: $(DOOM_ENGINE_SRC)/d_event.c $(DOOM_HOST_DIR)/.private.stamp
	@mkdir -p $(dir $@)
	$(HOST_CC) $(DOOM_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(TEST_BLD)/test_doom_input.o: $(TEST_CASES)/test_doom_input.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(DOOM_INPUT_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_doom_input_engine.o: $(TEST_CASES)/test_doom_input_engine.c \
		$(DOOM_HOST_DIR)/.private.stamp
	@mkdir -p $(dir $@)
	$(HOST_CC) $(DOOM_INPUT_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_doom_input.elf: $(TEST_BLD)/test_doom_input.o \
		$(DOOM_HOST_DIR)/doom_input.o \
		$(DOOM_HOST_DIR)/input_ps2.o
	@mkdir -p $(dir $@)
	$(HOST_CC) -Wl,--wrap=open,--wrap=read,--wrap=close -o $@ $^

$(TEST_BLD)/test_doom_input_engine.elf: $(TEST_BLD)/test_doom_input_engine.o \
		$(DOOM_HOST_DIR)/doom_input.o \
		$(DOOM_HOST_DIR)/input_ps2.o \
		$(DOOM_HOST_DIR)/i_input.o \
		$(DOOM_HOST_DIR)/d_event.o
	@mkdir -p $(dir $@)
	$(HOST_CC) -Wl,--wrap=open,--wrap=read,--wrap=close -o $@ $^

.PHONY: test-doom-input
test-doom-input: $(TEST_BLD)/test_doom_input.elf $(TEST_BLD)/test_doom_input_engine.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $^

# ── D04 T3: runtime lifetime and cooperative shutdown ────────────────
$(eval $(call register_host_test,test_doom_runtime,doom,protocol-or-legacy))

TEST_BINS += $(TEST_BLD)/test_doom_runtime.elf

DOOM_RUNTIME_HOST_CFLAGS := $(HOST_CFLAGS) -DOS01_HOST_TEST \
    -I$(TESTS_DIR)/user/doom \
    -I$(TESTS_DIR)/user \
    -I$(TESTS_DIR)/user/common \
    -I$(TESTS_DIR)/libgui/include \
    -I$(TESTS_DIR)/libgfx \
    -I$(TESTS_DIR)/kernel/include \
    -I$(DOOM_ENGINE_SRC) \
    -DNORMALUNIX -DLINUX -D_DEFAULT_SOURCE \
    -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DOS01_DOOM \
    -iquote $(TEST_INC) -iquote $(TEST_MOCK) \
    -D'errno=(*__errno_location())' \
    -include $(TEST_INC)/gfx_client_errno.h

$(DOOM_HOST_DIR)/doom_os01.o: $(TESTS_DIR)/user/doom/doom_os01.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(DOOM_RUNTIME_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(DOOM_HOST_DIR)/doom_runtime.o: $(TESTS_DIR)/user/doom/doom_runtime.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(DOOM_RUNTIME_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(TEST_BLD)/test_doom_runtime.o: $(TEST_CASES)/test_doom_runtime.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(DOOM_RUNTIME_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_doom_runtime.elf: $(TEST_BLD)/test_doom_runtime.o \
		$(DOOM_HOST_DIR)/doom_os01.o \
		$(DOOM_HOST_DIR)/doom_runtime.o \
		$(DOOM_HOST_DIR)/doom_video.o \
		$(DOOM_HOST_DIR)/doom_input.o \
		$(DOOM_HOST_DIR)/input_ps2.o \
		$(DOOM_HOST_DIR)/gfx_client_policy.o
	@mkdir -p $(dir $@)
	$(HOST_CC) -Wl,--wrap=open,--wrap=read,--wrap=close,--wrap=nanosleep,--wrap=clock_gettime,--wrap=abort,--wrap=exit -o $@ $^

.PHONY: test-doom-runtime
test-doom-runtime: $(TEST_BLD)/test_doom_runtime.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── D06: diagnostic observation fixture ──────────────────────────────
$(eval $(call register_host_test,test_doom_diag,doom,protocol-or-legacy))

TEST_BINS += $(TEST_BLD)/test_doom_diag.elf

$(DOOM_HOST_DIR)/diag.o: $(TESTS_DIR)/user/doom/diag.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(DOOM_RUNTIME_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(TEST_BLD)/test_doom_diag.o: $(TEST_CASES)/test_doom_diag.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(DOOM_RUNTIME_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_doom_diag.elf: $(TEST_BLD)/test_doom_diag.o $(DOOM_HOST_DIR)/diag.o
	@mkdir -p $(dir $@)
	$(HOST_CC) -o $@ $^

.PHONY: test-doom-diag
test-doom-diag: $(TEST_BLD)/test_doom_diag.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

-include $(DOOM_HOST_DIR)/*.d
