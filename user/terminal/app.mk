APP_SOURCES := terminal.c terminal_core.c terminal_render.c terminal_display.c
APP_LDLIBS := -lgfx

# Custom resources are defined only in the user component, not the root parser.
ifeq ($(USER_BUILD_CONTEXT),1)
APP_EXTRA_OBJS := $(OBJ_DIR)/terminal/terminal_font.o
$(OBJ_DIR)/terminal/terminal_font.o: terminal/terminal_font.psf terminal/app.mk
	@mkdir -p $(dir $@)
	cd terminal && $(OBJ_CPY) -B i386 -I binary -O elf64-x86-64 terminal_font.psf $(abspath $@)
endif
