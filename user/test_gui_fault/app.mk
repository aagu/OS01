APP_SOURCES := test_gui_fault.c
APP_CFLAGS := -I$(OS01_ROOT)/libgui/internal -I$(OS01_ROOT)/thirdpart -I$(OS01_ROOT)/thirdpart/lvgl -I$(OS01_ROOT)/config -DLV_CONF_INCLUDE_SIMPLE
APP_LDLIBS := -lgui -lgfx
