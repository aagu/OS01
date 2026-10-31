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
$(eval $(call register_host_test,test_pty_winsize,fs,protocol-or-legacy))
$(eval $(call register_host_test,test_devfs_input_dispatch,fs,protocol-or-legacy))

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
    $(TEST_BLD)/test_elf_layout.elf \
    $(TEST_BLD)/test_elf_load_4k.elf \
    $(TEST_BLD)/test_pty_winsize.elf \
    $(TEST_BLD)/test_devfs_input_dispatch.elf

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
    $(TEST_BLD)/serial_sink_stub.o

$(TEST_BLD)/test_poll_requested.o: $(TEST_CASES)/test_poll_requested.c
	$(HOST_CC) $(POLL_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_poll_requested.elf: $(TEST_BLD)/test_poll_requested.o $(POLL_PRODUCTION_OBJS)
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

$(TEST_BLD)/test_poll_waitqueue_handshake.o: $(TEST_CASES)/test_poll_waitqueue_handshake.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(POLL_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_poll_waitqueue_handshake.elf: $(TEST_BLD)/test_poll_waitqueue_handshake.o $(POLL_PRODUCTION_OBJS)
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
		$(TEST_BLD)/pty_file_production.o
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
