# GUI-2b: the desktop is a libgui application.  It uses **only** libgui's
# public, LVGL-free surface: native widgets come from the widget registry and
# keyboard focus comes from the shared focus engine.  The desktop therefore
# needs no third-party include paths at all — the sysroot deliberately does not
# publish LVGL headers, and the desktop must not depend on them.
APP_SOURCES := desktop.c desktop_shell.c desktop_apps.c desktop_registry.c desktop_state.c
APP_CFLAGS := -I$(OS01_ROOT)/config
# The system app reports the build profile; the name comes from the build
# system, never from a literal inside the desktop sources.
APP_CFLAGS += -DOS01_BUILD_PROFILE=\"$(PROFILE)\"
APP_LDLIBS := -lgui -lgfx
