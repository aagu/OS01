# ── Profile-only contract ───────────────────────────────────
# The root Makefile is the only supported entry; hosttests/Makefile
# requires OS01_PROFILE_FILE at parse time (root `test` invokes it
# via os01_submake). Host tests are an OS01 build: every object and
# binary is written ONLY under the selected profile's host-test dir
# (HOST_TEST_BUILD_DIR) and profile clean removes it. Host compilation
# stays host-native (HOST_CC, no --target, no sysroot, no kernel link
# flags) but obeys profile output isolation.
ifeq ($(strip $(OS01_PROFILE_FILE)),)
$(error invoke from the repository root: make PROFILE=x86_64-clang <target>)
endif
ifeq ($(wildcard $(OS01_PROFILE_FILE)),)
$(error OS01_PROFILE_FILE='$(OS01_PROFILE_FILE)' does not exist; invoke from the repository root)
endif
include $(OS01_PROFILE_FILE)

.DEFAULT_GOAL := all

include mk/common.mk
include mk/registry.mk

# Subsystem host tests
include mk/framework.mk
include mk/libc.mk
include mk/memory.mk
include mk/sched.mk
include mk/fs.mk
include mk/intr.mk
include mk/drivers.mk
include mk/net.mk
include mk/display.mk
include mk/terminal.mk
include mk/monitor.mk
include mk/gui.mk

.PHONY: all clean
all: $(TEST_BINS) $(TEST_EXIT_STATUS_BINS)

clean:
	rm -rf $(TEST_BLD)

include mk/runner.mk
