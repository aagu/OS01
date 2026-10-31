# Terminal subsystem host tests
$(eval $(call register_host_test,test_terminal_core,terminal,protocol-or-legacy))
$(eval $(call register_host_test,test_terminal_display,terminal,protocol-or-legacy))
$(eval $(call register_host_test,test_terminal_render,terminal,protocol-or-legacy))
$(eval $(call register_host_test,test_user_log,terminal,protocol-or-legacy))
$(eval $(call register_host_test,test_user_log_off,terminal,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_terminal_core.elf \
    $(TEST_BLD)/test_terminal_render.elf \
    $(TEST_BLD)/test_terminal_display.elf \
    $(TEST_BLD)/test_user_log.elf \
    $(TEST_BLD)/test_user_log_off.elf

# Terminal core test — compiles REAL user/terminal/terminal_core.c (pure logic)
$(TEST_BLD)/terminal_core.o: $(TESTS_DIR)/user/terminal/terminal_core.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/user/terminal -c $< -o $@

$(TEST_BLD)/test_terminal_core.o: $(TEST_CASES)/test_terminal_core.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/user/terminal -c $< -o $@

$(TEST_BLD)/test_terminal_core.elf: $(TEST_BLD)/test_terminal_core.o $(TEST_BLD)/terminal_core.o
	$(HOST_CC) -Wl,--wrap=malloc -o $@ $^

# Terminal render test — compiles REAL user/terminal/terminal_render.c + terminal_core.c + libgfx
$(TEST_BLD)/terminal_render.o: $(TESTS_DIR)/user/terminal/terminal_render.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/user/terminal -I$(TESTS_DIR)/libgfx -I$(TESTS_DIR)/kernel/include -c $< -o $@

$(TEST_BLD)/test_terminal_render.o: $(TEST_CASES)/test_terminal_render.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/user/terminal -I$(TESTS_DIR)/libgfx -I$(TESTS_DIR)/kernel/include -DREPO_ROOT=\"$(TESTS_DIR)\" -c $< -o $@

$(TEST_BLD)/test_terminal_render.elf: $(TEST_BLD)/test_terminal_render.o \
		$(TEST_BLD)/terminal_render.o \
		$(TEST_BLD)/terminal_core.o \
		$(TEST_BLD)/libgfx_gfx_production.o \
		$(TEST_BLD)/libgfx_line_production.o \
		$(TEST_BLD)/libgfx_sprite_production.o
	$(HOST_CC) -o $@ $^

test-terminal-render: $(TEST_BLD)/test_terminal_render.elf
	$<

# Terminal display transaction test — REAL user/terminal/terminal_display.c with an
# injected libgfx/FBIOGET_STATE backend; malloc is wrapped for ENOMEM injection.
# -DFB_RESOLUTION_TEST compiles the Task-9 fbtest ENOMEM hook so
# test_terminal_fbtest_enomem_once can drive it; the tests always install a
# stub op, so the real /dev/fbtest path is never taken on the host.
# -DUSER_DEBUG=1 compiles the [term-present] emit path in (the production
# default is USER_DEBUG=0, where the emit path does not exist at all).
$(TEST_BLD)/terminal_display.o: $(TESTS_DIR)/user/terminal/terminal_display.c \
		$(TESTS_DIR)/user/terminal/terminal_display.h \
		$(TESTS_DIR)/user/common/user_log.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_CLIENT_HOST_CFLAGS) -DFB_RESOLUTION_TEST -DUSER_DEBUG=1 \
		-I$(TEST_INC) -I$(TESTS_DIR)/user/terminal -I$(TESTS_DIR)/user/common -c $< -o $@

$(TEST_BLD)/test_terminal_display.o: $(TEST_CASES)/test_terminal_display.c \
		$(TESTS_DIR)/user/terminal/terminal_display.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_CLIENT_HOST_CFLAGS) -I$(TEST_INC) -I$(TESTS_DIR)/user/terminal -c $< -o $@

$(TEST_BLD)/test_terminal_display.elf: $(TEST_BLD)/test_terminal_display.o \
		$(TEST_BLD)/terminal_display.o \
		$(TEST_BLD)/user_log.o \
		$(TEST_BLD)/terminal_render.o \
		$(TEST_BLD)/terminal_core.o \
		$(TEST_BLD)/libgfx_gfx_production.o \
		$(TEST_BLD)/libgfx_line_production.o \
		$(TEST_BLD)/libgfx_sprite_production.o
	$(HOST_CC) -Wl,--wrap=malloc -o $@ $^

test-terminal-display: $(TEST_BLD)/test_terminal_core.elf $(TEST_BLD)/test_terminal_render.elf $(TEST_BLD)/test_terminal_display.elf
	$(TEST_BLD)/test_terminal_core.elf
	$(TEST_BLD)/test_terminal_render.elf
	$(TEST_BLD)/test_terminal_display.elf

# ── User-space debug log facade (user/common/user_log.c) ────────────
# The log_debug() facade the terminal's [term-present] / [term-probe]
# records are emitted through.  Two builds of the same case:
#   * USER_DEBUG=1  — the sink routing / exact-text / error-propagation
#                     contract of the enabled path.
#   * USER_DEBUG=0  — the production default: the macro must be compiled
#                     out, so neither the format string nor its arguments
#                     are ever evaluated.
# NOTE: this TU deliberately does NOT get $(TEST_INC):
# hosttests/include/errno.h declares a non-TLS `extern int errno;` while
# host glibc provides a TLS errno, and linking the module against libc.so
# with a non-TLS reference fails with the "TLS definition ... mismatches
# non-TLS reference" link error.  Without $(TEST_INC) the module picks up
# glibc's <errno.h> and the two agree on the TLS accessor.
$(TEST_BLD)/user_log.o: $(TESTS_DIR)/user/common/user_log.c \
		$(TESTS_DIR)/user/common/user_log.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -DUSER_DEBUG=1 -I$(TESTS_DIR)/user/common -c $< -o $@

$(TEST_BLD)/test_user_log.o: $(TEST_CASES)/test_user_log.c \
		$(TESTS_DIR)/user/common/user_log.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) -DUSER_DEBUG=1 -I$(TESTS_DIR)/user/common -c $< -o $@

$(TEST_BLD)/test_user_log.elf: $(TEST_BLD)/test_user_log.o $(TEST_BLD)/user_log.o
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_user_log_off.o: $(TEST_CASES)/test_user_log.c \
		$(TESTS_DIR)/user/common/user_log.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) -I$(TESTS_DIR)/user/common -c $< -o $@

$(TEST_BLD)/test_user_log_off.elf: $(TEST_BLD)/test_user_log_off.o
	$(HOST_CC) -o $@ $^

test-user-log: $(TEST_BLD)/test_user_log.elf $(TEST_BLD)/test_user_log_off.elf
	$(TEST_BLD)/test_user_log.elf
	$(TEST_BLD)/test_user_log_off.elf
