# config/rootfs.mk — version-controlled disk-image input manifest (spec:
# "Rootfs manifest"). Programs use the shared discovery list. sh.c is built
# for development, while /bin/sh remains the BusyBox applet.
ROOTFS_USER_PROGRAMS := $(filter-out sh,$(USER_PROGRAMS))
ROOTFS_FILES := $(foreach p,$(ROOTFS_USER_PROGRAMS),/bin/$(p)=$(USER_ARTIFACT_DIR)/$(p).elf:0755) \
                /bin/busybox=$(USER_ARTIFACT_DIR)/busybox.elf:0755 \
                /kernel.bin=$(KERNEL_ARTIFACT):0644 /etc/inittab=$(INITTAB_FILE):0644
ROOTFS_SYMLINKS := /bin/wget=busybox /bin/login=busybox /bin/sh=busybox /bin/[=busybox /bin/[[=busybox /bin/cat=busybox /bin/cp=busybox /bin/mv=busybox /bin/rm=busybox /bin/mkdir=busybox /bin/rmdir=busybox /bin/echo=busybox /bin/printf=busybox /bin/sort=busybox /bin/ps=busybox /bin/kill=busybox /bin/mount=busybox /bin/grep=busybox /bin/sed=busybox /bin/awk=busybox /bin/find=busybox /bin/ln=busybox /bin/xargs=busybox /bin/tar=busybox /bin/gzip=busybox /bin/gunzip=busybox /bin/ping=busybox /bin/ifconfig=busybox /bin/clear=busybox /bin/dmesg=busybox
# A discovered native program takes precedence over an applet symlink.
ROOTFS_SYMLINKS := $(filter-out $(foreach p,$(ROOTFS_USER_PROGRAMS),/bin/$(p)=%),$(ROOTFS_SYMLINKS))
