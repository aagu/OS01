# config/rootfs.mk — version-controlled disk-image input manifest (spec:
# "Rootfs manifest"). Programs use the shared discovery list. sh.c is built
# for development, while /bin/sh remains the BusyBox applet.
# Flavor-aware program filter: exactly one stress program per variant.
ROOTFS_USER_PROGRAMS := $(if $(filter 1,$(ARCH6_FSSTRESS_TEST)),$(filter-out sh test_arch10_capacity,$(USER_PROGRAMS)),$(if $(filter 1,$(CAPACITY_TEST)),$(filter-out sh test_arch6_fsstress,$(USER_PROGRAMS)),$(filter-out sh test_arch10_capacity test_arch6_fsstress,$(USER_PROGRAMS))))
ROOTFS_FILES := $(foreach p,$(ROOTFS_USER_PROGRAMS),/bin/$(p)=$(USER_ARTIFACT_DIR)/$(p).elf:0755) \
                /bin/busybox=$(USER_ARTIFACT_DIR)/busybox.elf:0755 \
                /kernel.bin=$(KERNEL_ARTIFACT):0644 /etc/inittab=$(INITTAB_FILE):0644
ROOTFS_SYMLINKS := /bin/wget=busybox /bin/login=busybox /bin/sh=busybox /bin/[=busybox /bin/[[=busybox /bin/cat=busybox /bin/head=busybox /bin/tail=busybox /bin/cp=busybox /bin/mv=busybox /bin/rm=busybox /bin/mkdir=busybox /bin/rmdir=busybox /bin/echo=busybox /bin/printf=busybox /bin/sort=busybox /bin/ps=busybox /bin/kill=busybox /bin/mount=busybox /bin/grep=busybox /bin/sed=busybox /bin/awk=busybox /bin/find=busybox /bin/ln=busybox /bin/xargs=busybox /bin/tar=busybox /bin/gzip=busybox /bin/gunzip=busybox /bin/ping=busybox /bin/ifconfig=busybox /bin/clear=busybox /bin/dmesg=busybox
# A discovered native program takes precedence over an applet symlink.
ROOTFS_SYMLINKS := $(filter-out $(foreach p,$(ROOTFS_USER_PROGRAMS),/bin/$(p)=%),$(ROOTFS_SYMLINKS))

# D05: Optional Doom assets (installed only when all 4 asset variables are provided)
ifeq ($(DOOM_ASSET_COUNT),4)
DOOM_ASSET_DIR := $(BUILD_DIR)/doom-assets
ROOTFS_FILES += /usr/share/games/doom/iwad.wad=$(DOOM_ASSET_DIR)/iwad.wad:0644 \
                /usr/share/games/doom/IWAD-LICENSE.txt=$(DOOM_ASSET_DIR)/IWAD-LICENSE.txt:0644 \
                /usr/share/games/doom/IWAD-SOURCE.txt=$(DOOM_ASSET_DIR)/IWAD-SOURCE.txt:0644
endif

# D06: Doom engine provenance & license notice (installed whenever /bin/doom is published)
ifneq ($(filter doom,$(ROOTFS_USER_PROGRAMS)),)
DOOM_NOTICE_DIR := $(BUILD_DIR)/doom-notice
DOOM_PATCHES_MANIFEST ?= $(OS01_ROOT)/thirdpart/doomgeneric.manifest
-include $(DOOM_PATCHES_MANIFEST)
ROOTFS_FILES += /usr/share/licenses/doom/LICENSE=$(DOOM_NOTICE_DIR)/LICENSE:0644 \
                /usr/share/doc/doom/SOURCE.txt=$(DOOM_NOTICE_DIR)/SOURCE.txt:0644 \
                /usr/share/doc/doom/PATCHES.txt=$(DOOM_NOTICE_DIR)/PATCHES.txt:0644 \
                $(foreach patch,$(DOOMGENERIC_PATCHES),/usr/share/doc/doom/patches/$(patch)=$(DOOM_NOTICE_DIR)/patches/$(patch):0644)
endif


