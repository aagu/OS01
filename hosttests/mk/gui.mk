# GUI subsystem host tests
$(eval $(call register_host_test,test_desktop_audit,gui,protocol-or-legacy))
$(eval $(call register_host_test,test_desktop_shell,gui,protocol-or-legacy))
$(eval $(call register_host_test,test_desktop_state,gui,protocol-or-legacy))
$(eval $(call register_host_test,test_gfx_input,gui,protocol-or-legacy))
$(eval $(call register_host_test,test_gui_input_decode,gui,protocol-or-legacy))
$(eval $(call register_host_test,test_gui_input_lvgl,gui,protocol-or-legacy))
$(eval $(call register_host_test,test_gui_input_nonce,gui,protocol-or-legacy))
$(eval $(call register_host_test,test_gui_input_platform,gui,protocol-or-legacy))
$(eval $(call register_host_test,test_gui_lvgl_integration,gui,protocol-or-legacy))
$(eval $(call register_host_test,test_gui_platform,gui,protocol-or-legacy))
$(eval $(call register_host_test,test_gui_theme,gui,protocol-or-legacy))
$(eval $(call register_host_test,test_gui_theme_apply,gui,protocol-or-legacy))
$(eval $(call register_host_test,test_gui_widget,gui,protocol-or-legacy))
$(eval $(call register_host_test,test_input_ps2_frame,gui,protocol-or-legacy))
$(eval $(call register_host_test,test_input_route,gui,protocol-or-legacy))
$(eval $(call register_host_test,test_tetris_logic,gui,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_tetris_logic.elf \
    $(TEST_BLD)/test_desktop_state.elf \
    $(TEST_BLD)/test_gui_platform.elf \
    $(TEST_BLD)/test_gui_lvgl_integration.elf \
    $(TEST_BLD)/test_input_ps2_frame.elf \
    $(TEST_BLD)/test_input_route.elf \
    $(TEST_BLD)/test_gfx_input.elf \
    $(TEST_BLD)/test_gui_input_decode.elf \
    $(TEST_BLD)/test_gui_input_nonce.elf \
    $(TEST_BLD)/test_gui_input_platform.elf \
    $(TEST_BLD)/test_gui_input_lvgl.elf \
    $(TEST_BLD)/test_gui_theme.elf \
    $(TEST_BLD)/test_gui_theme_apply.elf \
    $(TEST_BLD)/test_desktop_shell.elf \
    $(TEST_BLD)/test_desktop_audit.elf \
    $(TEST_BLD)/test_gui_widget.elf


# Tetris logic test — compiles REAL user/tetris/tetris_logic.c (pure C)
$(TEST_BLD)/tetris_logic.o: $(TESTS_DIR)/user/tetris/tetris_logic.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/user/tetris -c $< -o $@

$(TEST_BLD)/test_tetris_logic.o: $(TEST_CASES)/test_tetris_logic.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/user/tetris -c $< -o $@

$(TEST_BLD)/test_tetris_logic.elf: $(TEST_BLD)/test_tetris_logic.o $(TEST_BLD)/tetris_logic.o
	$(HOST_CC) -o $@ $^


# GUI-3 Task 3: the desktop surface state machine.  Compiles the REAL
# user/desktop/desktop_state.c (pure C, no LVGL dependency) so the
# (state x intent) matrix can be exhausted on the host.
$(TEST_BLD)/desktop_state.o: $(TESTS_DIR)/user/desktop/desktop_state.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/user/desktop -c $< -o $@

$(TEST_BLD)/test_desktop_state.o: $(TEST_CASES)/test_desktop_state.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/user/desktop -c $< -o $@

$(TEST_BLD)/test_desktop_state.elf: $(TEST_BLD)/test_desktop_state.o $(TEST_BLD)/desktop_state.o
	$(HOST_CC) -o $@ $^

# Gfx file-lifecycle test (Task 1): host-compiles the REAL

# ── GUI platform host tests ─────────────────────────────────────
GUI_HOST_DIR := $(TEST_BLD)/gui
GUI_LVGL_DIR := $(GUI_HOST_DIR)/lvgl
GUI_DETERMINISTIC_DIR := $(GUI_HOST_DIR)/deterministic
GUI_INTEGRATION_DIR := $(GUI_HOST_DIR)/integration

include $(TESTS_DIR)/thirdpart/lvgl.manifest

GUI_LVGL_SOURCES := $(shell find $(TESTS_DIR)/$(LVGL_SOURCE)/src -name '*.c')
GUI_LVGL_OBJS := $(patsubst $(TESTS_DIR)/$(LVGL_SOURCE)/src/%.c,$(GUI_LVGL_DIR)/obj/%.o,$(GUI_LVGL_SOURCES))
GUI_LVGL_ARCHIVE := $(GUI_LVGL_DIR)/liblvgl-host.a

# GUI-3: the Material theme unit (token table + LVGL theme).  Every host
# test that links the REAL libgui platform must also link these two
# translation units, because gui_platform_create() applies the theme and
# gui_platform_destroy() releases it.
GUI_THEME_OBJS := $(GUI_INTEGRATION_DIR)/theme.o $(GUI_INTEGRATION_DIR)/theme_apply.o

$(GUI_HOST_DIR)/lvgl-version.stamp: $(TESTS_DIR)/thirdpart/lvgl.manifest
	@test -e "$(TESTS_DIR)/$(LVGL_SOURCE)/.git" || { echo "ERROR: $(LVGL_SOURCE) not a git submodule (.git missing)"; exit 1; }
	@test -f "$(TESTS_DIR)/$(LVGL_SOURCE)/lvgl.h" || { echo "ERROR: $(LVGL_SOURCE) missing lvgl.h"; exit 1; }
	@head=$$(git -C "$(TESTS_DIR)/$(LVGL_SOURCE)" rev-parse --verify HEAD 2>/dev/null) || { echo "ERROR: failed to get git commit for $(LVGL_SOURCE)"; exit 1; }; \
	if [ "$$head" != "$(LVGL_REVISION)" ]; then \
	  echo "ERROR: $(LVGL_SOURCE) revision $$head does not match manifest $(LVGL_REVISION)"; exit 1; \
	fi
	@mkdir -p $(dir $@)
	@touch $@

$(GUI_HOST_DIR)/include/uapi/gfx.h: $(TESTS_DIR)/kernel/include/uapi/gfx.h
	@mkdir -p $(dir $@)
	ln -sf $< $@

$(GUI_HOST_DIR)/include/uapi/fb.h: $(TESTS_DIR)/kernel/include/uapi/fb.h
	@mkdir -p $(dir $@)
	ln -sf $< $@

$(GUI_HOST_DIR)/include/uapi/mouse.h: $(TESTS_DIR)/kernel/include/uapi/mouse.h
	@mkdir -p $(dir $@)
	ln -sf $< $@

GUI_HOST_UAPI_HEADERS := $(GUI_HOST_DIR)/include/uapi/gfx.h $(GUI_HOST_DIR)/include/uapi/fb.h \
                        $(GUI_HOST_DIR)/include/uapi/mouse.h

GUI_HOST_LVGL_CFLAGS := -O2 -g $(filter-out -O0,$(HOST_CFLAGS)) \
    -I$(TESTS_DIR)/thirdpart \
    -I$(TESTS_DIR)/thirdpart/lvgl \
    -I$(TESTS_DIR)/config \
    -DLV_CONF_INCLUDE_SIMPLE

$(GUI_LVGL_OBJS): | $(GUI_HOST_DIR)/lvgl-version.stamp

$(GUI_LVGL_DIR)/obj/%.o: $(TESTS_DIR)/$(LVGL_SOURCE)/src/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_LVGL_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_LVGL_ARCHIVE): $(GUI_LVGL_OBJS)
	@mkdir -p $(dir $@)
	ar rcs $@ $^

GUI_HOST_CFLAGS := $(HOST_CFLAGS) -DOS01_HOST_TEST \
    -I$(GUI_HOST_DIR)/include \
    -I$(TESTS_DIR)/libgui/include \
    -I$(TESTS_DIR)/libgui/internal \
    -I$(TESTS_DIR)/libgfx \
    -I$(TESTS_DIR)/user/common \
    -I$(TESTS_DIR)/thirdpart \
    -I$(TESTS_DIR)/thirdpart/lvgl \
    -I$(TESTS_DIR)/config \
    -iquote $(TEST_INC) \
    -iquote $(TEST_MOCK) \
    -DLV_CONF_INCLUDE_SIMPLE \
    -DOS01_BUILD_PROFILE=\"$(PROFILE)\"

$(GUI_DETERMINISTIC_DIR)/platform.o: $(TESTS_DIR)/libgui/src/platform.c $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_DETERMINISTIC_DIR)/tick.o: $(TESTS_DIR)/libgui/src/tick.c $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_DETERMINISTIC_DIR)/gfx_client_policy.o: $(TESTS_DIR)/user/common/gfx_client_policy.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

# The libgui input unit (GUI-2 Task 6).  Every GUI host binary links it,
# because the platform TU owns the input lifecycle (step / terminal /
# destroy) and therefore references the input hooks.  The unit itself is
# device-free until an input is created: a binary that never creates an
# input keeps the GUI-1 behaviour exactly.
GUI_INPUT_UNIT_DIR := $(GUI_HOST_DIR)/input_unit
GUI_INPUT_UNIT_OBJS := $(GUI_INPUT_UNIT_DIR)/input.o \
                       $(GUI_INPUT_UNIT_DIR)/input_lvgl.o \
                       $(GUI_INPUT_UNIT_DIR)/input_focus.o \
                       $(GUI_INPUT_UNIT_DIR)/input_ps2.o \
                       $(GUI_INPUT_UNIT_DIR)/cursor_icon.o

$(GUI_INPUT_UNIT_DIR)/%.o: $(TESTS_DIR)/libgui/src/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

# The libgui widget unit (GUI-2b Task 1).  Like the input unit, every GUI
# host binary that links the REAL platform.o must also link it: platform.c
# owns the native widget registry lifetime (created with the platform,
# released after the display is gone).
GUI_WIDGET_DIR := $(GUI_HOST_DIR)/widget
GUI_WIDGET_UNIT_OBJS := $(GUI_WIDGET_DIR)/widget.o $(GUI_WIDGET_DIR)/layout.o \
                       $(GUI_WIDGET_DIR)/appearance.o $(GUI_WIDGET_DIR)/event.o \
                       $(GUI_WIDGET_DIR)/focus.o

$(GUI_WIDGET_DIR)/widget.o: $(TESTS_DIR)/libgui/src/widget.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_WIDGET_DIR)/layout.o: $(TESTS_DIR)/libgui/src/layout.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_WIDGET_DIR)/appearance.o: $(TESTS_DIR)/libgui/src/appearance.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_WIDGET_DIR)/event.o: $(TESTS_DIR)/libgui/src/event.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_WIDGET_DIR)/focus.o: $(TESTS_DIR)/libgui/src/focus.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_DETERMINISTIC_DIR)/gui_test_runtime.o: $(TEST_MOCK)/gui_test_runtime.c $(TEST_MOCK)/gui_test_runtime.h $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_DETERMINISTIC_DIR)/test_gui_platform.o: $(TEST_CASES)/test_gui_platform.c $(TEST_MOCK)/gui_test_runtime.h $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

GUI_PLATFORM_WRAPS := \
    -Wl,--wrap=clock_gettime \
    -Wl,--wrap=calloc \
    -Wl,--wrap=mmap \
    -Wl,--wrap=munmap \
    -Wl,--wrap=open \
    -Wl,--wrap=read \
    -Wl,--wrap=close \
    -Wl,--wrap=gfx_open \
    -Wl,--wrap=gfx_get_info \
    -Wl,--wrap=gfx_acquire \
    -Wl,--wrap=gfx_present \
    -Wl,--wrap=gfx_release \
    -Wl,--wrap=gfx_close \
    -Wl,--wrap=gfx_sprite_blit \
    -Wl,--wrap=gfx_bind_input \
    -Wl,--wrap=gfx_unbind_input \
    -Wl,--wrap=gfx_get_display_state \
    -Wl,--wrap=lv_timer_handler \
    -Wl,--wrap=lv_display_flush_ready \
    -Wl,--wrap=lv_display_create

$(TEST_BLD)/test_gui_platform.elf: \
		$(GUI_DETERMINISTIC_DIR)/test_gui_platform.o \
		$(GUI_DETERMINISTIC_DIR)/gui_test_runtime.o \
		$(GUI_DETERMINISTIC_DIR)/platform.o \
		$(GUI_WIDGET_UNIT_OBJS) \
		$(GUI_THEME_OBJS) \
		$(GUI_DETERMINISTIC_DIR)/tick.o \
		$(GUI_DETERMINISTIC_DIR)/gfx_client_policy.o \
		$(GUI_INPUT_UNIT_OBJS) \
		$(GUI_LVGL_ARCHIVE)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_PLATFORM_WRAPS) -o $@ $^ -lpthread -lm

.PHONY: test-gui-platform
test-gui-platform: $(TEST_BLD)/test_gui_platform.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

$(GUI_INTEGRATION_DIR)/platform.o: $(TESTS_DIR)/libgui/src/platform.c $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -DGUI_INTEGRATION_TEST -MMD -MP -c $< -o $@

$(GUI_INTEGRATION_DIR)/tick.o: $(TESTS_DIR)/libgui/src/tick.c $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -DGUI_INTEGRATION_TEST -MMD -MP -c $< -o $@

$(GUI_INTEGRATION_DIR)/gfx_client_policy.o: $(TESTS_DIR)/user/common/gfx_client_policy.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -DGUI_INTEGRATION_TEST -MMD -MP -c $< -o $@

$(GUI_INTEGRATION_DIR)/gui_test_runtime.o: $(TEST_MOCK)/gui_test_runtime.c $(TEST_MOCK)/gui_test_runtime.h $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -DGUI_INTEGRATION_TEST -MMD -MP -c $< -o $@

$(GUI_INTEGRATION_DIR)/test_gui_lvgl_integration.o: $(TEST_CASES)/test_gui_lvgl_integration.c $(TEST_MOCK)/gui_test_runtime.h $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -DGUI_INTEGRATION_TEST -MMD -MP -c $< -o $@

GUI_INTEGRATION_WRAPS := \
    -Wl,--wrap=clock_gettime \
    -Wl,--wrap=calloc \
    -Wl,--wrap=mmap \
    -Wl,--wrap=munmap \
    -Wl,--wrap=open \
    -Wl,--wrap=read \
    -Wl,--wrap=close \
    -Wl,--wrap=gfx_open \
    -Wl,--wrap=gfx_get_info \
    -Wl,--wrap=gfx_acquire \
    -Wl,--wrap=gfx_present \
    -Wl,--wrap=gfx_release \
    -Wl,--wrap=gfx_close \
    -Wl,--wrap=gfx_sprite_blit \
    -Wl,--wrap=gfx_bind_input \
    -Wl,--wrap=gfx_unbind_input \
    -Wl,--wrap=gfx_get_display_state

$(TEST_BLD)/test_gui_lvgl_integration.elf: \
		$(GUI_INTEGRATION_DIR)/test_gui_lvgl_integration.o \
		$(GUI_INTEGRATION_DIR)/gui_test_runtime.o \
		$(GUI_INTEGRATION_DIR)/platform.o \
		$(GUI_WIDGET_UNIT_OBJS) \
		$(GUI_THEME_OBJS) \
		$(GUI_INTEGRATION_DIR)/tick.o \
		$(GUI_INTEGRATION_DIR)/gfx_client_policy.o \
		$(GUI_INPUT_UNIT_OBJS) \
		$(GUI_LVGL_ARCHIVE)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_INTEGRATION_WRAPS) -o $@ $^ -lpthread -lm

.PHONY: test-gui-lvgl
test-gui-lvgl: $(TEST_BLD)/test_gui_lvgl_integration.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── GUI-3 Task 1: Material design token table ────────────────────
# The token table (libgui/src/theme.c) is pure C with no LVGL dependency,
# so this case links ONLY the token table plus the test TU: the design
# system's values are verified without dragging in the LVGL archive.  The
# assertions cover the spec §9.1 contract: well-formed color roles,
# on_*/base pairing, strictly decreasing font scale, strictly increasing
# space and radius scales, monotonic elevation shadows, the elevation
# surface sequence, and -1 for every out-of-range enum.
GUI_THEME_UNIT_DIR := $(GUI_HOST_DIR)/theme_unit
GUI_THEME_UNIT_OBJS := $(GUI_THEME_UNIT_DIR)/theme.o

$(GUI_THEME_UNIT_DIR)/%.o: $(TESTS_DIR)/libgui/src/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_THEME_UNIT_DIR)/test_gui_theme.o: $(TEST_CASES)/test_gui_theme.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(TEST_BLD)/test_gui_theme.elf: \
		$(GUI_THEME_UNIT_DIR)/test_gui_theme.o \
		$(GUI_THEME_UNIT_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CC) -o $@ $^

.PHONY: test-gui-theme
test-gui-theme: $(TEST_BLD)/test_gui_theme.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── GUI-3 Task 2: Material theme applied to LVGL ──────────────────
# Links the REAL libgui/src/theme.c (token table) + theme_apply.c (LVGL
# theme) + the REAL platform and the REAL LVGL archive, so the assertions
# observe the production theme rather than a stub.  Covers: the theme is
# attached to the display and re-application is idempotent; container /
# button / label styles come from the token table; the focus ring is a
# 2px primary outline; a NULL platform yields NULL + EINVAL.
$(GUI_INTEGRATION_DIR)/theme.o: $(TESTS_DIR)/libgui/src/theme.c $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -DGUI_INTEGRATION_TEST -MMD -MP -c $< -o $@

$(GUI_INTEGRATION_DIR)/theme_apply.o: $(TESTS_DIR)/libgui/src/theme_apply.c $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -DGUI_INTEGRATION_TEST -MMD -MP -c $< -o $@

$(GUI_INTEGRATION_DIR)/test_gui_theme_apply.o: $(TEST_CASES)/test_gui_theme_apply.c $(TEST_MOCK)/gui_test_runtime.h $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -DGUI_INTEGRATION_TEST -MMD -MP -c $< -o $@

$(TEST_BLD)/test_gui_theme_apply.elf: \
		$(GUI_INTEGRATION_DIR)/test_gui_theme_apply.o \
		$(GUI_INTEGRATION_DIR)/gui_test_runtime.o \
		$(GUI_INTEGRATION_DIR)/platform.o \
		$(GUI_WIDGET_UNIT_OBJS) \
		$(GUI_THEME_OBJS) \
		$(GUI_INTEGRATION_DIR)/tick.o \
		$(GUI_INTEGRATION_DIR)/gfx_client_policy.o \
		$(GUI_INTEGRATION_DIR)/theme.o \
		$(GUI_INTEGRATION_DIR)/theme_apply.o \
		$(GUI_INPUT_UNIT_OBJS) \
		$(GUI_LVGL_ARCHIVE)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_INTEGRATION_WRAPS) -o $@ $^ -lpthread -lm

.PHONY: test-gui-theme-apply
test-gui-theme-apply: $(TEST_BLD)/test_gui_theme_apply.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── GUI-3 Task 3: desktop surface state machine ───────────────────
# The state machine is pure C (user/desktop/desktop_state.c), so this case
# needs neither LVGL nor the platform: only the real state machine plus
# the test TU.
.PHONY: test-gui-desktop-state
test-gui-desktop-state: $(TEST_BLD)/test_desktop_state.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# GUI-3 Task 4: the desktop shell.  Links the REAL desktop shell, registry
# and state machine against the REAL LVGL and the REAL offscreen platform,
# so the pixel assertions observe the production surfaces.  The shell needs
# the LVGL headers and the libgui public headers.
GUI_DESKTOP_DIR := $(GUI_HOST_DIR)/desktop
GUI_DESKTOP_OBJS := \
    $(GUI_DESKTOP_DIR)/desktop_shell.o \
    $(GUI_DESKTOP_DIR)/desktop_apps.o \
    $(GUI_DESKTOP_DIR)/desktop_registry.o \
    $(GUI_DESKTOP_DIR)/desktop_state.o

$(GUI_DESKTOP_DIR)/%.o: $(TESTS_DIR)/user/desktop/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_DESKTOP_DIR)/test_desktop_shell.o: $(TEST_CASES)/test_desktop_shell.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -I$(TESTS_DIR)/user/desktop -MMD -MP -c $< -o $@

$(TEST_BLD)/test_desktop_shell.elf: \
		$(GUI_DESKTOP_DIR)/test_desktop_shell.o \
		$(GUI_DESKTOP_OBJS) \
		$(GUI_INTEGRATION_DIR)/gui_test_runtime.o \
		$(GUI_INTEGRATION_DIR)/platform.o \
		$(GUI_WIDGET_DIR)/gui_native_fixture.o \
		$(GUI_WIDGET_UNIT_OBJS) \
		$(GUI_INTEGRATION_DIR)/tick.o \
		$(GUI_INTEGRATION_DIR)/gfx_client_policy.o \
		$(GUI_THEME_OBJS) \
		$(GUI_INPUT_UNIT_OBJS) \
		$(GUI_LVGL_ARCHIVE)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_INTEGRATION_WRAPS) -o $@ $^ -lpthread -lm

.PHONY: test-gui-desktop-shell
test-gui-desktop-shell: $(TEST_BLD)/test_desktop_shell.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── GUI-3 Task 5: desktop source audit ───────────────────────────
# The design-system discipline ("colors come from the token table") and
# the "no window system" promise are constraints on the SHAPE of the
# code, not on its behaviour, so they are audited statically.  The
# audit reads the real sources from the repository (the path comes from
# the build system, never from the runtime cwd).
$(TEST_BLD)/test_desktop_audit.o: $(TEST_CASES)/test_desktop_audit.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) \
	    -DGUI_DESKTOP_SRC=\"$(TESTS_DIR)/user/desktop\" -c $< -o $@

$(TEST_BLD)/test_desktop_audit.elf: $(TEST_BLD)/test_desktop_audit.o
	$(HOST_CC) -o $@ $^

.PHONY: test-gui-desktop-audit
test-gui-desktop-audit: $(TEST_BLD)/test_desktop_audit.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── GUI-2b Task 1: the native widget registry ─────────────────────
# The widget fixture itself: the host-private native fixture plus the
# public-contract test case.
$(GUI_WIDGET_DIR)/gui_native_fixture.o: $(TEST_MOCK)/gui_native_fixture.c \
		$(TEST_MOCK)/gui_native_fixture.h \
		$(TESTS_DIR)/libgui/internal/gui_widget_private.h \
		$(TESTS_DIR)/libgui/internal/gui_platform_private.h \
		$(TESTS_DIR)/libgui/internal/gui_input_private.h \
		$(TESTS_DIR)/libgui/internal/gui_lvgl_private.h \
		$(TESTS_DIR)/libgui/include/gui/gui_layout.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_WIDGET_DIR)/test_gui_widget.o: $(TEST_CASES)/test_gui_widget.c \
		$(TEST_MOCK)/gui_native_fixture.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

# GUI-2b Task 4: the typed geometry / layout / text-mode contract.
$(GUI_WIDGET_DIR)/test_gui_widget_layout.o: $(TEST_CASES)/test_gui_widget_layout.c \
		$(TEST_MOCK)/gui_native_fixture.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(TEST_BLD)/test_gui_widget_layout.elf: \
		$(GUI_WIDGET_DIR)/test_gui_widget_layout.o \
		$(GUI_WIDGET_DIR)/gui_native_fixture.o \
		$(GUI_WIDGET_UNIT_OBJS) \
		$(GUI_INTEGRATION_DIR)/gui_test_runtime.o \
		$(GUI_INTEGRATION_DIR)/platform.o \
		$(GUI_THEME_OBJS) \
		$(GUI_INTEGRATION_DIR)/tick.o \
		$(GUI_INTEGRATION_DIR)/gfx_client_policy.o \
		$(GUI_INPUT_UNIT_OBJS) \
		$(GUI_LVGL_ARCHIVE)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_INTEGRATION_WRAPS) -o $@ $^ -lpthread -lm

.PHONY: test-gui-widget-layout
test-gui-widget-layout: $(TEST_BLD)/test_gui_widget_layout.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# GUI-2b Task 5: the Material appearance contract.  The fixture's
# focus-visual helper lets the test observe the theme's focus ring through
# the public API only.
$(GUI_WIDGET_DIR)/test_gui_widget_appearance.o: $(TEST_CASES)/test_gui_widget_appearance.c \
		$(TEST_MOCK)/gui_native_fixture.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(TEST_BLD)/test_gui_widget_appearance.elf: \
		$(GUI_WIDGET_DIR)/test_gui_widget_appearance.o \
		$(GUI_WIDGET_DIR)/gui_native_fixture.o \
		$(GUI_WIDGET_UNIT_OBJS) \
		$(GUI_INTEGRATION_DIR)/gui_test_runtime.o \
		$(GUI_INTEGRATION_DIR)/platform.o \
		$(GUI_THEME_OBJS) \
		$(GUI_INTEGRATION_DIR)/tick.o \
		$(GUI_INTEGRATION_DIR)/gfx_client_policy.o \
		$(GUI_INPUT_UNIT_OBJS) \
		$(GUI_LVGL_ARCHIVE)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_INTEGRATION_WRAPS) -o $@ $^ -lpthread -lm

.PHONY: test-gui-widget-appearance
test-gui-widget-appearance: $(TEST_BLD)/test_gui_widget_appearance.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# GUI-2b Task 6: the native event bridge and the platform statistics.
$(GUI_WIDGET_DIR)/test_gui_widget_event.o: $(TEST_CASES)/test_gui_widget_event.c \
		$(TEST_MOCK)/gui_native_fixture.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(TEST_BLD)/test_gui_widget_event.elf: \
		$(GUI_WIDGET_DIR)/test_gui_widget_event.o \
		$(GUI_WIDGET_DIR)/gui_native_fixture.o \
		$(GUI_WIDGET_UNIT_OBJS) \
		$(GUI_INTEGRATION_DIR)/gui_test_runtime.o \
		$(GUI_INTEGRATION_DIR)/platform.o \
		$(GUI_THEME_OBJS) \
		$(GUI_INTEGRATION_DIR)/tick.o \
		$(GUI_INTEGRATION_DIR)/gfx_client_policy.o \
		$(GUI_INPUT_UNIT_OBJS) \
		$(GUI_LVGL_ARCHIVE)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_INTEGRATION_WRAPS) -o $@ $^ -lpthread -lm

.PHONY: test-gui-widget-event
test-gui-widget-event: $(TEST_BLD)/test_gui_widget_event.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# GUI-2b Task 7: the single focus engine (native + diagnostic registration).
$(GUI_WIDGET_DIR)/test_gui_focus.o: $(TEST_CASES)/test_gui_focus.c \
		$(TEST_MOCK)/gui_native_fixture.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(TEST_BLD)/test_gui_focus.elf: \
		$(GUI_WIDGET_DIR)/test_gui_focus.o \
		$(GUI_WIDGET_DIR)/gui_native_fixture.o \
		$(GUI_WIDGET_UNIT_OBJS) \
		$(GUI_INTEGRATION_DIR)/gui_test_runtime.o \
		$(GUI_INTEGRATION_DIR)/platform.o \
		$(GUI_THEME_OBJS) \
		$(GUI_INTEGRATION_DIR)/tick.o \
		$(GUI_INTEGRATION_DIR)/gfx_client_policy.o \
		$(GUI_INPUT_UNIT_OBJS) \
		$(GUI_LVGL_ARCHIVE)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_INTEGRATION_WRAPS) -o $@ $^ -lpthread -lm

.PHONY: test-gui-focus
test-gui-focus: $(TEST_BLD)/test_gui_focus.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

$(TEST_BLD)/test_gui_widget.elf: \
		$(GUI_WIDGET_DIR)/test_gui_widget.o \
		$(GUI_WIDGET_DIR)/gui_native_fixture.o \
		$(GUI_WIDGET_UNIT_OBJS) \
		$(GUI_INTEGRATION_DIR)/gui_test_runtime.o \
		$(GUI_INTEGRATION_DIR)/platform.o \
		$(GUI_THEME_OBJS) \
		$(GUI_INTEGRATION_DIR)/tick.o \
		$(GUI_INTEGRATION_DIR)/gfx_client_policy.o \
		$(GUI_INPUT_UNIT_OBJS) \
		$(GUI_LVGL_ARCHIVE)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_INTEGRATION_WRAPS) -o $@ $^ -lpthread -lm

# The publication boundary of the widget API: a PURE type consumer (only
# the published headers) must compile as C11 and as C++17, and the
# NEGATIVE control — a value handle passed where a void* is expected —
# must NOT compile.  A handle that converts to void* could be forged.
.PHONY: gui-widget-public-types
gui-widget-public-types:
	@mkdir -p $(GUI_WIDGET_DIR)
	@echo "  [gui-widget] public type consumer: C11"
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fsyntax-only \
	    -I$(TESTS_DIR)/libgui/include \
	    $(TEST_CASES)/gui_widget_public_consumer.c
	@echo "  [gui-widget] public type consumer: C++17"
	$(HOST_CC) -std=c++17 -Wall -Wextra -Werror -fsyntax-only -x c++ \
	    -I$(TESTS_DIR)/libgui/include \
	    $(TEST_CASES)/gui_widget_public_consumer.c
	@echo "  [gui-widget] negative control: the value handle must NOT convert to void*"
	@if $(HOST_CC) -std=c11 -Wall -Wextra \
	    -Werror=incompatible-pointer-types -Werror=int-conversion \
	    -fsyntax-only -I$(TESTS_DIR)/libgui/include \
	    $(TEST_CASES)/gui_widget_public_negative.c \
	    >$(GUI_WIDGET_DIR)/negative_c.log 2>&1; then \
	    echo "ERROR: the value handle was accepted as a void* — the identity is forgeable" >&2; \
	    exit 1; \
	fi
	@grep -q "gui_widget_t" $(GUI_WIDGET_DIR)/negative_c.log || { \
	    echo "ERROR: the negative control failed for the wrong reason" >&2; \
	    cat $(GUI_WIDGET_DIR)/negative_c.log >&2; exit 1; }
	@if $(HOST_CC) -std=c++17 -Wall -Wextra -fsyntax-only -x c++ \
	    -I$(TESTS_DIR)/libgui/include \
	    $(TEST_CASES)/gui_widget_public_negative.c \
	    >$(GUI_WIDGET_DIR)/negative_cpp.log 2>&1; then \
	    echo "ERROR: the value handle was accepted as a void* (C++) — the identity is forgeable" >&2; \
	    exit 1; \
	fi
	@grep -q "gui_widget_t" $(GUI_WIDGET_DIR)/negative_cpp.log || { \
	    echo "ERROR: the negative control failed for the wrong reason (C++)" >&2; \
	    cat $(GUI_WIDGET_DIR)/negative_cpp.log >&2; exit 1; }
	@echo "  [gui-widget] public type consumer: PASS"

.PHONY: test-gui-widget
test-gui-widget: gui-widget-public-types $(TEST_BLD)/test_gui_widget.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $(TEST_BLD)/test_gui_widget.elf

# GUI-2 plan Task 1: file-aware devfs read/poll dispatch.  Host-compiles
# the REAL kernel/fs/devfs.c + kernel/fs/file.c + kernel/fs/poll.c
# GUI-2 plan Task 2: pure PS/2 Set 1 scan-sequence framer.  The framer
# is pure (no locks, no device IO, no TTY, no display), so the test needs
# no device mock at all: it host-compiles the REAL
# kernel/driver/input_ps2.c and drives it byte by byte.  The assertions
# observe the production framer, never a stub's return value.
# Tested production symbols: input_ps2_frame_reset, input_ps2_frame_feed.
.PHONY: test-gui-input-frame
test-gui-input-frame: $(TEST_BLD)/test_input_ps2_frame.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

$(TEST_BLD)/test_input_ps2_frame.o: $(TEST_CASES)/test_input_ps2_frame.c \
		$(TESTS_DIR)/kernel/include/driver/input_ps2.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) $(KERNEL_INC) -c $< -o $@

$(TEST_BLD)/test_input_ps2_frame.elf: $(TEST_BLD)/test_input_ps2_frame.o \
		$(TEST_BLD)/input_ps2_production.o
	$(HOST_CC) -o $@ $^

# GUI-2 plan Task 3: the exclusive physical-input lease.  The route owns
# the physical key state, the lease and the ACTIVE queues, so the fixture
# host-compiles the REAL kernel/driver/input_route.c (plus the REAL
# kernel/driver/input_ps2.c framer) and links them against the recording
# runtime in hosttests/mock/input_route_test_runtime.{c,h}.
#
# The recording runtime is not a convenience: the route's contract is a
# lock ORDER (route → NORMAL queue/poll) and an ATOMIC "authorize +
# dequeue + register waiter" window.  A no-op mock spinlock cannot show
# either, so the runtime records every acquisition with its nesting
# depth and the assertions look at the recorded order.
#
# Tested production symbols: input_route_init, input_route_open,
# input_route_bind, input_route_unbind, input_route_read,
# input_route_poll, input_route_keyboard_byte, input_route_mouse_event,
# input_route_release_file, input_route_set_normal_sinks.
.PHONY: test-gui-input-route
test-gui-input-route: $(TEST_BLD)/test_input_route.elf $(TEST_BLD)/test_gfx_input.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $(TEST_BLD)/test_input_route.elf \
	    $(TEST_BLD)/test_gfx_input.elf

ROUTE_HOST_CFLAGS := $(HOST_CFLAGS) -ffunction-sections -fdata-sections \
    -Wno-unused-function -D__SPINLOCK_H__ -DOS01_HOST_TEST \
    -include $(TEST_MOCK)/input_route_test_runtime.h \
    -I$(TEST_MOCK) $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)

$(TEST_BLD)/test_input_route.o: $(TEST_CASES)/test_input_route.c \
		$(TESTS_DIR)/kernel/include/driver/input_route.h \
		$(TEST_MOCK)/input_route_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(ROUTE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/input_route_production.o: $(TESTS_DIR)/kernel/driver/input_route.c \
		$(TESTS_DIR)/kernel/include/driver/input_route.h \
		$(TESTS_DIR)/kernel/include/driver/input_ps2.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(ROUTE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/input_route_runtime.o: $(TEST_MOCK)/input_route_test_runtime.c \
		$(TEST_MOCK)/input_route_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(ROUTE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_input_route.elf: $(TEST_BLD)/test_input_route.o \
		$(TEST_BLD)/input_route_production.o \
		$(TEST_BLD)/input_route_runtime.o \
		$(TEST_BLD)/input_ps2_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

# GUI-2 plan Task 4: the gfx input-lease UAPI.  The fixture host-
# compiles the REAL kernel/driver/gfx.c (the GFX_BIND_INPUT /
# GFX_UNBIND_INPUT ioctls), the REAL kernel/driver/fb_state.c (the
# owner / lease coordinator) and the REAL kernel/driver/input_route.c
# (the lease itself) and asserts the spec §4.1 contract: the 32-byte
# INOUT struct, EFAULT / EBADF / EINVAL / EACCES / ESTALE / EBUSY, the
# exclusive lease, the copy-out rollback that must not revoke a newer
# generation, and the libgfx wrapper.
#
# The REAL libgfx/gfx.c is linked through the same --wrap harness the
# gfx client test uses, so the userspace wrapper is observed exactly as
# it calls the kernel.
GFX_INPUT_TEST_CFLAGS := $(HOST_CFLAGS) -DOS01_HOST_TEST -I$(TEST_MOCK) \
    -include $(TEST_MOCK)/fb_resolution_runtime.h \
    $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC) -I$(TESTS_DIR)/libgfx \
    -D'errno=(*__errno_location())' -include $(TEST_INC)/gfx_client_errno.h

$(TEST_BLD)/test_gfx_input.o: $(TEST_CASES)/test_gfx_input.c \
		$(TESTS_DIR)/kernel/include/uapi/gfx.h \
		$(TESTS_DIR)/kernel/include/driver/fb_state.h \
		$(TESTS_DIR)/kernel/include/driver/input_route.h \
		$(TESTS_DIR)/libgfx/gfx.h \
		$(TEST_MOCK)/fb_resolution_runtime.h \
		$(TEST_INC)/test_framework.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_INPUT_TEST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_gfx_input.elf: $(TEST_BLD)/test_gfx_input.o \
		$(TEST_BLD)/gfx_production.o \
		$(TEST_BLD)/fb_state.o \
		$(TEST_BLD)/fb_map.o \
		$(TEST_BLD)/fb_resolution_runtime.o \
		$(TEST_BLD)/input_route_production.o \
		$(TEST_BLD)/input_route_runtime.o \
		$(TEST_BLD)/input_ps2_production.o \
		$(TEST_BLD)/libgfx_gfx_production.o
	$(HOST_CC) -lpthread \
	    -Wl,--wrap=open,--wrap=close,--wrap=ioctl,--wrap=malloc,--wrap=calloc,--wrap=free,--wrap=mmap,--wrap=munmap \
	    -o $@ $^

.PHONY: test-gui-gfx-input
test-gui-gfx-input: $(TEST_BLD)/test_gfx_input.elf
	$(TEST_BLD)/test_gfx_input.elf

# GUI-2 plan Task 5: the native input API surface (libgui/include/gui/
# gui_input.h) and the PURE userspace PS/2 decoder (libgui/src/
# input_ps2.c).  The decoder has no device access, no locks and no LVGL,
# so the fixture needs no mock at all: it host-compiles the REAL
# production decoder and drives it byte by byte, and asserts the spec
# §5.1 / §6.1 contract (US ASCII, the frozen modifier bit layout, the
# UP identity, the typematic make, the swallowed PrintScreen / Pause
# sequences, the 100 ms truncated-prefix expiry, and the keys that
# report their physical identity only).
#
# The decoder TU and the test TU both set errno, so they use the
# repo's errno ABI shim (the same mechanism as the gfx client test):
# hosttests/include/errno.h declares a non-TLS `extern int errno;`
# while host glibc provides a TLS errno, so `errno` is defined as
# glibc's TLS accessor for this binary only.
GUI_INPUT_HOST_CFLAGS := $(HOST_CFLAGS) \
    -I$(TESTS_DIR)/libgui/include -I$(TESTS_DIR)/libgui/internal \
    -I$(TEST_INC) $(LIBC_INC) \
    -D'errno=(*__errno_location())' -include $(TEST_INC)/gfx_client_errno.h

.PHONY: test-gui-input-decode
test-gui-input-decode: $(TEST_BLD)/test_gui_input_decode.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

$(TEST_BLD)/test_gui_input_decode.o: $(TEST_CASES)/test_gui_input_decode.c \
		$(TESTS_DIR)/libgui/include/gui/gui_input.h \
		$(TESTS_DIR)/libgui/internal/gui_input_ps2.h \
		$(TEST_INC)/test_framework.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GUI_INPUT_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/gui_input_ps2_production.o: $(TESTS_DIR)/libgui/src/input_ps2.c \
		$(TESTS_DIR)/libgui/include/gui/gui_input.h \
		$(TESTS_DIR)/libgui/internal/gui_input_ps2.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GUI_INPUT_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_gui_input_decode.elf: $(TEST_BLD)/test_gui_input_decode.o \
		$(TEST_BLD)/gui_input_ps2_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

# GUI-2 plan Task 10 (fix round 1): the guest input probe's nonce rule.
# The nonce is interpolated into the control file's path, so it must be
# hex: a path escape is then impossible by construction.  The fixture
# host-compiles the REAL rule (user/test_gui_input/gui_input_nonce.h, the
# header the guest probe uses) and pins the accept / reject table and the
# fixed control path shape.
.PHONY: test-gui-input-nonce
test-gui-input-nonce: $(TEST_BLD)/test_gui_input_nonce.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

$(TEST_BLD)/test_gui_input_nonce.o: $(TEST_CASES)/test_gui_input_nonce.c \
		$(TESTS_DIR)/user/test_gui_input/gui_input_nonce.h \
		$(TEST_INC)/test_framework.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TESTS_DIR)/user/test_gui_input \
	    -I$(TEST_INC) -c $< -o $@

$(TEST_BLD)/test_gui_input_nonce.elf: $(TEST_BLD)/test_gui_input_nonce.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

# GUI-2 plan Task 6: the bounded input lifecycle, the FIFO, the fair pump
# and the platform error convergence.  The fixture host-compiles the REAL
# libgui input unit (libgui/src/input.c + libgui/src/input_lvgl.c), the
# REAL platform (libgui/src/platform.c) and the REAL PS/2 decoder
# (libgui/src/input_ps2.c) and links them against the recording runtime
# in hosttests/mock/gui_test_runtime.{c,h} plus the REAL LVGL archive.
#
# The assertions observe the production code paths: the creation order
# and its rollback, the FIFO capacity, the per-device pump budgets, the
# poll-flag → error mapping and the platform terminal cleanup.  The LVGL
# adapter hook is wrapped ONLY to inject a create failure; the real hook
# is linked and used by every other assertion (including the 100
# create/destroy cycles on the real LVGL release path).
GUI_INPUT_DIR := $(GUI_HOST_DIR)/input

.PHONY: test-gui-input-platform
test-gui-input-platform: $(TEST_BLD)/test_gui_input_platform.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

$(GUI_INPUT_DIR)/test_gui_input_platform.o: $(TEST_CASES)/test_gui_input_platform.c \
		$(TEST_MOCK)/gui_test_runtime.h $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_INPUT_DIR)/gui_test_runtime.o: $(TEST_MOCK)/gui_test_runtime.c $(TEST_MOCK)/gui_test_runtime.h $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_INPUT_DIR)/platform.o: $(TESTS_DIR)/libgui/src/platform.c $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_INPUT_DIR)/tick.o: $(TESTS_DIR)/libgui/src/tick.c $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_INPUT_DIR)/gfx_client_policy.o: $(TESTS_DIR)/user/common/gfx_client_policy.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

#
# GUI-2 输入单元的共享链接包装：记录型 mock（设备 / 时钟 / gfx）与 LVGL 的
# 观察点。两个 GUI 输入二进制共用这一份清单，避免两份列表各自漂移。
GUI_INPUT_WRAPS_COMMON := \
    -Wl,--wrap=clock_gettime \
    -Wl,--wrap=calloc \
    -Wl,--wrap=mmap \
    -Wl,--wrap=munmap \
    -Wl,--wrap=open \
    -Wl,--wrap=read \
    -Wl,--wrap=close \
    -Wl,--wrap=gfx_open \
    -Wl,--wrap=gfx_get_info \
    -Wl,--wrap=gfx_acquire \
    -Wl,--wrap=gfx_present \
    -Wl,--wrap=gfx_release \
    -Wl,--wrap=gfx_close \
    -Wl,--wrap=gfx_sprite_blit \
    -Wl,--wrap=gfx_bind_input \
    -Wl,--wrap=gfx_unbind_input \
    -Wl,--wrap=gfx_get_display_state \
    -Wl,--wrap=lv_display_flush_ready \
    -Wl,--wrap=lv_display_create \
    -Wl,--wrap=lv_indev_read \
    -Wl,--wrap=gui_input_lvgl_create

# input-platform 额外把 lv_timer_handler 换成只计数的桩（不渲染）。
GUI_INPUT_PLATFORM_WRAPS := $(GUI_INPUT_WRAPS_COMMON) \
    -Wl,--wrap=lv_timer_handler

$(TEST_BLD)/test_gui_input_platform.elf: \
		$(GUI_INPUT_DIR)/test_gui_input_platform.o \
		$(GUI_INPUT_DIR)/gui_test_runtime.o \
		$(GUI_INPUT_DIR)/platform.o \
		$(GUI_WIDGET_UNIT_OBJS) \
		$(GUI_THEME_OBJS) \
		$(GUI_INPUT_DIR)/tick.o \
		$(GUI_INPUT_UNIT_OBJS) \
		$(GUI_INPUT_DIR)/gfx_client_policy.o \
		$(GUI_LVGL_ARCHIVE)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_INPUT_PLATFORM_WRAPS) -o $@ $^ -lpthread -lm

# GUI-2 plan Task 7: the keyboard half of the LVGL adapter — keypad indev
# behaviour, the software repeat policy, native shortcut delivery and the
# in-process focus group with Tab navigation.
#
# The fixture host-compiles the REAL libgui input unit (libgui/src/input.c
# + input_lvgl.c + input_focus.c), the REAL platform and the REAL PS/2
# decoder, and links them against the recording runtime plus the REAL LVGL
# archive.  Nothing under test is mocked: the keypad indev, the focus group
# and the event handling are the production ones.
#
# Synthetic Set 1 bytes enter through the private injection bridge
# (gui_input_test_feed_keybytes), which is compiled ONLY for the host build
# (-DOS01_HOST_TEST) and therefore never reaches the public sysroot.
GUI_INPUT_LVGL_DIR := $(GUI_HOST_DIR)/input_lvgl

# The input-lvgl binary reuses the shared input wrapper set (the keypad read
# observation point) but keeps the REAL lv_timer_handler: the focus outline is
# asserted by sampling rendered pixels, so LVGL must actually refresh the
# display (the shared mock replaces the timer handler with a counting stub
# that never renders).
GUI_INPUT_LVGL_WRAPS := $(GUI_INPUT_WRAPS_COMMON)

.PHONY: test-gui-input-lvgl
test-gui-input-lvgl: $(TEST_BLD)/test_gui_input_lvgl.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

$(GUI_INPUT_LVGL_DIR)/test_gui_input_lvgl.o: $(TEST_CASES)/test_gui_input_lvgl.c \
		$(TEST_MOCK)/gui_test_runtime.h $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_INPUT_LVGL_DIR)/gui_test_runtime.o: $(TEST_MOCK)/gui_test_runtime.c $(TEST_MOCK)/gui_test_runtime.h $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_INPUT_LVGL_DIR)/platform.o: $(TESTS_DIR)/libgui/src/platform.c $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_INPUT_LVGL_DIR)/tick.o: $(TESTS_DIR)/libgui/src/tick.c $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_INPUT_LVGL_DIR)/gfx_client_policy.o: $(TESTS_DIR)/user/common/gfx_client_policy.c $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(TEST_BLD)/test_gui_input_lvgl.elf: \
		$(GUI_INPUT_LVGL_DIR)/test_gui_input_lvgl.o \
		$(GUI_INPUT_LVGL_DIR)/gui_test_runtime.o \
		$(GUI_INPUT_LVGL_DIR)/platform.o \
		$(GUI_WIDGET_UNIT_OBJS) \
		$(GUI_THEME_OBJS) \
		$(GUI_INPUT_LVGL_DIR)/tick.o \
		$(GUI_INPUT_LVGL_DIR)/gfx_client_policy.o \
		$(GUI_INPUT_UNIT_OBJS) \
		$(GUI_LVGL_ARCHIVE)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_INPUT_LVGL_WRAPS) -o $@ $^ -lpthread -lm

-include $(patsubst %.o,%.d,$(GUI_LVGL_OBJS))
-include $(GUI_DETERMINISTIC_DIR)/*.d
-include $(GUI_INTEGRATION_DIR)/*.d
-include $(GUI_INPUT_DIR)/*.d
-include $(GUI_INPUT_LVGL_DIR)/*.d
-include $(GUI_INPUT_UNIT_DIR)/*.d
# The widget unit's objects carry the private bridge headers (the slot
# layout lives there): without this the header changes would NOT trigger a
# rebuild and the fixture would disagree with the library about the slot
# size.
-include $(GUI_WIDGET_DIR)/*.d
