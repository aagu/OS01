# GUI-3: the desktop is an LVGL application built on libgui.  The LVGL
# headers come from the source tree (the sysroot deliberately does not
# publish them, so that no published header exposes an LVGL symbol);
# the business API it uses is libgui's public, LVGL-free surface.
APP_SOURCES := desktop.c desktop_shell.c desktop_registry.c desktop_state.c
APP_CFLAGS := -I$(OS01_ROOT)/thirdpart -I$(OS01_ROOT)/thirdpart/lvgl -I$(OS01_ROOT)/config -DLV_CONF_INCLUDE_SIMPLE
APP_LDLIBS := -lgui -lgfx
