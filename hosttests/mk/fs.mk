# ── FS Subsystem Host Tests (spec §5) ───────────────────────────

$(eval $(call register_host_test,test_vfs_basic,fs,protocol-or-legacy))
$(eval $(call register_host_test,test_vfs_fs_registry,fs,protocol-or-legacy))
$(eval $(call register_host_test,test_vfs_iops_fops,fs,protocol-or-legacy))
$(eval $(call register_host_test,test_vfs_mount_tree,fs,protocol-or-legacy))
$(eval $(call register_host_test,test_vfs_dcache,fs,protocol-or-legacy))
$(eval $(call register_host_test,test_fat32_basic,fs,protocol-or-legacy))
$(eval $(call register_host_test,test_elf_validate,fs,protocol-or-legacy))
$(eval $(call register_host_test,test_elf_layout,fs,protocol-or-legacy))
$(eval $(call register_host_test,test_elf_load_4k,fs,protocol-or-legacy))
$(eval $(call register_host_test,test_canon,fs,protocol-or-legacy))
$(eval $(call register_host_test,test_poll_requested,fs,protocol-or-legacy))
$(eval $(call register_host_test,test_poll_waitqueue_handshake,fs,protocol-or-legacy))
$(eval $(call register_host_test,test_poll_capacity,fs,protocol-or-legacy))
$(eval $(call register_host_test,test_poll_nofile64,fs,protocol-or-legacy))
$(eval $(call register_host_test,test_pty_winsize,fs,protocol-or-legacy))
$(eval $(call register_host_test,test_devfs_input_dispatch,fs,protocol-or-legacy))
$(eval $(call register_host_test,test_devfs_capacity,fs,protocol-or-legacy))
$(eval $(call register_host_test,test_devfs_hierarchy,fs,protocol-or-legacy))
$(eval $(call register_host_test,test_nofile_limits,fs,protocol-or-legacy))
$(eval $(call register_host_test,test_pci_devfs,fs,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_vfs_basic.elf \
    $(TEST_BLD)/test_vfs_fs_registry.elf \
    $(TEST_BLD)/test_vfs_iops_fops.elf \
    $(TEST_BLD)/test_vfs_mount_tree.elf \
    $(TEST_BLD)/test_vfs_dcache.elf \
    $(TEST_BLD)/test_fat32_basic.elf \
    $(TEST_BLD)/test_elf_validate.elf \
    $(TEST_BLD)/test_canon.elf \
    $(TEST_BLD)/test_poll_requested.elf \
    $(TEST_BLD)/test_poll_waitqueue_handshake.elf \
    $(TEST_BLD)/test_poll_capacity.elf \
    $(TEST_BLD)/nofile64/test_poll_nofile64.elf \
    $(TEST_BLD)/test_elf_layout.elf \
    $(TEST_BLD)/test_elf_load_4k.elf \
    $(TEST_BLD)/test_pty_winsize.elf \
    $(TEST_BLD)/test_devfs_input_dispatch.elf \
    $(TEST_BLD)/test_devfs_capacity.elf \
    $(TEST_BLD)/test_devfs_hierarchy.elf \
    $(TEST_BLD)/test_nofile_limits.elf \
    $(TEST_BLD)/test_pci_devfs.elf

# Basic VFS & FAT32 tests
$(TEST_BLD)/test_vfs_basic.elf: $(TEST_BLD)/test_vfs_basic.o $(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_fat32_basic.elf: $(TEST_BLD)/test_fat32_basic.o $(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -o $@ $^

# Full VFS mock infrastructure
VFS_HOST_CFLAGS := $(HOST_CFLAGS) -ffunction-sections -fdata-sections \
    -Wno-unused-function -D__SPINLOCK_H__ -DOS01_HOST_TEST \
    -include $(TEST_MOCK)/vfs_test_runtime.h \
    -I$(TEST_MOCK)/include $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)

$(TEST_BLD)/vfs_production.o: $(TESTS_DIR)/kernel/fs/vfs.c \
		$(TESTS_DIR)/kernel/include/fs/vfs.h \
		$(TEST_MOCK)/vfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VFS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/dcache_production.o: $(TESTS_DIR)/kernel/fs/dcache.c \
		$(TESTS_DIR)/kernel/include/fs/dcache.h \
		$(TESTS_DIR)/kernel/include/fs/vfs.h \
		$(TEST_MOCK)/vfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VFS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/ext2_production.o: $(TESTS_DIR)/kernel/fs/ext2.c \
		$(TESTS_DIR)/kernel/include/fs/ext2.h \
		$(TESTS_DIR)/kernel/include/fs/vfs.h \
		$(TEST_MOCK)/vfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VFS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/fat_production.o: $(TESTS_DIR)/kernel/fs/fat.c \
		$(TESTS_DIR)/kernel/include/fs/fat.h \
		$(TESTS_DIR)/kernel/include/fs/vfs.h \
		$(TEST_MOCK)/vfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VFS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/tmpfs_production.o: $(TESTS_DIR)/kernel/fs/tmpfs.c \
		$(TESTS_DIR)/kernel/include/fs/tmpfs.h \
		$(TESTS_DIR)/kernel/include/fs/vfs.h \
		$(TEST_MOCK)/vfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VFS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/devfs_vfs_production.o: $(TESTS_DIR)/kernel/fs/devfs.c \
		$(TESTS_DIR)/kernel/include/fs/devfs.h \
		$(TESTS_DIR)/kernel/include/fs/vfs.h \
		$(TEST_MOCK)/vfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VFS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_vfs_fs_registry.o: $(TEST_CASES)/test_vfs_fs_registry.c \
		$(TESTS_DIR)/kernel/include/fs/vfs.h \
		$(TEST_MOCK)/vfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VFS_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_vfs_fs_registry.elf: $(TEST_BLD)/test_vfs_fs_registry.o \
		$(TEST_BLD)/vfs_production.o \
		$(TEST_BLD)/dcache_production.o \
		$(TEST_BLD)/ext2_production.o \
		$(TEST_BLD)/fat_production.o \
		$(TEST_BLD)/tmpfs_production.o \
		$(TEST_BLD)/devfs_vfs_production.o \
		$(TEST_BLD)/serial_sink_stub.o \
		$(TEST_BLD)/rwlock.o \
		$(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

$(TEST_BLD)/test_vfs_iops_fops.o: $(TEST_CASES)/test_vfs_iops_fops.c \
		$(TESTS_DIR)/kernel/include/fs/vfs.h \
		$(TEST_MOCK)/vfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VFS_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_vfs_iops_fops.elf: $(TEST_BLD)/test_vfs_iops_fops.o \
		$(TEST_BLD)/vfs_production.o \
		$(TEST_BLD)/dcache_production.o \
		$(TEST_BLD)/ext2_production.o \
		$(TEST_BLD)/fat_production.o \
		$(TEST_BLD)/tmpfs_production.o \
		$(TEST_BLD)/devfs_vfs_production.o \
		$(TEST_BLD)/serial_sink_stub.o \
		$(TEST_BLD)/rwlock.o \
		$(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

$(TEST_BLD)/test_vfs_mount_tree.o: $(TEST_CASES)/test_vfs_mount_tree.c \
		$(TESTS_DIR)/kernel/include/fs/vfs.h \
		$(TEST_MOCK)/vfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VFS_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_vfs_mount_tree.elf: $(TEST_BLD)/test_vfs_mount_tree.o \
		$(TEST_BLD)/vfs_production.o \
		$(TEST_BLD)/dcache_production.o \
		$(TEST_BLD)/ext2_production.o \
		$(TEST_BLD)/fat_production.o \
		$(TEST_BLD)/tmpfs_production.o \
		$(TEST_BLD)/devfs_vfs_production.o \
		$(TEST_BLD)/serial_sink_stub.o \
		$(TEST_BLD)/rwlock.o \
		$(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

$(TEST_BLD)/test_vfs_dcache.o: $(TEST_CASES)/test_vfs_dcache.c \
		$(TESTS_DIR)/kernel/include/fs/vfs.h \
		$(TEST_MOCK)/vfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VFS_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_vfs_dcache.elf: $(TEST_BLD)/test_vfs_dcache.o \
		$(TEST_BLD)/vfs_production.o \
		$(TEST_BLD)/dcache_production.o \
		$(TEST_BLD)/ext2_production.o \
		$(TEST_BLD)/fat_production.o \
		$(TEST_BLD)/tmpfs_production.o \
		$(TEST_BLD)/devfs_vfs_production.o \
		$(TEST_BLD)/serial_sink_stub.o \
		$(TEST_BLD)/rwlock.o \
		$(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

# ELF validate test
$(TEST_BLD)/test_elf_validate.elf: $(TEST_BLD)/test_elf_validate.o $(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -o $@ $^

# ELF layout validator
ELF_LAYOUT_HOST_CFLAGS := $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) \
                          -I$(TESTS_DIR)/kernel/include

$(TEST_BLD)/elf_layout.o: $(TESTS_DIR)/kernel/fs/elf_layout.c \
		$(TESTS_DIR)/kernel/include/fs/elf.h \
		$(TESTS_DIR)/kernel/include/fs/elf_layout.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(ELF_LAYOUT_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_elf_layout.o: $(TEST_CASES)/test_elf_layout.c \
		$(TESTS_DIR)/kernel/include/fs/elf.h \
		$(TESTS_DIR)/kernel/include/fs/elf_layout.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(ELF_LAYOUT_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_elf_layout.elf: $(TEST_BLD)/test_elf_layout.o $(TEST_BLD)/elf_layout.o
	$(HOST_CC) -o $@ $^

# Per-4 KiB ELF loader test
ELF_LOAD_HOST_CFLAGS := $(HOST_CFLAGS) $(LIBC_INC) \
                        -I$(TESTS_DIR)/kernel/include \
                        -include $(TEST_MOCK)/elf_load/elf_load_runtime.h \
                        -I$(TEST_MOCK) -I$(TEST_INC) \
                        -I$(TEST_MOCK)/elf_load

$(TEST_BLD)/elf_load_production.o: $(TESTS_DIR)/kernel/fs/elf.c \
		$(TESTS_DIR)/kernel/include/fs/elf.h \
		$(TESTS_DIR)/kernel/include/fs/elf_layout.h \
		$(TEST_MOCK)/elf_load/elf_load_stubs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(ELF_LOAD_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/elf_load_stubs.o: $(TEST_MOCK)/elf_load/elf_load_stubs.c \
		$(TEST_MOCK)/elf_load/elf_load_stubs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(ELF_LOAD_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_elf_load_4k.o: $(TEST_CASES)/test_elf_load_4k.c \
		$(TEST_MOCK)/elf_load/elf_load_stubs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(ELF_LOAD_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_elf_load_4k.elf: $(TEST_BLD)/test_elf_load_4k.o \
		$(TEST_BLD)/elf_load_production.o \
		$(TEST_BLD)/elf_load_stubs.o \
		$(TEST_BLD)/elf_layout.o \
		$(MOCK_OBJS)
	$(HOST_CC) -o $@ $^

# Canon line discipline test
$(TEST_BLD)/canon.o: $(TESTS_DIR)/kernel/tty/canon.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/kernel/include -c $< -o $@

$(TEST_BLD)/test_canon.o: $(TEST_CASES)/test_canon.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/kernel/include -c $< -o $@

$(TEST_BLD)/test_canon.elf: $(TEST_BLD)/test_canon.o $(TEST_BLD)/canon.o
	$(HOST_CC) -o $@ $^

# Poll test infrastructure
$(TEST_BLD)/poll_production.o: $(TESTS_DIR)/kernel/fs/poll.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(POLL_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/select_production.o: $(TESTS_DIR)/kernel/fs/select.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(POLL_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/file_poll_production.o: $(TESTS_DIR)/kernel/fs/file.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(POLL_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/tty_poll_production.o: $(TESTS_DIR)/kernel/tty/tty.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(POLL_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/devfs_poll_production.o: $(TESTS_DIR)/kernel/fs/devfs.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(POLL_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/poll_clocksource_stub.o: $(TEST_MOCK)/poll_clocksource_stub.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(POLL_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/serial_sink_stub.o: $(TEST_MOCK)/serial_sink_stub.c \
		$(TEST_MOCK)/test_platform.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -D__SPINLOCK_H__ -DOS01_HOST_TEST \
		-include $(TEST_MOCK)/test_platform.h \
		$(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC) -c $< -o $@

POLL_PRODUCTION_OBJS := \
    $(TEST_BLD)/poll_production.o \
    $(TEST_BLD)/select_production.o \
    $(TEST_BLD)/file_poll_production.o \
    $(TEST_BLD)/tty_poll_production.o \
    $(TEST_BLD)/devfs_poll_production.o \
    $(TEST_BLD)/poll_clocksource_stub.o \
    $(TEST_BLD)/serial_sink_stub.o \
    $(NOFILE_SOFT_STUB_OBJ)

$(TEST_BLD)/test_poll_requested.o: $(TEST_CASES)/test_poll_requested.c
	$(HOST_CC) $(POLL_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_poll_requested.elf: $(TEST_BLD)/test_poll_requested.o $(POLL_PRODUCTION_OBJS)
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

$(TEST_BLD)/test_poll_waitqueue_handshake.o: $(TEST_CASES)/test_poll_waitqueue_handshake.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(POLL_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_poll_waitqueue_handshake.elf: $(TEST_BLD)/test_poll_waitqueue_handshake.o $(POLL_PRODUCTION_OBJS)
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

# ── ARCH-10 Task 7: the poll capacity contract ───────────────
#
# test_poll_capacity.c drives the REAL production poll/select code
# and asserts the per-call capacity contract (the descriptor count is
# bounded by the task's RLIMIT_NOFILE soft limit; both buffers are
# heap-allocated; a wait chain that cannot be fully registered fails
# with -ENOMEM instead of sleeping forever).
$(TEST_BLD)/test_poll_capacity.o: $(TEST_CASES)/test_poll_capacity.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(POLL_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_poll_capacity.elf: $(TEST_BLD)/test_poll_capacity.o $(POLL_PRODUCTION_OBJS)
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

# ── ARCH-10 Task 7: the NOFILE=64 build of the SAME poll code ──
#
# The default build caps a poll() call at the compile-time descriptor
# capacity (16); a task with a raised soft limit must still be able to
# poll more descriptors.  This is a REAL SECOND BUILD of the same
# production sources with -DOS01_NOFILE_CAPACITY=64: every layout
# consumer (the production objects, the case TU and the NOFILE
# soft-limit stub) is compiled into a SEPARATE object directory, so the
# 16-layout objects are never mixed with the 64-layout ones.  The stub
# reports the capacity of ITS OWN build, so the 64 build's soft limit
# really is 64.
POLL64_BLD := $(TEST_BLD)/nofile64
POLL64_HOST_CFLAGS := $(POLL_HOST_CFLAGS) -DOS01_NOFILE_CAPACITY=64

$(POLL64_BLD)/poll_production.o: $(TESTS_DIR)/kernel/fs/poll.c
	@mkdir -p $(POLL64_BLD)
	$(HOST_CC) $(POLL64_HOST_CFLAGS) -c $< -o $@

$(POLL64_BLD)/select_production.o: $(TESTS_DIR)/kernel/fs/select.c
	@mkdir -p $(POLL64_BLD)
	$(HOST_CC) $(POLL64_HOST_CFLAGS) -c $< -o $@

$(POLL64_BLD)/file_poll_production.o: $(TESTS_DIR)/kernel/fs/file.c
	@mkdir -p $(POLL64_BLD)
	$(HOST_CC) $(POLL64_HOST_CFLAGS) -c $< -o $@

$(POLL64_BLD)/tty_poll_production.o: $(TESTS_DIR)/kernel/tty/tty.c
	@mkdir -p $(POLL64_BLD)
	$(HOST_CC) $(POLL64_HOST_CFLAGS) -c $< -o $@

$(POLL64_BLD)/devfs_poll_production.o: $(TESTS_DIR)/kernel/fs/devfs.c
	@mkdir -p $(POLL64_BLD)
	$(HOST_CC) $(POLL64_HOST_CFLAGS) -c $< -o $@

$(POLL64_BLD)/poll_clocksource_stub.o: $(TEST_MOCK)/poll_clocksource_stub.c
	@mkdir -p $(POLL64_BLD)
	$(HOST_CC) $(POLL64_HOST_CFLAGS) -c $< -o $@

$(POLL64_BLD)/serial_sink_stub.o: $(TEST_MOCK)/serial_sink_stub.c \
		$(TEST_MOCK)/test_platform.h
	@mkdir -p $(POLL64_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -D__SPINLOCK_H__ -DOS01_HOST_TEST \
		-DOS01_NOFILE_CAPACITY=64 \
		-include $(TEST_MOCK)/test_platform.h \
		$(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC) -c $< -o $@

# The soft-limit snapshot stub must report THIS build's capacity.
$(POLL64_BLD)/nofile_soft_stub.o: $(TEST_MOCK)/nofile_soft_stub.c
	@mkdir -p $(POLL64_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -DOS01_NOFILE_CAPACITY=64 $(KERNEL_INC) -c $< -o $@

POLL64_PRODUCTION_OBJS := \
    $(POLL64_BLD)/poll_production.o \
    $(POLL64_BLD)/select_production.o \
    $(POLL64_BLD)/file_poll_production.o \
    $(POLL64_BLD)/tty_poll_production.o \
    $(POLL64_BLD)/devfs_poll_production.o \
    $(POLL64_BLD)/poll_clocksource_stub.o \
    $(POLL64_BLD)/serial_sink_stub.o \
    $(POLL64_BLD)/nofile_soft_stub.o

$(POLL64_BLD)/test_poll_capacity.o: $(TEST_CASES)/test_poll_capacity.c
	@mkdir -p $(POLL64_BLD)
	$(HOST_CC) $(POLL64_HOST_CFLAGS) -c $< -o $@

$(POLL64_BLD)/test_poll_nofile64.elf: $(POLL64_BLD)/test_poll_capacity.o $(POLL64_PRODUCTION_OBJS)
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

# PTY window resize notifications
PTY_HOST_CFLAGS := $(HOST_CFLAGS) -ffunction-sections -fdata-sections \
    -Wno-unused-function -D__SPINLOCK_H__ -DOS01_HOST_TEST \
    -include $(TEST_MOCK)/pty_test_runtime.h \
    -I$(TEST_MOCK)/include $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)

$(TEST_BLD)/pty_production.o: $(TESTS_DIR)/kernel/tty/pty.c \
		$(TEST_MOCK)/pty_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PTY_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/pty_file_production.o: $(TESTS_DIR)/kernel/fs/file.c \
		$(TEST_MOCK)/pty_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PTY_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_pty_winsize.o: $(TEST_CASES)/test_pty_winsize.c \
		$(TEST_MOCK)/pty_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PTY_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_pty_winsize.elf: $(TEST_BLD)/test_pty_winsize.o \
		$(TEST_BLD)/pty_production.o \
		$(TEST_BLD)/pty_file_production.o \
		$(NOFILE_SOFT_STUB_OBJ)
	$(HOST_CC) -Wl,--gc-sections -lpthread -o $@ $^

.PHONY: test-pty-winsize
test-pty-winsize: $(TEST_BLD)/test_pty_winsize.elf
	$<

# devfs input dispatch
.PHONY: test-gui-devfs-input
test-gui-devfs-input: $(TEST_BLD)/test_devfs_input_dispatch.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

$(TEST_BLD)/test_devfs_input_dispatch.o: $(TEST_CASES)/test_devfs_input_dispatch.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(POLL_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_devfs_input_dispatch.elf: $(TEST_BLD)/test_devfs_input_dispatch.o $(POLL_PRODUCTION_OBJS)
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

$(TEST_BLD)/test_devfs_capacity.o: $(TEST_CASES)/test_devfs_capacity.c \
		$(TESTS_DIR)/kernel/include/fs/vfs.h \
		$(TESTS_DIR)/kernel/include/fs/devfs.h \
		$(TEST_MOCK)/vfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VFS_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_devfs_capacity.elf: $(TEST_BLD)/test_devfs_capacity.o \
		$(TEST_BLD)/vfs_production.o \
		$(TEST_BLD)/dcache_production.o \
		$(TEST_BLD)/ext2_production.o \
		$(TEST_BLD)/fat_production.o \
		$(TEST_BLD)/tmpfs_production.o \
		$(TEST_BLD)/devfs_vfs_production.o \
		$(TEST_BLD)/serial_sink_stub.o \
		$(TEST_BLD)/rwlock.o \
		$(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -Wl,--gc-sections -Wl,--wrap=kmalloc -o $@ $^

# ── ARCH-11: devfs sub-directory hierarchy ─────────────────────
#
# The case drives the PRODUCTION devfs tree: multi-component
# registration paths, per-level lookup and readdir, and the path
# validation rules.
$(TEST_BLD)/test_devfs_hierarchy.o: $(TEST_CASES)/test_devfs_hierarchy.c \
		$(TESTS_DIR)/kernel/include/fs/vfs.h \
		$(TESTS_DIR)/kernel/include/fs/devfs.h \
		$(TEST_MOCK)/vfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VFS_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_devfs_hierarchy.elf: $(TEST_BLD)/test_devfs_hierarchy.o \
		$(TEST_BLD)/vfs_production.o \
		$(TEST_BLD)/dcache_production.o \
		$(TEST_BLD)/ext2_production.o \
		$(TEST_BLD)/fat_production.o \
		$(TEST_BLD)/tmpfs_production.o \
		$(TEST_BLD)/devfs_vfs_production.o \
		$(TEST_BLD)/serial_sink_stub.o \
		$(TEST_BLD)/rwlock.o \
		$(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

# ── ARCH-10 Task 6: NOFILE soft limit at descriptor installation ──
#
# The case drives the PRODUCTION descriptor-installation code:
#   kernel/fs/file.c      fd_alloc_limit / fd_dup / fd_dup2 / do_pipe
#   kernel/net/socket.c   socket() / accept() install paths
#   kernel/tty/pty.c      PTMX install path
#   kernel/sync/wait.c    the pipe wait queues
#
# Those TUs use the REAL kernel headers (the production task_t and the
# production spinlock), so the mock runtime only supplies the lwIP
# surface kernel/net/socket.c needs (its real lwIP headers are
# compiled out under OS01_HOST_TEST) and the observation counters.
FD_INSTALL_HOST_CFLAGS := $(HOST_CFLAGS) -ffunction-sections -fdata-sections \
    -Wno-unused-function -DOS01_HOST_TEST \
    -include $(TEST_MOCK)/fd_install_test_runtime.h \
    $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC) -I$(TEST_MOCK) \
    -I$(TEST_MOCK)/fd_install_include -I$(TEST_MOCK)/include

$(TEST_BLD)/fd_install_file_production.o: $(TESTS_DIR)/kernel/fs/file.c \
		$(TESTS_DIR)/kernel/include/fs/file.h \
		$(TEST_MOCK)/fd_install_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FD_INSTALL_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/fd_install_socket_production.o: $(TESTS_DIR)/kernel/net/socket.c \
		$(TEST_MOCK)/fd_install_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FD_INSTALL_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/fd_install_pty_production.o: $(TESTS_DIR)/kernel/tty/pty.c \
		$(TEST_MOCK)/fd_install_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FD_INSTALL_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/fd_install_wait_production.o: $(TESTS_DIR)/kernel/sync/wait.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FD_INSTALL_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/fd_install_stubs.o: $(TEST_MOCK)/fd_install_stubs.c \
		$(TEST_MOCK)/fd_install_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FD_INSTALL_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_nofile_limits.o: $(TEST_CASES)/test_nofile_limits.c \
		$(TEST_MOCK)/fd_install_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FD_INSTALL_HOST_CFLAGS) -D'errno=(*__errno_location())' \
		-include $(TEST_INC)/gfx_client_errno.h -c $< -o $@

$(TEST_BLD)/test_nofile_limits.elf: $(TEST_BLD)/test_nofile_limits.o \
    $(TEST_BLD)/fd_install_file_production.o \
    $(TEST_BLD)/fd_install_socket_production.o \
    $(TEST_BLD)/fd_install_pty_production.o \
    $(TEST_BLD)/fd_install_wait_production.o \
    $(TEST_BLD)/fd_install_stubs.o \
    $(TEST_BLD)/sched_resource_production.o \
    $(TEST_BLD)/sys_resource_production.o \
    $(TEST_BLD)/libc_resource_production.o \
    $(TEST_BLD)/syscall_dispatch_production.o \
    $(TEST_BLD)/compat_dispatch_production.o \
    $(TEST_BLD)/compat_table_x86_64_production.o \
    $(TEST_BLD)/compat_proc_production.o \
    $(TEST_BLD)/compat_fs_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test-nofile-limits
test-nofile-limits: $(TEST_BLD)/test_nofile_limits.elf
	$<

# ── ARCH-12: the PCI device namespace in devfs ────────────────
#
# The case drives the PRODUCTION devfs tree and the PRODUCTION PCI
# core: the case TU supplies a mock arch_pci_backend() that serves a
# synthetic configuration space, and publishes a synthetic device
# through pci_devfs_register_device().
#
# pci core is compiled with the VFS runtime flags (not PCI_HOST_CFLAGS):
# pci_devfs.c pulls in fs/devfs.h -> fs/vfs.h -> arch/spinlock.h, and that
# header is only excluded by the -D__SPINLOCK_H__ that VFS_HOST_CFLAGS
# carries.  The object gets its own name so it does not collide with the
# PCI_HOST_CFLAGS build of the same source in mk/drivers.mk.
#
# pci core is compiled with -ffunction-sections (VFS_HOST_CFLAGS) and the
# binary is linked with --gc-sections: only pci_enumerate() reaches
# device_alloc_id(), and the case never enumerates, so
# kernel/device/core.c stays out of the link surface.
$(TEST_BLD)/pci_core_fs_production.o: $(TESTS_DIR)/kernel/bus/pci/core.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VFS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/pci_devfs_production.o: $(TESTS_DIR)/kernel/bus/pci/pci_devfs.c \
		$(TESTS_DIR)/kernel/include/bus/pci/pci_devfs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VFS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_pci_devfs.o: $(TEST_CASES)/test_pci_devfs.c \
		$(TESTS_DIR)/kernel/include/bus/pci/pci_devfs.h \
		$(TEST_MOCK)/vfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VFS_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_pci_devfs.elf: $(TEST_BLD)/test_pci_devfs.o \
		$(TEST_BLD)/vfs_production.o \
		$(TEST_BLD)/dcache_production.o \
		$(TEST_BLD)/ext2_production.o \
		$(TEST_BLD)/fat_production.o \
		$(TEST_BLD)/tmpfs_production.o \
		$(TEST_BLD)/devfs_vfs_production.o \
		$(TEST_BLD)/pci_core_fs_production.o \
		$(TEST_BLD)/pci_devfs_production.o \
		$(TEST_BLD)/serial_sink_stub.o \
		$(TEST_BLD)/rwlock.o \
		$(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

# ── BOOT-3 mounts & terminal host tests ───────────────────────────
$(eval $(call register_host_test,test_boot_failure_mounts,fs,protocol-or-legacy))
$(eval $(call register_host_test,test_boot_failure_terminal,fs,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_boot_failure_mounts.elf \
    $(TEST_BLD)/test_boot_failure_terminal.elf

$(TEST_BLD)/boot3_fs_boot_production.o: $(TESTS_DIR)/kernel/fs/boot.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/boot3_test_mounts.o: $(TEST_CASES)/test_boot_failure_mounts.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_boot_failure_mounts.elf: \
		$(TEST_BLD)/boot3_test_mounts.o \
		$(TEST_BLD)/boot3_fs_boot_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test_boot_failure_mounts
test_boot_failure_mounts: $(TEST_BLD)/test_boot_failure_mounts.elf
	$<

$(TEST_BLD)/boot3_tty_boot_production.o: $(TESTS_DIR)/kernel/tty/boot.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/boot3_pty_production.o: $(TESTS_DIR)/kernel/tty/pty.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PTY_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_boot_failure_terminal.o: $(TEST_CASES)/test_boot_failure_terminal.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_boot_failure_terminal.elf: \
		$(TEST_BLD)/test_boot_failure_terminal.o \
		$(TEST_BLD)/boot3_tty_boot_production.o \
		$(TEST_BLD)/boot3_pty_production.o
	$(HOST_CC) -Wl,--gc-sections -lpthread -o $@ $^

.PHONY: test_boot_failure_terminal
test_boot_failure_terminal: $(TEST_BLD)/test_boot_failure_terminal.elf
	$<

# BOOT-5 Task 1: VFS bootstrap end contract test
$(eval $(call register_host_test,test_boot_services_vfs,fs,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_boot_services_vfs.elf

$(TEST_BLD)/test_boot_services_vfs.o: $(TEST_CASES)/test_boot_services_vfs.c \
		$(TESTS_DIR)/kernel/include/fs/vfs.h \
		$(TEST_MOCK)/vfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VFS_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_boot_services_vfs.elf: \
		$(TEST_BLD)/test_boot_services_vfs.o \
		$(TEST_BLD)/vfs_production.o \
		$(TEST_BLD)/dcache_production.o \
		$(TEST_BLD)/ext2_production.o \
		$(TEST_BLD)/fat_production.o \
		$(TEST_BLD)/tmpfs_production.o \
		$(TEST_BLD)/devfs_vfs_production.o \
		$(TEST_BLD)/serial_sink_stub.o \
		$(TEST_BLD)/rwlock.o \
		$(TEST_BLD)/boot3_exec_production.o \
		$(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -Wl,--gc-sections -Wl,--wrap=vfs_lookup -lpthread -o $@ $^

.PHONY: test_boot_services_vfs
test_boot_services_vfs: $(TEST_BLD)/test_boot_services_vfs.elf
	$<
