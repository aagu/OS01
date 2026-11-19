HOST_DEPFILES := $(sort $(wildcard $(TEST_BLD)/*.d $(TEST_BLD)/*/*.d $(TEST_BLD)/*/*/*.d $(HOST_EXTRA_DEPFILES)))

host_stale_dep_object = $(if $(filter $(1:.d=.o):,$(firstword $(2))),$(if $(filter %.c %.cc %.cpp %.cxx %.s %.S,$(word 2,$(2))),$(if $(wildcard $(word 2,$(2))),,$(1:.d=.o))))
HOST_STALE_DEP_OBJECTS := $(strip $(foreach depfile,$(HOST_DEPFILES),$(call host_stale_dep_object,$(depfile),$(filter-out \,$(file <$(depfile))))))

ifneq ($(HOST_STALE_DEP_OBJECTS),)
.PHONY: _host_depfile_rebuild
$(HOST_STALE_DEP_OBJECTS): _host_depfile_rebuild
endif

-include $(filter-out $(HOST_STALE_DEP_OBJECTS:.o=.d),$(HOST_DEPFILES))
