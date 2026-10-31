# ── Sched Subsystem Host Tests (spec §5) ────────────────────────

$(eval $(call register_host_test,test_syscall_dispatch,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_compat_linux,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_wait_basic,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_sync_primitives,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_mutex_handshake,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_ap_work_protocol,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_double_state_publish,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_clocksource,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_process_image_lifecycle,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_fork_user_map,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_percpu_layout,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_smp_starting_gate,sched,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_syscall_dispatch.elf \
    $(TEST_BLD)/test_compat_linux.elf \
    $(TEST_BLD)/test_wait_basic.elf \
    $(TEST_BLD)/test_sync_primitives.elf \
    $(TEST_BLD)/test_mutex_handshake.elf \
    $(TEST_BLD)/test_ap_work_protocol.elf \
    $(TEST_BLD)/test_double_state_publish.elf \
    $(TEST_BLD)/test_clocksource.elf \
    $(TEST_BLD)/test_process_image_lifecycle.elf \
    $(TEST_BLD)/test_fork_user_map.elf \
    $(TEST_BLD)/test_percpu_layout.elf \
    $(TEST_BLD)/test_smp_starting_gate.elf

# Focused boundary test links the production dispatcher.
$(TEST_BLD)/test_syscall_dispatch.elf: $(TEST_CASES)/test_syscall_dispatch.c $(TESTS_DIR)/kernel/syscall/dispatch.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) $^ -o $@

.PHONY: test-syscall-dispatch
test-syscall-dispatch: $(TEST_BLD)/test_syscall_dispatch.elf
	$<

$(TEST_BLD)/test_compat_linux.elf: $(TEST_CASES)/test_compat_linux.c \
    $(TESTS_DIR)/kernel/syscall/dispatch.c \
    $(TESTS_DIR)/kernel/syscall/compat/dispatch.c \
    $(TESTS_DIR)/kernel/syscall/compat/table_x86_64.c \
    $(TESTS_DIR)/kernel/syscall/compat/proc.c \
    $(TESTS_DIR)/kernel/syscall/compat/fs.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) $^ -o $@

.PHONY: test-compat-linux
test-compat-linux: $(TEST_BLD)/test_compat_linux.elf
	$<

# Standalone wait test
$(TEST_BLD)/test_wait_basic.elf: $(TEST_BLD)/test_wait_basic.o
	$(HOST_CC) -o $@ $<

# Sync primitive test — compiles the real rwlock/seqlock implementations.
$(TEST_BLD)/test_sync_primitives.o: $(TEST_CASES)/test_sync_primitives.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SYNC_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/rwlock.o: $(TESTS_DIR)/kernel/sync/rwlock.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SYNC_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/seqlock.o: $(TESTS_DIR)/kernel/sync/seqlock.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SYNC_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_sync_primitives.elf: $(TEST_BLD)/test_sync_primitives.o \
		$(TEST_BLD)/rwlock.o $(TEST_BLD)/seqlock.o
	$(HOST_CC) -pthread -o $@ $^

# Mutex handshake test
MUTEX_HOST_CFLAGS := $(HOST_CFLAGS) -include $(TEST_MOCK)/mutex_runtime.h \
    -I$(TEST_INC) $(KERNEL_INC) -idirafter $(TESTS_DIR)/libc/include

$(TEST_BLD)/mutex_handshake.o: $(TESTS_DIR)/kernel/sync/mutex.c $(TEST_MOCK)/mutex_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(MUTEX_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/mutex_wait.o: $(TESTS_DIR)/kernel/sync/wait.c $(TEST_MOCK)/mutex_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(MUTEX_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_mutex_handshake.o: $(TEST_CASES)/test_mutex_handshake.c $(TEST_MOCK)/mutex_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(MUTEX_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_mutex_handshake.elf: $(TEST_BLD)/test_mutex_handshake.o \
        $(TEST_BLD)/mutex_handshake.o $(TEST_BLD)/mutex_wait.o
	$(HOST_CC) -Wl,--wrap=wait_queue_arm -Wl,--wrap=wait_queue_sleep -o $@ $^

# ap_work protocol hosttest (M3 Task 10)
AP_WORK_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC)
AP_WORK_PROD_CFLAGS := $(AP_WORK_HOST_CFLAGS) \
                        -include $(TEST_MOCK)/ap_work_test_runtime.h

$(TEST_BLD)/ap_work_production.o: $(TESTS_DIR)/kernel/arch/aarch64/smp/ap_work.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/ap_work.h \
		$(TEST_MOCK)/ap_work_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(AP_WORK_PROD_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_ap_work_protocol.o: $(TEST_CASES)/test_ap_work_protocol.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/ap_work.h \
		$(TEST_MOCK)/ap_work_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(AP_WORK_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_ap_work_protocol.elf: $(TEST_BLD)/test_ap_work_protocol.o \
		$(TEST_BLD)/ap_work_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test_ap_work_protocol
test_ap_work_protocol: $(TEST_BLD)/test_ap_work_protocol.elf
	$<

# percpu_t layout pin hosttest (M3 Task 9)
$(TEST_BLD)/test_percpu_layout.o: $(TEST_CASES)/test_percpu_layout.c \
		$(TESTS_DIR)/kernel/include/percpu/percpu.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_INC) $(KERNEL_INC) $(LIBC_INC) \
		-c $< -o $@

$(TEST_BLD)/test_percpu_layout.elf: $(TEST_BLD)/test_percpu_layout.o
	$(HOST_CC) -o $@ $^

.PHONY: test_percpu_layout
test_percpu_layout: $(TEST_BLD)/test_percpu_layout.elf
	$<

# Process-image lifecycle test
LIFECYCLE_HOST_CFLAGS := $(HOST_CFLAGS) $(LIBC_INC) \
                         -I$(TESTS_DIR)/kernel/include \
                         -include $(TEST_MOCK)/lifecycle/lifecycle_runtime.h \
                         -I$(TEST_MOCK) -I$(TEST_INC) \
                         -I$(TEST_MOCK)/lifecycle

$(TEST_BLD)/vma_production.o: $(TESTS_DIR)/kernel/memory/vma.c \
		$(TESTS_DIR)/kernel/include/memory/vma.h \
		$(TESTS_DIR)/kernel/include/sched/task.h \
		$(TEST_MOCK)/lifecycle/lifecycle_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(LIFECYCLE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/lifecycle_stubs.o: $(TEST_MOCK)/lifecycle/lifecycle_stubs.c \
		$(TEST_MOCK)/lifecycle/lifecycle_stubs.h \
		$(TEST_MOCK)/lifecycle/lifecycle_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(LIFECYCLE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_process_image_lifecycle.o: $(TEST_CASES)/test_process_image_lifecycle.c \
		$(TEST_MOCK)/lifecycle/lifecycle_stubs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(LIFECYCLE_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_process_image_lifecycle.elf: $(TEST_BLD)/test_process_image_lifecycle.o \
		$(TEST_BLD)/vma_production.o \
		$(TEST_BLD)/lifecycle_stubs.o
	$(HOST_CC) -Wl,--wrap=kmalloc,--wrap=kfree -o $@ $^

# Fork / teardown 4 KiB ownership test
FORK_USER_MAP_HOST_CFLAGS := $(HOST_CFLAGS) $(LIBC_INC) \
                              -I$(TESTS_DIR)/kernel/include \
                              -include $(TEST_MOCK)/fork_user_map/fork_user_map_runtime.h \
                              -I$(TEST_MOCK) -I$(TEST_INC) \
                              -I$(TEST_MOCK)/fork_user_map

$(TEST_BLD)/vma_fork_user_map_production.o: $(TESTS_DIR)/kernel/memory/vma.c \
		$(TESTS_DIR)/kernel/include/memory/vma.h \
		$(TEST_MOCK)/fork_user_map/fork_user_map_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FORK_USER_MAP_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/fork_user_map_stubs.o: $(TEST_MOCK)/fork_user_map/fork_user_map_stubs.c \
		$(TEST_MOCK)/fork_user_map/fork_user_map_stubs.h \
		$(TEST_MOCK)/fork_user_map/fork_user_map_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FORK_USER_MAP_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_fork_user_map.o: $(TEST_CASES)/test_fork_user_map.c \
		$(TEST_MOCK)/fork_user_map/fork_user_map_stubs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FORK_USER_MAP_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_fork_user_map.elf: $(TEST_BLD)/test_fork_user_map.o \
		$(TEST_BLD)/vma_fork_user_map_production.o \
		$(TEST_BLD)/fork_user_map_stubs.o
	$(HOST_CC) -o $@ $^

# M3 Task 11: BSP double-state publish ordering
GIC_MARKER_CFLAGS ?= $(HOST_CFLAGS) -DOS01_KERNEL_SRC=\"$(TESTS_DIR)\" \
                     -I$(TEST_INC) -include $(TEST_MOCK)/test_platform.h

$(TEST_BLD)/test_double_state_publish.o: $(TEST_CASES)/test_double_state_publish.c \
		$(TESTS_DIR)/kernel/arch/aarch64/boot/main.c \
		$(TESTS_DIR)/kernel/arch/aarch64/smp/smp.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GIC_MARKER_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_double_state_publish.elf: $(TEST_BLD)/test_double_state_publish.o
	$(HOST_CC) -o $@ $<

.PHONY: test_double_state_publish
test_double_state_publish: $(TEST_BLD)/test_double_state_publish.elf
	$<

# Clocksource test
CLOCKSOURCE_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)
CLOCKSOURCE_PREINCLUDE := -include $(TEST_MOCK)/clocksource_test_runtime.h

$(TEST_BLD)/clocksource_production.o: $(TESTS_DIR)/kernel/time/clocksource.c \
		$(TESTS_DIR)/kernel/include/time/clocksource.h \
		$(TESTS_DIR)/kernel/include/time/clocksource_internal.h \
		$(TEST_MOCK)/clocksource_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(CLOCKSOURCE_HOST_CFLAGS) \
		$(CLOCKSOURCE_PREINCLUDE) -c $< -o $@

$(TEST_BLD)/clocksource_test_stubs.o: $(TEST_MOCK)/clocksource_test_stubs.c \
		$(TEST_MOCK)/clocksource_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(CLOCKSOURCE_HOST_CFLAGS) \
		$(CLOCKSOURCE_PREINCLUDE) -c $< -o $@

$(TEST_BLD)/test_clocksource.o: $(TEST_CASES)/test_clocksource.c \
		$(TESTS_DIR)/kernel/include/time/clocksource.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/clocksource.h \
		$(TESTS_DIR)/kernel/include/time/clocksource_internal.h \
		$(TEST_MOCK)/clocksource_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(CLOCKSOURCE_HOST_CFLAGS) \
		$(CLOCKSOURCE_PREINCLUDE) -c $< -o $@

$(TEST_BLD)/test_clocksource.elf: $(TEST_BLD)/test_clocksource.o \
		$(TEST_BLD)/clocksource_production.o \
		$(TEST_BLD)/clocksource_test_stubs.o
	$(HOST_CC) -o $@ $^

.PHONY: test_clocksource
test_clocksource: $(TEST_BLD)/test_clocksource.elf
	$<

# M3.5 Task 23: smp_starting gate
$(TEST_BLD)/test_smp_starting_gate.o: $(TEST_CASES)/test_smp_starting_gate.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/vmm_gate.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/dtb.h \
		$(TEST_MOCK)/vmm_gate_test_runtime.h \
		$(TESTS_DIR)/kernel/arch/aarch64/memory/page_table.c \
		$(TESTS_DIR)/kernel/arch/aarch64/memory/vmm_backend.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VMM_GATE_HOST_CFLAGS) -DOS01_KERNEL_SRC=\"$(TESTS_DIR)\" \
		$(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_smp_starting_gate.elf: $(TEST_BLD)/test_smp_starting_gate.o \
		$(TEST_BLD)/vmm_gate_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test_smp_starting_gate
test_smp_starting_gate: $(TEST_BLD)/test_smp_starting_gate.elf
	$<
