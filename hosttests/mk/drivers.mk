# ── Drivers Subsystem Host Tests (spec §5) ───────────────────────

$(eval $(call register_host_test,test_i8042_demux,drivers,protocol-or-legacy))
$(eval $(call register_host_test,test_mouse_init,drivers,protocol-or-legacy))
$(eval $(call register_host_test,test_mouse_proto,drivers,protocol-or-legacy))
$(eval $(call register_host_test,test_block_device_ops,drivers,protocol-or-legacy))
$(eval $(call register_host_test,test_pci_driver_model,drivers,protocol-or-legacy))
$(eval $(call register_host_test,test_pci_backend_irq,drivers,protocol-or-legacy))
$(eval $(call register_host_test,test_ahci_lifecycle,drivers,protocol-or-legacy))
$(eval $(call register_host_test,test_device_boot,drivers,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_i8042_demux.elf \
    $(TEST_BLD)/test_mouse_init.elf \
    $(TEST_BLD)/test_mouse_proto.elf \
    $(TEST_BLD)/test_block_device_ops.elf \
    $(TEST_BLD)/test_pci_driver_model.elf \
    $(TEST_BLD)/test_pci_backend_irq.elf \
    $(TEST_BLD)/test_ahci_lifecycle.elf \
    $(TEST_BLD)/test_device_boot.elf

# PS/2 Mouse & Keyboard over i8042
I8042_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC) \
    -D_ARCH_IO_H -D_ARCH_SPINLOCK_H -D_ARCH_CPU_H \
    -include $(TEST_MOCK)/i8042_test_runtime.h

KBD_SHADOW_INC := -I$(TEST_MOCK)/kbd_include
KBD_HOST_CFLAGS := $(HOST_CFLAGS) $(KBD_SHADOW_INC) $(KERNEL_INC) \
    $(LIBC_INC) -I$(TEST_INC) \
    -D_ARCH_IO_H -D_ARCH_SPINLOCK_H -D_ARCH_CPU_H \
    -include $(TEST_MOCK)/i8042_test_runtime.h

$(TEST_BLD)/i8042_production.o: $(TESTS_DIR)/kernel/driver/i8042.c \
		$(TESTS_DIR)/kernel/include/driver/i8042.h \
		$(TEST_MOCK)/i8042_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(I8042_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/keyboard_production.o: $(TESTS_DIR)/kernel/driver/keyboard.c \
		$(TESTS_DIR)/kernel/include/driver/keyboard.h \
		$(TESTS_DIR)/kernel/include/driver/i8042.h \
		$(TEST_MOCK)/i8042_test_runtime.h \
		$(TEST_MOCK)/kbd_include/tty/tty.h \
		$(TEST_MOCK)/kbd_include/fs/vfs.h \
		$(TEST_MOCK)/kbd_include/fs/poll.h \
		$(TEST_MOCK)/kbd_include/intr/interrupt.h \
		$(TEST_MOCK)/kbd_include/core/debug.h \
		$(TEST_MOCK)/kbd_include/percpu/percpu.h \
		$(TEST_MOCK)/kbd_include/subsys/subsys.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(KBD_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/input_ps2_production.o: $(TESTS_DIR)/kernel/driver/input_ps2.c \
		$(TESTS_DIR)/kernel/include/driver/input_ps2.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -c $< -o $@

$(TEST_BLD)/input_route_kbd_production.o: $(TESTS_DIR)/kernel/driver/input_route.c \
		$(TESTS_DIR)/kernel/include/driver/input_route.h \
		$(TESTS_DIR)/kernel/include/driver/input_ps2.h \
		$(TEST_MOCK)/kbd_include/fs/file.h \
		$(TEST_MOCK)/kbd_include/fs/poll.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(KBD_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_i8042_demux.o: $(TEST_CASES)/test_i8042_demux.c \
		$(TESTS_DIR)/kernel/include/driver/i8042.h \
		$(TESTS_DIR)/kernel/include/driver/keyboard.h \
		$(TEST_MOCK)/i8042_test_runtime.h \
		$(TEST_MOCK)/kbd_include/tty/tty.h \
		$(TEST_MOCK)/kbd_include/fs/poll.h \
		$(TEST_MOCK)/kbd_include/percpu/percpu.h \
		$(TEST_MOCK)/kbd_include/subsys/subsys.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(KBD_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_i8042_demux.elf: $(TEST_BLD)/test_i8042_demux.o \
		$(TEST_BLD)/i8042_production.o \
		$(TEST_BLD)/keyboard_production.o \
		$(TEST_BLD)/input_route_kbd_production.o \
		$(TEST_BLD)/input_ps2_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test_i8042_demux
test_i8042_demux: $(TEST_BLD)/test_i8042_demux.elf
	$<

$(TEST_BLD)/mouse_proto_production.o: $(TESTS_DIR)/kernel/driver/mouse_proto.c $(TESTS_DIR)/kernel/include/driver/mouse_proto.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -c $< -o $@

$(TEST_BLD)/test_mouse_proto.o: $(TEST_CASES)/test_mouse_proto.c $(TESTS_DIR)/kernel/include/driver/mouse_proto.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -I$(TEST_INC) -c $< -o $@

$(TEST_BLD)/test_mouse_proto.elf: $(TEST_BLD)/test_mouse_proto.o $(TEST_BLD)/mouse_proto_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test_mouse_proto
test_mouse_proto: $(TEST_BLD)/test_mouse_proto.elf
	$<

$(TEST_BLD)/mouse_init_mouse_production.o: $(TESTS_DIR)/kernel/driver/mouse.c $(TESTS_DIR)/kernel/include/driver/mouse.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(KBD_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/mouse_init_route_production.o: $(TESTS_DIR)/kernel/driver/input_route.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(KBD_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/mouse_init_ps2_production.o: $(TESTS_DIR)/kernel/driver/input_ps2.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(KBD_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_mouse_init.o: $(TEST_CASES)/test_mouse_init.c $(TESTS_DIR)/kernel/include/driver/mouse.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(KBD_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_mouse_init.elf: $(TEST_BLD)/test_mouse_init.o \
		$(TEST_BLD)/mouse_init_mouse_production.o \
		$(TEST_BLD)/mouse_init_route_production.o \
		$(TEST_BLD)/mouse_init_ps2_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test_mouse_init
test_mouse_init: $(TEST_BLD)/test_mouse_init.elf
	$<

# ARCH-9 Phase A: Block device ops & partition wrapping
BLOCK_HOST_CFLAGS := $(HOST_CFLAGS) -I$(TEST_MOCK) $(FRAMEWORK_INC) $(LIBC_INC) $(KERNEL_INC) \
    -include $(TEST_MOCK)/arch9/block_runtime.h -DOS01_HOST_TEST

$(TEST_BLD)/blockdev_production.o: $(TESTS_DIR)/kernel/block/blockdev.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BLOCK_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_block_device_ops.o: $(TEST_CASES)/test_block_device_ops.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BLOCK_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_block_device_ops.elf: $(TEST_BLD)/test_block_device_ops.o $(TEST_BLD)/blockdev_production.o $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

.PHONY: test-block
test-block: $(TEST_BLD)/test_block_device_ops.elf
	$<

# ARCH-9 Phase B: PCI driver model & enumeration
PCI_HOST_CFLAGS := $(HOST_CFLAGS) -D_GNU_SOURCE -I$(TEST_MOCK) $(FRAMEWORK_INC) $(LIBC_INC) $(KERNEL_INC) \
    -include $(TEST_MOCK)/arch9/pci_runtime.h -DOS01_HOST_TEST

$(TEST_BLD)/device_core_production.o: $(TESTS_DIR)/kernel/device/core.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PCI_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/pci_core_production.o: $(TESTS_DIR)/kernel/bus/pci/core.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PCI_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/pci_match_production.o: $(TESTS_DIR)/kernel/bus/pci/match.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PCI_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/pci_platform_production.o: $(TESTS_DIR)/kernel/bus/pci/platform.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PCI_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_pci_driver_model.o: $(TEST_CASES)/test_pci_driver_model.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PCI_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_pci_driver_model.elf: $(TEST_BLD)/test_pci_driver_model.o \
    $(TEST_BLD)/device_core_production.o \
    $(TEST_BLD)/pci_core_production.o \
    $(TEST_BLD)/pci_match_production.o \
    $(TEST_BLD)/pci_platform_production.o \
    $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

.PHONY: test-pci
test-pci: $(TEST_BLD)/test_pci_driver_model.elf
	$<

# ARCH-9 Phase B: x86 backend, resource access and IRQ preservation
BACKEND_HOST_CFLAGS := $(HOST_CFLAGS) -D_GNU_SOURCE -I$(TEST_MOCK) $(FRAMEWORK_INC) $(LIBC_INC) $(KERNEL_INC) \
    -include $(TEST_MOCK)/arch9/backend_runtime.h -DOS01_HOST_TEST

$(TEST_BLD)/x86_pci_production.o: $(TESTS_DIR)/kernel/arch/x86_64/bus/pci.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BACKEND_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/irq_production.o: $(TESTS_DIR)/kernel/intr/irq.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BACKEND_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_pci_backend_irq.o: $(TEST_CASES)/test_pci_backend_irq.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BACKEND_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_pci_backend_irq.elf: $(TEST_BLD)/test_pci_backend_irq.o \
    $(TEST_BLD)/x86_pci_production.o \
    $(TEST_BLD)/pci_core_production.o \
    $(TEST_BLD)/pci_match_production.o \
    $(TEST_BLD)/pci_platform_production.o \
    $(TEST_BLD)/device_core_production.o \
    $(TEST_BLD)/irq_production.o \
    $(LIBC_OBJS)
	$(HOST_CC) -lpthread -o $@ $^

.PHONY: test-backend
test-backend: $(TEST_BLD)/test_pci_backend_irq.elf
	$<

# ARCH-9 Phase B: AHCI lifecycle & driver model
AHCI_HOST_CFLAGS := $(HOST_CFLAGS) -D_GNU_SOURCE -I$(TEST_MOCK) $(FRAMEWORK_INC) $(LIBC_INC) $(KERNEL_INC) \
    -include $(TEST_MOCK)/arch9/ahci_runtime.h -DOS01_HOST_TEST

$(TEST_BLD)/ahci_lifecycle_production.o: $(TESTS_DIR)/kernel/driver/ahci_lifecycle.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(AHCI_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/ahci_production.o: $(TESTS_DIR)/kernel/driver/ahci.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(AHCI_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_ahci_lifecycle.o: $(TEST_CASES)/test_ahci_lifecycle.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(AHCI_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_ahci_lifecycle.elf: $(TEST_BLD)/test_ahci_lifecycle.o \
    $(TEST_BLD)/ahci_production.o \
    $(TEST_BLD)/ahci_lifecycle_production.o \
    $(TEST_BLD)/blockdev_production.o \
    $(TEST_BLD)/device_core_production.o \
    $(TEST_BLD)/pci_core_production.o \
    $(TEST_BLD)/pci_match_production.o \
    $(TEST_BLD)/pci_platform_production.o \
    $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

.PHONY: test-ahci
test-ahci: $(TEST_BLD)/test_ahci_lifecycle.elf
	$<

# ARCH-9 Phase B: Phase 6 device coordinator and root requirement
BOOT_HOST_CFLAGS := $(HOST_CFLAGS) -D_GNU_SOURCE -I$(TEST_MOCK) $(FRAMEWORK_INC) $(LIBC_INC) $(KERNEL_INC) \
    -include $(TEST_MOCK)/arch9/boot_runtime.h -DOS01_HOST_TEST

$(TEST_BLD)/device_boot_production.o: $(TESTS_DIR)/kernel/device/boot.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/fs_boot_production.o: $(TESTS_DIR)/kernel/fs/boot.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_device_boot.o: $(TEST_CASES)/test_device_boot.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_device_boot.elf: $(TEST_BLD)/test_device_boot.o \
    $(TEST_BLD)/device_boot_production.o \
    $(TEST_BLD)/fs_boot_production.o \
    $(TEST_BLD)/device_core_production.o \
    $(TEST_BLD)/blockdev_production.o \
    $(TEST_BLD)/pci_core_production.o \
    $(TEST_BLD)/pci_match_production.o \
    $(TEST_BLD)/pci_platform_production.o \
    $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

.PHONY: test-device-boot
test-device-boot: $(TEST_BLD)/test_device_boot.elf
	$<
