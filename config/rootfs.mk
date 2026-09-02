# config/rootfs.mk — version-controlled disk-image input manifest (spec:
# "Rootfs manifest"). Each ROOTFS_FILES item is dest=source:mode; each
# ROOTFS_SYMLINKS item is dest=target (BusyBox applet entry). image.mk parses
# these into a fresh staging tree and emits the tab-separated manifest that
# mkdisk fills the ext2 image from. Adding a program means adding one line
# here — never a recipe line in a Makefile.
#
# PLAN DEVIATION: ROOTFS_SYMLINKS entries are staged as regular-file COPIES
# of the busybox binary, not symlinks — the OS01 kernel has no symlink
# support in path lookup/exec (a symlink exec fails ENOEXEC), and the
# pre-refactor build likewise wrote busybox copies per applet. Applet
# dispatch is by argv[0] basename.
ROOTFS_FILES := /bin/init=$(USER_ARTIFACT_DIR)/init.elf:0755 /bin/busybox=$(USER_ARTIFACT_DIR)/busybox.elf:0755 /bin/spin=$(USER_ARTIFACT_DIR)/spin.elf:0755 /bin/sigtest=$(USER_ARTIFACT_DIR)/sigtest.elf:0755 /bin/poweroff=$(USER_ARTIFACT_DIR)/poweroff.elf:0755 /bin/halt=$(USER_ARTIFACT_DIR)/halt.elf:0755 /bin/reboot=$(USER_ARTIFACT_DIR)/reboot.elf:0755 /bin/systest=$(USER_ARTIFACT_DIR)/systest.elf:0755 /bin/test_mmap=$(USER_ARTIFACT_DIR)/test_mmap.elf:0755 /bin/test_fork_mmap=$(USER_ARTIFACT_DIR)/test_fork_mmap.elf:0755 /bin/test_cow=$(USER_ARTIFACT_DIR)/test_cow.elf:0755 /bin/terminal=$(USER_ARTIFACT_DIR)/terminal.elf:0755 /bin/smp_stress=$(USER_ARTIFACT_DIR)/smp_stress.elf:0755 /bin/socktest=$(USER_ARTIFACT_DIR)/socktest.elf:0755 /bin/udptest=$(USER_ARTIFACT_DIR)/udptest.elf:0755 /bin/ipaddr=$(USER_ARTIFACT_DIR)/ipaddr.elf:0755 /bin/nettest=$(USER_ARTIFACT_DIR)/nettest.elf:0755 /bin/tetris=$(USER_ARTIFACT_DIR)/tetris.elf:0755 /kernel.bin=$(KERNEL_ARTIFACT):0644 /etc/inittab=$(INITTAB_FILE):0644
ROOTFS_SYMLINKS := /bin/wget=busybox /bin/login=busybox /bin/sh=busybox /bin/[=busybox /bin/[[=busybox /bin/cat=busybox /bin/cp=busybox /bin/mv=busybox /bin/rm=busybox /bin/mkdir=busybox /bin/rmdir=busybox /bin/echo=busybox /bin/printf=busybox /bin/sort=busybox /bin/ps=busybox /bin/kill=busybox /bin/mount=busybox /bin/grep=busybox /bin/sed=busybox /bin/awk=busybox /bin/find=busybox /bin/xargs=busybox /bin/tar=busybox /bin/gzip=busybox /bin/gunzip=busybox /bin/ping=busybox /bin/ifconfig=busybox /bin/clear=busybox /bin/dmesg=busybox
