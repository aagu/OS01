# Shared discovery contract: included by both root publishing and user builds.
# Only immediate user/*/app.mk directories define applications; common/ has none.
USER_SOURCE_DIR := $(OS01_ROOT)/user
USER_APP_MAKEFILES := $(sort $(wildcard $(USER_SOURCE_DIR)/*/app.mk))
USER_FLAT_SOURCES := $(notdir $(wildcard $(USER_SOURCE_DIR)/*.c))
ifneq ($(FB_RESOLUTION_TEST),1)
USER_FLAT_SOURCES := $(filter-out test_resolution.c,$(USER_FLAT_SOURCES))
endif
USER_FLAT_PROGRAMS := $(basename $(USER_FLAT_SOURCES))
USER_DIRECTORY_PROGRAMS := $(notdir $(patsubst %/,%,$(dir $(USER_APP_MAKEFILES))))
USER_DUPLICATES := $(filter $(USER_FLAT_PROGRAMS),$(USER_DIRECTORY_PROGRAMS))
ifneq ($(strip $(USER_DUPLICATES)),)
$(error duplicate user program names: $(USER_DUPLICATES))
endif
USER_PROGRAMS := $(sort $(USER_FLAT_PROGRAMS) $(USER_DIRECTORY_PROGRAMS))
ifneq ($(filter busybox,$(USER_PROGRAMS)),)
$(error user program name busybox is reserved for the BusyBox adapter)
endif

# Reset each manifest's inputs and snapshot immediately: no cross-app leakage.
# APP_SOURCES are relative to APP_DIR; APP_SHARED_SOURCES are relative to user/.
define load_user_app
APP_DIR := $(patsubst $(USER_SOURCE_DIR)/%,%,$(patsubst %/,%,$(dir $(1))))
APP_SOURCES :=
APP_SHARED_SOURCES :=
APP_CFLAGS :=
APP_LDFLAGS :=
APP_LDLIBS :=
APP_EXTRA_OBJS :=
include $(1)
ifeq ($$(strip $$(APP_SOURCES)),)
$$(error $(1) must define non-empty APP_SOURCES)
endif
USER_APP_$(notdir $(patsubst %/,%,$(dir $(1))))_SOURCES := $$(addprefix $$(APP_DIR)/,$$(APP_SOURCES)) $$(APP_SHARED_SOURCES)
USER_APP_$(notdir $(patsubst %/,%,$(dir $(1))))_CFLAGS := $$(APP_CFLAGS)
USER_APP_$(notdir $(patsubst %/,%,$(dir $(1))))_LDFLAGS := $$(APP_LDFLAGS)
USER_APP_$(notdir $(patsubst %/,%,$(dir $(1))))_LDLIBS := $$(APP_LDLIBS)
USER_APP_$(notdir $(patsubst %/,%,$(dir $(1))))_EXTRA_OBJS := $$(APP_EXTRA_OBJS)
endef
$(foreach app,$(USER_APP_MAKEFILES),$(eval $(call load_user_app,$(app))))
