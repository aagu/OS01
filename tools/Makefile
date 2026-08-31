# tools/Makefile (host-only — runs on the build machine)
HOST_CC ?= cc
CFLAGS = -Wall -Wextra -O2 -std=c11

.PHONY: all mkdisk check-deps clean

all: mkdisk

# mkdisk is a tracked host binary. Always rebuild it so checkout/archive
# timestamps cannot make an older binary appear newer than mkdisk.c.
mkdisk: mkdisk.c
	$(HOST_CC) $(CFLAGS) -o $@ $<

check-deps:
	@missing=0; \
	for cmd in mkfs.vfat mcopy mmd mke2fs debugfs; do \
	    command -v $$cmd >/dev/null 2>&1 || { echo "ERROR: missing $$cmd (install dosfstools mtools e2fsprogs)"; missing=1; }; \
	done; \
	exit $$missing

clean:
	rm -f mkdisk
