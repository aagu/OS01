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

# Adapter objects depend on the upstream source stamp: their compile
# flags point into the private copy, which the stamp recipe materializes.
# The rule must be EXPLICIT (one target per object): a pattern rule that
# has only prerequisites and no recipe is ignored by GNU make, so a
# clean-tree parallel build could compile these sources before the
# private copy (and doomgeneric.h) existed.
DOOM_ADAPTER_OBJECTS := $(addprefix $(OBJ_DIR)/doom/, \
    $(patsubst %.c,%.o,$(filter %.c,$(APP_SOURCES))))
$(DOOM_ADAPTER_OBJECTS): $(DOOM_SOURCE_STAMP)
