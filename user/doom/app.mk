APP_SOURCES := doom_os01.c doom_video.c doom_input.c doom_runtime.c
APP_SHARED_SOURCES := common/gfx_client_policy.c
APP_LDLIBS := -lgui -lgfx
APP_EXTRA_OBJS := $(DOOM_ARCHIVE)
APP_CFLAGS := -I$(DOOM_PRIVATE_DIR)/doomgeneric \
              -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DOS01_DOOM

ifneq ($(filter 1,$(DOOM_TEST))$(filter doom-diag,$(USER_VARIANT)),)
APP_SOURCES += diag.c
APP_CFLAGS += -DOS01_DOOM_DIAG
endif

# Adapter objects depend on upstream source stamp
$(OBJ_DIR)/doom/%.o: $(DOOM_SOURCE_STAMP)
