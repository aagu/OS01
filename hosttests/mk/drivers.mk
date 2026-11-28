# ── Drivers Subsystem Host Tests (spec §5) ───────────────────────



# PS/2 Mouse & Keyboard over i8042
I8042_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC) \
    -D_ARCH_IO_H -D_ARCH_SPINLOCK_H -D_ARCH_CPU_H \
    -include $(TEST_MOCK)/i8042_test_runtime.h

KBD_SHADOW_INC := -I$(TEST_MOCK)/kbd_include
KBD_HOST_CFLAGS := $(HOST_CFLAGS) $(KBD_SHADOW_INC) $(KERNEL_INC) \
    $(LIBC_INC) -I$(TEST_INC) \
    -DOS01_HOST_TEST -D_ARCH_IO_H -D_ARCH_SPINLOCK_H -D_ARCH_CPU_H \
    -include $(TEST_MOCK)/i8042_test_runtime.h

$(TEST_BLD)/i8042_production.o: $(TESTS_DIR)/kernel/arch/x86_64/driver/i8042.c \
		$(TESTS_DIR)/kernel/include/driver/i8042.h \
		$(TEST_MOCK)/i8042_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(I8042_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/input_arbitrate_production.o: $(TESTS_DIR)/kernel/arch/x86_64/driver/input_arbitrate.c \
		$(TESTS_DIR)/kernel/include/driver/keyboard.h \
		$(TESTS_DIR)/kernel/include/driver/mouse.h \
		$(TESTS_DIR)/kernel/include/driver/xhci_hid.h \
		$(TEST_MOCK)/kbd_include/subsys/subsys.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(KBD_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/ps2_kbd_production.o: $(TESTS_DIR)/kernel/arch/x86_64/driver/ps2_kbd.c \
		$(TESTS_DIR)/kernel/include/driver/keyboard.h \
		$(TESTS_DIR)/kernel/include/driver/i8042.h \
		$(TEST_MOCK)/i8042_test_runtime.h \
		$(TEST_MOCK)/kbd_include/intr/interrupt.h \
		$(TEST_MOCK)/kbd_include/core/debug.h \
		$(TEST_MOCK)/kbd_include/subsys/subsys.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(KBD_HOST_CFLAGS) -c $< -o $@

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
		$(TEST_BLD)/input_arbitrate_production.o \
		$(TEST_BLD)/ps2_kbd_production.o \
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

$(TEST_BLD)/ps2_mouse_production.o: $(TESTS_DIR)/kernel/arch/x86_64/driver/ps2_mouse.c $(TESTS_DIR)/kernel/include/driver/mouse.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(KBD_HOST_CFLAGS) -c $< -o $@

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
		$(TEST_BLD)/ps2_mouse_production.o \
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

$(TEST_BLD)/blockdev_production.o: $(TESTS_DIR)/kernel/block/blockdev.c \
		$(TESTS_DIR)/kernel/include/block/blockdev.h \
		$(TEST_MOCK)/arch9/block_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BLOCK_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_block_device_ops.o: $(TEST_CASES)/test_block_device_ops.c \
		$(TESTS_DIR)/kernel/include/block/blockdev.h \
		$(TEST_MOCK)/arch9/block_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BLOCK_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_block_device_ops.elf: $(TEST_BLD)/test_block_device_ops.o $(TEST_BLD)/blockdev_production.o $(TEST_BLD)/bcache_production.o $(TEST_BLD)/bcache_wait.o $(TEST_BLD)/arch6_bcache_runtime.o $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

.PHONY: test-block
test-block: $(TEST_BLD)/test_block_device_ops.elf
	$<

# ARCH-6: block cache read core + deterministic wait harness.  The bcache
# objects and the production wait queue are compiled with the arch6 runtime
# (same spinlock_T/task_t layout across objects); the elf wraps
# wait_queue_arm/sleep so cases can inject completion windows.
# Note: no FRAMEWORK_INC here — test_platform.h would conflict with the
# arch6 runtime's spinlock_T/task_t substrate (mutex_runtime.h pattern).
BCACHE_HOST_CFLAGS := $(HOST_CFLAGS) -I$(TEST_MOCK) -I$(TEST_INC) $(LIBC_INC) $(KERNEL_INC) \
    -include $(TEST_MOCK)/arch6_bcache_runtime.h -DOS01_HOST_TEST

$(TEST_BLD)/bcache_production.o: $(TESTS_DIR)/kernel/block/bcache.c \
		$(TESTS_DIR)/kernel/include/block/bcache.h \
		$(TESTS_DIR)/kernel/include/block/blockdev.h \
		$(TEST_MOCK)/arch6_bcache_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BCACHE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/bcache_wait.o: $(TESTS_DIR)/kernel/sync/wait.c \
		$(TEST_MOCK)/arch6_bcache_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BCACHE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/arch6_bcache_runtime.o: $(TEST_MOCK)/arch6_bcache_runtime.c \
		$(TEST_MOCK)/arch6_bcache_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_MOCK) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_bcache.o: $(TEST_CASES)/test_bcache.c \
		$(TESTS_DIR)/kernel/include/block/bcache.h \
		$(TESTS_DIR)/kernel/include/block/blockdev.h \
		$(TEST_MOCK)/arch6_bcache_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BCACHE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_bcache.elf: $(TEST_BLD)/test_bcache.o \
		$(TEST_BLD)/bcache_production.o \
		$(TEST_BLD)/bcache_wait.o \
		$(TEST_BLD)/arch6_bcache_runtime.o \
		$(TEST_BLD)/blockdev_production.o \
		$(LIBC_OBJS)
	$(HOST_CC) -Wl,--wrap=wait_queue_arm -Wl,--wrap=wait_queue_sleep -o $@ $^

.PHONY: test-bcache
test-bcache: $(TEST_BLD)/test_bcache.elf
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

# AArch64 Phase-1: 4 KiB ECAM config space, BAR & ranges translation
LINK_CFLAGS_pci-extended-config = $(PCI_HOST_CFLAGS)
LINK_GROUP_pci-extended-config := \
    $(TEST_BLD)/device_core_production.o \
    $(TEST_BLD)/pci_core_production.o \
    $(TEST_BLD)/pci_match_production.o \
    $(TEST_BLD)/pci_platform_production.o \
    $(LIBC_OBJS)

.PHONY: test-pci-extended-config
test-pci-extended-config: $(TEST_BLD)/test_pci_extended_config.elf
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
    $(TEST_BLD)/bcache_production.o \
    $(TEST_BLD)/bcache_wait.o \
    $(TEST_BLD)/arch6_bcache_runtime.o \
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
    $(TEST_BLD)/bcache_production.o \
    $(TEST_BLD)/bcache_wait.o \
    $(TEST_BLD)/arch6_bcache_runtime.o \
    $(TEST_BLD)/pci_core_production.o \
    $(TEST_BLD)/pci_match_production.o \
    $(TEST_BLD)/pci_platform_production.o \
    $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

.PHONY: test-device-boot
test-device-boot: $(TEST_BLD)/test_device_boot.elf
	$<

# xHCI Phase 1: Controller Skeleton & Ring Init
XHCI_HOST_CFLAGS := $(HOST_CFLAGS) -D_GNU_SOURCE -I$(TEST_MOCK) $(FRAMEWORK_INC) $(LIBC_INC) $(KERNEL_INC) \
    -include $(TEST_MOCK)/arch9/xhci_runtime.h -DOS01_HOST_TEST

$(TEST_BLD)/xhci_production.o: $(TESTS_DIR)/kernel/driver/xhci.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(XHCI_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/xhci_pci_production.o: $(TESTS_DIR)/kernel/driver/xhci_pci.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(XHCI_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/xhci_hub_production.o: $(TESTS_DIR)/kernel/driver/xhci_hub.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(XHCI_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/xhci_hid_desc_production.o: $(TESTS_DIR)/kernel/driver/xhci_hid_desc.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(XHCI_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_xhci_init.o: $(TEST_CASES)/test_xhci_init.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(XHCI_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_xhci_init.elf: $(TEST_BLD)/test_xhci_init.o \
    $(TEST_BLD)/xhci_production.o \
    $(TEST_BLD)/xhci_dma_production.o \
    $(TEST_BLD)/xhci_pci_production.o \
    $(TEST_BLD)/xhci_hub_production.o \
    $(TEST_BLD)/xhci_hid_desc_production.o \
    $(TEST_BLD)/device_core_production.o \
    $(TEST_BLD)/pci_core_production.o \
    $(TEST_BLD)/pci_match_production.o \
    $(TEST_BLD)/pci_platform_production.o \
    $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

.PHONY: test-xhci
test-xhci: $(TEST_BLD)/test_xhci_init.elf
	$<

$(TEST_BLD)/test_xhci_phase2.o: $(TEST_CASES)/test_xhci_phase2.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(XHCI_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_xhci_phase2.elf: $(TEST_BLD)/test_xhci_phase2.o \
    $(TEST_BLD)/xhci_production.o \
    $(TEST_BLD)/xhci_dma_production.o \
    $(TEST_BLD)/xhci_pci_production.o \
    $(TEST_BLD)/xhci_hub_production.o \
    $(TEST_BLD)/xhci_hid_desc_production.o \
    $(TEST_BLD)/device_core_production.o \
    $(TEST_BLD)/pci_core_production.o \
    $(TEST_BLD)/pci_match_production.o \
    $(TEST_BLD)/pci_platform_production.o \
    $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

.PHONY: test-xhci-phase2
test-xhci-phase2: $(TEST_BLD)/test_xhci_phase2.elf
	$<

$(TEST_BLD)/xhci_msc_production.o: $(TESTS_DIR)/kernel/driver/xhci_msc.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(XHCI_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_xhci_phase3.o: $(TEST_CASES)/test_xhci_phase3.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(XHCI_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_xhci_phase3.elf: $(TEST_BLD)/test_xhci_phase3.o \
    $(TEST_BLD)/xhci_production.o \
    $(TEST_BLD)/xhci_dma_production.o \
    $(TEST_BLD)/xhci_pci_production.o \
    $(TEST_BLD)/xhci_hub_production.o \
    $(TEST_BLD)/xhci_hid_desc_production.o \
    $(TEST_BLD)/xhci_msc_production.o \
    $(TEST_BLD)/device_core_production.o \
    $(TEST_BLD)/pci_core_production.o \
    $(TEST_BLD)/pci_match_production.o \
    $(TEST_BLD)/pci_platform_production.o \
    $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

.PHONY: test-xhci-phase3
test-xhci-phase3: $(TEST_BLD)/test_xhci_phase3.elf
	$<

# Phase 4: USB devfs & lsusb
USB_DEVFS_HOST_CFLAGS := $(XHCI_HOST_CFLAGS) -D__SPINLOCK_H__

$(TEST_BLD)/test_usb_devfs.o: $(TEST_CASES)/test_usb_devfs.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(USB_DEVFS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/usb_devfs_production.o: $(TESTS_DIR)/kernel/driver/usb_devfs.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(USB_DEVFS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/lsusb_production.o: $(TESTS_DIR)/user/lsusb.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(USB_DEVFS_HOST_CFLAGS) -DLSUSB_NO_MAIN -c $< -o $@

$(TEST_BLD)/test_usb_devfs.elf: $(TEST_BLD)/test_usb_devfs.o \
    $(TEST_BLD)/usb_devfs_production.o \
    $(TEST_BLD)/lsusb_production.o \
    $(TEST_BLD)/xhci_production.o \
    $(TEST_BLD)/xhci_dma_production.o \
    $(TEST_BLD)/xhci_pci_production.o \
    $(TEST_BLD)/xhci_hub_production.o \
    $(TEST_BLD)/xhci_hid_desc_production.o \
    $(TEST_BLD)/device_core_production.o \
    $(TEST_BLD)/pci_core_production.o \
    $(TEST_BLD)/pci_match_production.o \
    $(TEST_BLD)/pci_platform_production.o \
    $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

.PHONY: test-usb-devfs
test-usb-devfs: $(TEST_BLD)/test_usb_devfs.elf
	$<

# USB Mass Storage (BOT + SCSI + read-ahead)
USB_MSC_HOST_CFLAGS := $(HOST_CFLAGS) -D_GNU_SOURCE -I$(TEST_MOCK) $(FRAMEWORK_INC) $(LIBC_INC) $(KERNEL_INC) \
    -include $(TEST_MOCK)/arch9/usb_msc_mutex.h \
    -include $(TEST_MOCK)/arch9/xhci_runtime.h -DOS01_HOST_TEST

$(TEST_BLD)/usb_msc_bot_production.o: $(TESTS_DIR)/kernel/driver/usb_msc_bot.c \
    $(TESTS_DIR)/kernel/include/driver/usb_msc_bot.h \
    $(TESTS_DIR)/kernel/include/driver/usb_msc.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(USB_MSC_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/usb_msc_production.o: $(TESTS_DIR)/kernel/driver/usb_msc.c \
    $(TESTS_DIR)/kernel/include/driver/usb_msc.h \
    $(TESTS_DIR)/kernel/include/driver/usb_msc_bot.h \
    $(TESTS_DIR)/kernel/include/block/blockdev.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(USB_MSC_HOST_CFLAGS) -c $< -o $@

LINK_CFLAGS_usb-msc = $(HOST_CFLAGS) -D_GNU_SOURCE -I$(TEST_MOCK) $(FRAMEWORK_INC) $(LIBC_INC) $(KERNEL_INC) \
    -include $(TEST_MOCK)/arch9/usb_msc_mutex.h -DOS01_HOST_TEST
LINK_LDFLAGS_usb-msc = -lpthread
LINK_GROUP_usb-msc := \
    $(TEST_BLD)/usb_msc_bot_production.o \
    $(TEST_BLD)/usb_msc_production.o \
    $(TEST_BLD)/blockdev_production.o \
    $(TEST_BLD)/bcache_production.o \
    $(TEST_BLD)/bcache_wait.o \
    $(TEST_BLD)/arch6_bcache_runtime.o \
    $(LIBC_OBJS)

.PHONY: test-usb-msc
test-usb-msc: $(TEST_BLD)/test_usb_msc.elf
	$<

# UEFI boot device path parser
$(TEST_BLD)/uefi_boot_device.o: $(TESTS_DIR)/boot/uefi/boot_device.c \
    $(TESTS_DIR)/boot/uefi/boot_device.h \
    $(TESTS_DIR)/kernel/include/core/bootinfo.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -c $< -o $@

LINK_CFLAGS_boot-devpath = $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) $(KERNEL_INC)
LINK_GROUP_boot-devpath := $(TEST_BLD)/uefi_boot_device.o

.PHONY: test-boot-devpath
test-boot-devpath: $(TEST_BLD)/test_boot_devpath.elf
	$<

# USB Boot HID report decoder
$(TEST_BLD)/usb_hid_report_production.o: $(TESTS_DIR)/kernel/driver/usb_hid_report.c \
    $(TESTS_DIR)/kernel/include/driver/usb_hid_report.h \
    $(TESTS_DIR)/kernel/include/driver/input_ps2.h \
    $(TESTS_DIR)/kernel/include/uapi/mouse.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -c $< -o $@

LINK_CFLAGS_usb-hid-report = $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) $(KERNEL_INC)
LINK_GROUP_usb-hid-report := $(TEST_BLD)/usb_hid_report_production.o

.PHONY: test-usb-hid-report test_usb_hid_report
test-usb-hid-report test_usb_hid_report: $(TEST_BLD)/test_usb_hid_report.elf
	$<

# xHCI Boot HID config descriptor scanner & endpoint selection
LINK_CFLAGS_xhci-hid-desc = $(XHCI_HOST_CFLAGS)
LINK_GROUP_xhci-hid-desc := \
    $(TEST_BLD)/xhci_production.o \
    $(TEST_BLD)/xhci_dma_production.o \
    $(TEST_BLD)/xhci_pci_production.o \
    $(TEST_BLD)/xhci_hub_production.o \
    $(TEST_BLD)/xhci_hid_desc_production.o \
    $(TEST_BLD)/device_core_production.o \
    $(TEST_BLD)/pci_core_production.o \
    $(TEST_BLD)/pci_match_production.o \
    $(TEST_BLD)/pci_platform_production.o \
    $(LIBC_OBJS)

.PHONY: test-xhci-hid-desc test_xhci_hid_desc
test-xhci-hid-desc test_xhci_hid_desc: $(TEST_BLD)/test_xhci_hid_desc.elf
	$<

# xHCI shared completion events & asynchronous Interrupt IN
LINK_CFLAGS_xhci-intr-in = $(XHCI_HOST_CFLAGS)
LINK_GROUP_xhci-intr-in := \
    $(TEST_BLD)/xhci_production.o \
    $(TEST_BLD)/xhci_dma_production.o \
    $(TEST_BLD)/xhci_pci_production.o \
    $(TEST_BLD)/xhci_hub_production.o \
    $(TEST_BLD)/xhci_hid_desc_production.o \
    $(TEST_BLD)/device_core_production.o \
    $(TEST_BLD)/pci_core_production.o \
    $(TEST_BLD)/pci_match_production.o \
    $(TEST_BLD)/pci_platform_production.o \
    $(LIBC_OBJS)

.PHONY: test-xhci-intr-in test_xhci_intr_in
test-xhci-intr-in test_xhci_intr_in: $(TEST_BLD)/test_xhci_intr_in.elf
	$<

# xHCI Boot HID runtime service (binding, monitor, disconnect, teardown)
$(TEST_BLD)/xhci_hid_production.o: $(TESTS_DIR)/kernel/arch/x86_64/driver/xhci_hid.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(XHCI_HOST_CFLAGS) -c $< -o $@

LINK_CFLAGS_xhci-hid-service = $(XHCI_HOST_CFLAGS)
LINK_GROUP_xhci-hid-service := \
    $(TEST_BLD)/xhci_production.o \
    $(TEST_BLD)/xhci_dma_production.o \
    $(TEST_BLD)/xhci_pci_production.o \
    $(TEST_BLD)/xhci_hub_production.o \
    $(TEST_BLD)/xhci_hid_desc_production.o \
    $(TEST_BLD)/xhci_hid_production.o \
    $(TEST_BLD)/usb_hid_report_production.o \
    $(TEST_BLD)/device_core_production.o \
    $(TEST_BLD)/pci_core_production.o \
    $(TEST_BLD)/pci_match_production.o \
    $(TEST_BLD)/pci_platform_production.o \
    $(LIBC_OBJS)

.PHONY: test-xhci-hid-service test_xhci_hid_service
test-xhci-hid-service test_xhci_hid_service: $(TEST_BLD)/test_xhci_hid_service.elf
	$<

# xHCI DMA ops, non-identity bus addressing, cache sync ordering & aarch64 PCI probe
$(TEST_BLD)/xhci_dma_production.o: $(TESTS_DIR)/kernel/driver/xhci_dma.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(XHCI_HOST_CFLAGS) -c $< -o $@

LINK_CFLAGS_xhci-dma = $(XHCI_HOST_CFLAGS)
LINK_GROUP_xhci-dma := \
    $(TEST_BLD)/xhci_dma_production.o \
    $(TEST_BLD)/xhci_production.o \
    $(TEST_BLD)/xhci_pci_production.o \
    $(TEST_BLD)/xhci_hub_production.o \
    $(TEST_BLD)/xhci_hid_desc_production.o \
    $(TEST_BLD)/xhci_msc_production.o \
    $(TEST_BLD)/device_core_production.o \
    $(TEST_BLD)/pci_core_production.o \
    $(TEST_BLD)/pci_match_production.o \
    $(TEST_BLD)/pci_platform_production.o

$(TEST_BLD)/dtb_parse_rp1_production.o: $(TESTS_DIR)/kernel/arch/aarch64/platform/dtb_parse_rp1.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(XHCI_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/rp1_pcie_production.o: $(TESTS_DIR)/kernel/arch/aarch64/bus/rp1_pcie.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(XHCI_HOST_CFLAGS) -c $< -o $@

LINK_CFLAGS_rp1-xhci = $(XHCI_HOST_CFLAGS)
LINK_GROUP_rp1-xhci := \
    $(TEST_BLD)/dtb_parse_rp1_production.o \
    $(TEST_BLD)/rp1_pcie_production.o \
    $(LIBC_OBJS)

.PHONY: test-aarch64-rp1-xhci
test-aarch64-rp1-xhci: $(TEST_BLD)/test_aarch64_rp1_xhci.elf
	$<

$(TEST_BLD)/rp1_pcie_hw_production.o: \
		$(TESTS_DIR)/kernel/arch/aarch64/bus/rp1_pcie_hw.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/rp1_pcie.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/rp1_xhci.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/boot_mmio.h \
		$(TESTS_DIR)/kernel/include/core/bootinfo.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(XHCI_HOST_CFLAGS) -c $< -o $@

LINK_CFLAGS_rp1-topology = $(XHCI_HOST_CFLAGS)
LINK_GROUP_rp1-topology := \
    $(TEST_BLD)/dtb_parse_rp1_production.o \
    $(TEST_BLD)/rp1_pcie_production.o \
    $(TEST_BLD)/rp1_pcie_hw_production.o \
    $(LIBC_OBJS)

.PHONY: test-aarch64-rp1-topology
test-aarch64-rp1-topology: $(TEST_BLD)/test_aarch64_rp1_topology.elf
	$<





# AArch64 UEFI kernel-ELF validator — the production boot/uefi elf.c
# compiled twice (default 0x40000000 layout, and the 0x42000000
# relocated layout with compile-time renamed exports), linked side by
# side into one binary (LMA Task 2).
$(TEST_BLD)/aarch64_boot_elf_default.o: $(TESTS_DIR)/boot/uefi/arch/aarch64/elf.c \
    $(TESTS_DIR)/boot/uefi/arch/aarch64/loader.h \
    $(TESTS_DIR)/kernel/include/arch/aarch64/handoff_layout.h \
    $(TESTS_DIR)/kernel/include/arch/aarch64/boot_layout.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -c $< -o $@

$(TEST_BLD)/aarch64_boot_elf_alt.o: $(TESTS_DIR)/boot/uefi/arch/aarch64/elf.c \
    $(TESTS_DIR)/boot/uefi/arch/aarch64/loader.h \
    $(TESTS_DIR)/kernel/include/arch/aarch64/handoff_layout.h \
    $(TESTS_DIR)/kernel/include/arch/aarch64/boot_layout.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) \
	  -DAARCH64_BOOT_BLOCK_BASE=0x42000000 \
	  -Daarch64_elf_validate=aarch64_elf_validate_alt \
	  -Daarch64_page_interval=aarch64_page_interval_alt -c $< -o $@

LINK_CFLAGS_aarch64-boot-elf = $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) $(KERNEL_INC)
LINK_GROUP_aarch64-boot-elf := \
    $(TEST_BLD)/aarch64_boot_elf_default.o \
    $(TEST_BLD)/aarch64_boot_elf_alt.o

# AArch64 loader early-MMIO FDT walker (roadmap P2 hardcode item:
# head.S Device block slots come from the platform via boot_context).
$(TEST_BLD)/aarch64_fdt_early_production.o: \
		$(TESTS_DIR)/boot/uefi/arch/aarch64/fdt_early.c \
		$(TESTS_DIR)/boot/uefi/arch/aarch64/loader.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/handoff_layout.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/boot_layout.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -c $< -o $@

LINK_CFLAGS_aarch64-fdt-early = $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) $(KERNEL_INC)
LINK_GROUP_aarch64-fdt-early := \
    $(TEST_BLD)/aarch64_fdt_early_production.o

# UEFI ESP /config.txt parser (no firmware dependencies)
$(TEST_BLD)/uefi_boot_config.o: $(TESTS_DIR)/boot/uefi/boot_config.c \
		$(TESTS_DIR)/boot/uefi/boot_config.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

LINK_CFLAGS_uefi-boot-config = $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC)
LINK_GROUP_uefi-boot-config := $(TEST_BLD)/uefi_boot_config.o

# AArch64 UEFI boot memory & DTB window safety checks
$(TEST_BLD)/aarch64_uefi_boot_memory.o: \
		$(TESTS_DIR)/boot/uefi/arch/aarch64/boot_memory.c \
		$(TESTS_DIR)/boot/uefi/arch/aarch64/loader.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/uefi_memory_map.h \
		$(TESTS_DIR)/kernel/include/core/bootinfo.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -c $< -o $@

LINK_CFLAGS_aarch64-uefi-boot-memory = $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) $(KERNEL_INC)
LINK_GROUP_aarch64-uefi-boot-memory := $(TEST_BLD)/aarch64_uefi_boot_memory.o

# AArch64 ASLR (kernel/arch/aarch64/memory/aslr.c)
$(TEST_BLD)/aarch64_aslr.o: $(TESTS_DIR)/kernel/arch/aarch64/memory/aslr.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/aslr.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/user_image.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/mmu.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -include $(TESTS_DIR)/hosttests/mock/test_platform.h -D__KERNEL_ARCH_CPU_H__ -D__SPINLOCK_H__ -I$(TESTS_DIR)/hosttests/mock $(LIBC_INC) $(KERNEL_INC) -c $< -o $@

LINK_CFLAGS_aarch64-aslr = $(HOST_CFLAGS) $(FRAMEWORK_INC) -D__KERNEL_ARCH_CPU_H__ -D__SPINLOCK_H__ -I$(TESTS_DIR)/hosttests/mock $(LIBC_INC) $(KERNEL_INC)
LINK_GROUP_aarch64-aslr := $(TEST_BLD)/aarch64_aslr.o

.PHONY: test-aarch64-boot-elf
test-aarch64-boot-elf: $(TEST_BLD)/test_aarch64_boot_elf.elf
	$<

# AArch64 UEFI RP1 PCI capture
$(TEST_BLD)/rp1_uefi_pci.o: \
		$(TESTS_DIR)/boot/uefi/arch/aarch64/rp1_pci.c \
		$(TESTS_DIR)/boot/uefi/arch/aarch64/rp1_pci.h \
		$(TESTS_DIR)/kernel/include/core/bootinfo.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -DOS01_HOST_TEST $(KERNEL_INC) -c $< -o $@

LINK_CFLAGS_rp1-uefi-pci = $(HOST_CFLAGS) -DOS01_HOST_TEST $(FRAMEWORK_INC) $(LIBC_INC) $(KERNEL_INC)
LINK_GROUP_rp1-uefi-pci := $(TEST_BLD)/rp1_uefi_pci.o

.PHONY: test-aarch64-uefi-rp1-pci
test-aarch64-uefi-rp1-pci: $(TEST_BLD)/test_aarch64_uefi_rp1_pci.elf
	$<
