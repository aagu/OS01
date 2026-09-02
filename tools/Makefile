# tools/Makefile (host-only — runs on the build machine)
HOST_CC ?= cc
CFLAGS = -Wall -Wextra -O2 -std=c11
# mkdisk builds into $(HOST_TOOLS_DIR) (the profile's host-tools dir), NOT
# into tools/ — the root image rule invokes
# `$(MAKE) -C tools HOST_TOOLS_DIR=$(BUILD_DIR)/host-tools` so parallel
# profiles cannot share or collide on the binary. The TARGET is
# $(HOST_TOOLS_DIR)/mkdisk so make's up-to-date check keys on the file the
# recipe actually writes (the old `mkdisk:` target keyed on tools/mkdisk, a
# stale tracked binary, and could skip the build).
HOST_TOOLS_DIR ?= .

.PHONY: all check-deps clean

all: $(HOST_TOOLS_DIR)/mkdisk

$(HOST_TOOLS_DIR)/mkdisk: mkdisk.c
	@mkdir -p $(HOST_TOOLS_DIR)
	$(HOST_CC) $(CFLAGS) -o $@ $<

check-deps:
	@missing=0; \
	for cmd in mkfs.vfat mcopy mmd mke2fs debugfs; do \
	    command -v $$cmd >/dev/null 2>&1 || { echo "ERROR: missing $$cmd (install dosfstools mtools e2fsprogs)"; missing=1; }; \
	done; \
	exit $$missing

clean:
	rm -f $(HOST_TOOLS_DIR)/mkdisk
