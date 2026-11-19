APP_SOURCES := terminal.c terminal_core.c terminal_render.c terminal_display.c
APP_LDLIBS := -lgfx

# Shared user-space debug log (user/common/user_log.{c,h}): the log_debug()
# facade the [term-present] / [term-probe] records are emitted through.
# With the default USER_DEBUG=0 the module compiles to nothing.
APP_SHARED_SOURCES := common/user_log.c

# Custom resources are defined only in the user component, not the root parser.
ifeq ($(USER_BUILD_CONTEXT),1)
ifeq ($(USER_ARCH),aarch64)
TERM_FONT_OBJCOPY_FLAGS := -B aarch64 -I binary -O elf64-littleaarch64
else
TERM_FONT_OBJCOPY_FLAGS := -B i386 -I binary -O elf64-x86-64
endif
APP_EXTRA_OBJS := $(OBJ_DIR)/terminal/terminal_font.o
$(OBJ_DIR)/terminal/terminal_font.o: terminal/terminal_font.psf terminal/app.mk
	@mkdir -p $(dir $@)
	cd terminal && $(OBJ_CPY) $(TERM_FONT_OBJCOPY_FLAGS) terminal_font.psf $(abspath $@)
endif
