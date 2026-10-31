# ── Net Subsystem Host Tests (spec §5) ───────────────────────────

$(eval $(call register_host_test,test_lwip_rand,net,protocol-or-legacy))
$(eval $(call register_host_test,test_net_device,net,protocol-or-legacy))
$(eval $(call register_host_test,test_net_lwip,net,protocol-or-legacy))
$(eval $(call register_host_test,test_e1000_instances,net,protocol-or-legacy))
$(eval $(call register_host_test,test_virtio_net_instances,net,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_lwip_rand.elf \
    $(TEST_BLD)/test_net_device.elf \
    $(TEST_BLD)/test_net_lwip.elf \
    $(TEST_BLD)/test_e1000_instances.elf \
    $(TEST_BLD)/test_virtio_net_instances.elf

# LWIP_RAND white-box fixture
LWIP_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC)

$(TEST_BLD)/lwip_rand_production.o: $(TESTS_DIR)/kernel/net/lwip_sys_arch.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(LWIP_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_lwip_rand.o: $(TEST_CASES)/test_lwip_rand.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_INC) $(KERNEL_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_lwip_rand.elf: $(TEST_BLD)/test_lwip_rand.o $(TEST_BLD)/lwip_rand_production.o
	$(HOST_CC) -Wl,--wrap=get_random_bytes -o $@ $^

.PHONY: test_lwip_rand
test_lwip_rand: $(TEST_BLD)/test_lwip_rand.elf
	$<

# ARCH-9 Phase C: net_device registry and lwIP adapter
NET_HOST_CFLAGS := $(HOST_CFLAGS) -D_GNU_SOURCE -I$(TEST_MOCK) $(FRAMEWORK_INC) $(LIBC_INC) $(KERNEL_INC) \
    -include $(TEST_MOCK)/arch9/net_runtime.h -DOS01_HOST_TEST

$(TEST_BLD)/net_device_production.o: $(TESTS_DIR)/kernel/net/device.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(NET_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/net_lwip_production.o: $(TESTS_DIR)/kernel/net/lwip.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(NET_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_net_device.o: $(TEST_CASES)/test_net_device.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(NET_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_net_device.elf: $(TEST_BLD)/test_net_device.o \
    $(TEST_BLD)/net_device_production.o \
    $(TEST_BLD)/net_lwip_production.o \
    $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

.PHONY: test-net
test-net: $(TEST_BLD)/test_net_device.elf
	$<

$(TEST_BLD)/test_net_lwip.o: $(TEST_CASES)/test_net_lwip.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(NET_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_net_lwip.elf: $(TEST_BLD)/test_net_lwip.o \
    $(TEST_BLD)/net_device_production.o \
    $(TEST_BLD)/net_lwip_production.o \
    $(TEST_BLD)/pci_core_production.o \
    $(TEST_BLD)/pci_match_production.o \
    $(TEST_BLD)/pci_platform_production.o \
    $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

.PHONY: test-lwip
test-lwip: $(TEST_BLD)/test_net_lwip.elf
	$<

# ARCH-9 Phase C Task 10: socket readiness and default-iface queries
SOCKET_HOST_CFLAGS := $(POLL_HOST_CFLAGS) -I$(TEST_MOCK) $(KERNEL_INC) $(LIBC_INC) \
    -include $(TEST_MOCK)/arch9/socket_runtime.h -DOS01_HOST_TEST

$(TEST_BLD)/net_socket_production.o: $(TESTS_DIR)/kernel/net/socket.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SOCKET_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_socket_net_readiness.o: $(TEST_CASES)/test_socket_net_readiness.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SOCKET_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_socket_net_readiness.elf: $(TEST_BLD)/test_socket_net_readiness.o \
    $(TEST_BLD)/net_socket_production.o \
    $(TEST_BLD)/net_lwip_production.o \
    $(TEST_BLD)/net_device_production.o \
    $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

.PHONY: test-socket
test-socket: $(TEST_BLD)/test_socket_net_readiness.elf
	$<

# ARCH-9 Phase C: e1000 private instance and IRQ modes
E1000_HOST_CFLAGS := $(HOST_CFLAGS) -D_GNU_SOURCE -I$(TEST_MOCK) $(FRAMEWORK_INC) $(LIBC_INC) $(KERNEL_INC) \
    -include $(TEST_MOCK)/arch9/e1000_runtime.h -DOS01_HOST_TEST

$(TEST_BLD)/e1000_production.o: $(TESTS_DIR)/kernel/driver/e1000.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(E1000_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_e1000_instances.o: $(TEST_CASES)/test_e1000_instances.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(E1000_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_e1000_instances.elf: $(TEST_BLD)/test_e1000_instances.o \
    $(TEST_BLD)/e1000_production.o \
    $(TEST_BLD)/net_device_production.o \
    $(TEST_BLD)/device_core_production.o \
    $(TEST_BLD)/pci_core_production.o \
    $(TEST_BLD)/pci_match_production.o \
    $(TEST_BLD)/pci_platform_production.o \
    $(TEST_BLD)/irq_production.o \
    $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

.PHONY: test-e1000
test-e1000: $(TEST_BLD)/test_e1000_instances.elf
	$<

# ARCH-9 Phase C: virtio-net private instance and polling mode
VIRTIO_HOST_CFLAGS := $(HOST_CFLAGS) -D_GNU_SOURCE -I$(TEST_MOCK) $(FRAMEWORK_INC) $(LIBC_INC) $(KERNEL_INC) \
    -include $(TEST_MOCK)/arch9/virtio_runtime.h -DOS01_HOST_TEST

$(TEST_BLD)/virtio_production.o: $(TESTS_DIR)/kernel/driver/virtio-net.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VIRTIO_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_virtio_net_instances.o: $(TEST_CASES)/test_virtio_net_instances.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VIRTIO_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_virtio_net_instances.elf: $(TEST_BLD)/test_virtio_net_instances.o \
    $(TEST_BLD)/virtio_production.o \
    $(TEST_BLD)/net_device_production.o \
    $(TEST_BLD)/device_core_production.o \
    $(TEST_BLD)/pci_core_production.o \
    $(TEST_BLD)/pci_match_production.o \
    $(TEST_BLD)/pci_platform_production.o \
    $(TEST_BLD)/irq_production.o \
    $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

.PHONY: test-virtio
test-virtio: $(TEST_BLD)/test_virtio_net_instances.elf
	$<
