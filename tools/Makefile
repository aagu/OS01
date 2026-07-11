# tools/Makefile
CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11

all: mkdisk

mkdisk: mkdisk.c
	$(CC) $(CFLAGS) -o $@ $<

check-deps:
	@missing=0; \
	for cmd in mkfs.vfat mcopy mmd mke2fs debugfs; do \
	    command -v $$cmd >/dev/null 2>&1 || { echo "ERROR: missing $$cmd (install dosfstools mtools e2fsprogs)"; missing=1; }; \
	done; \
	exit $$missing

clean:
	rm -f mkdisk
