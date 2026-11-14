# ── BOOT-1 CPU lifecycle host tests ─────────────────────────────
# Both cases compile the REAL production object and replace only the
# machine boundary via preinclude mocks (mock/boot_cpu_runtime.h for
# percpu.c, mock/x86_smp_boot_sync_runtime.h for boot_sync.c) — the
# construction and bounded-wait algorithms under test are never
# reimplemented in the tests.
$(eval $(call register_host_test,test_boot_cpu_lifecycle,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_x86_smp_boot_sync,sched,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_boot_cpu_lifecycle.elf \
    $(TEST_BLD)/test_x86_smp_boot_sync.elf

# Private object names: no other suite may own the production compile of
# these sources, and a shared name would clobber that object.
BOOT_CPU_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)
BOOT_CPU_PROD_CFLAGS := $(BOOT_CPU_HOST_CFLAGS) \
    -include $(TEST_MOCK)/boot_cpu_runtime.h

$(TEST_BLD)/boot_cpu_percpu_production.o: $(TESTS_DIR)/kernel/percpu/percpu.c \
		$(TEST_MOCK)/boot_cpu_runtime.h \
		$(TESTS_DIR)/kernel/include/percpu/percpu.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT_CPU_PROD_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_boot_cpu_lifecycle.o: $(TEST_CASES)/test_boot_cpu_lifecycle.c \
		$(TEST_MOCK)/boot_cpu_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT_CPU_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_boot_cpu_lifecycle.elf: $(TEST_BLD)/test_boot_cpu_lifecycle.o \
		$(TEST_BLD)/boot_cpu_percpu_production.o
	$(HOST_CC) -o $@ $^

X86_BOOT_SYNC_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC) -I$(TEST_INC)
X86_BOOT_SYNC_PROD_CFLAGS := $(X86_BOOT_SYNC_HOST_CFLAGS) \
	-include $(TEST_MOCK)/x86_smp_boot_sync_runtime.h

$(TEST_BLD)/x86_boot_sync_production.o: \
		$(TESTS_DIR)/kernel/arch/x86_64/smp/boot_sync.c \
		$(TESTS_DIR)/kernel/include/arch/x86_64/smp/boot_sync.h \
		$(TEST_MOCK)/x86_smp_boot_sync_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(X86_BOOT_SYNC_PROD_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_x86_smp_boot_sync.o: $(TEST_CASES)/test_x86_smp_boot_sync.c \
		$(TEST_MOCK)/x86_smp_boot_sync_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(X86_BOOT_SYNC_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_x86_smp_boot_sync.elf: $(TEST_BLD)/test_x86_smp_boot_sync.o \
		$(TEST_BLD)/x86_boot_sync_production.o
	$(HOST_CC) -o $@ $^

# Focused dispatch (root entry: make test-boot-lifecycle-host CASE=...):
#   construction — the real construction test + existing layout/publish
#                  tests; does NOT build the wait binary
#   wait         — only the bounded-wait helper test
#   all          — both groups
.PHONY: test_boot_lifecycle_construction test_boot_lifecycle_wait test_boot_lifecycle_all
test_boot_lifecycle_construction: $(TEST_BLD)/test_boot_cpu_lifecycle.elf \
		test_percpu_layout test_double_state_publish
	$(TEST_BLD)/test_boot_cpu_lifecycle.elf

test_boot_lifecycle_wait: $(TEST_BLD)/test_x86_smp_boot_sync.elf
	$(TEST_BLD)/test_x86_smp_boot_sync.elf

test_boot_lifecycle_all: test_boot_lifecycle_construction test_boot_lifecycle_wait \
	test_tlb_serial_protocol

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
$(TEST_BLD)/syscall_dispatch_production.o: $(TESTS_DIR)/kernel/syscall/dispatch.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -c $< -o $@

$(TEST_BLD)/test_syscall_dispatch.o: $(TEST_CASES)/test_syscall_dispatch.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -c $< -o $@

$(TEST_BLD)/test_syscall_dispatch.elf: $(TEST_BLD)/test_syscall_dispatch.o $(TEST_BLD)/syscall_dispatch_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test-syscall-dispatch
test-syscall-dispatch: $(TEST_BLD)/test_syscall_dispatch.elf \
		$(TEST_BLD)/test_syscall_dispatch_subset.elf
	$(TEST_BLD)/test_syscall_dispatch.elf
	$(TEST_BLD)/test_syscall_dispatch_subset.elf

# ── AArch64 syscall subset (M4 Task 1) ─────────────────────────
#
# The SAME production dispatcher source, compiled with the AArch64
# subset configuration: only the four shared native entries stay
# visible.  The case source is shared with the full-table binary — the
# subset assertions live behind OS01_SYSCALL_A64_SUBSET in the case
# itself, so both configurations are proven by one source of truth.
$(eval $(call register_host_test,test_syscall_dispatch_subset,sched,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_syscall_dispatch_subset.elf

$(TEST_BLD)/syscall_dispatch_a64_subset_production.o: \
		$(TESTS_DIR)/kernel/syscall/dispatch.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -DOS01_SYSCALL_A64_SUBSET=1 \
		-c $< -o $@

$(TEST_BLD)/test_syscall_dispatch_subset.o: $(TEST_CASES)/test_syscall_dispatch.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -DOS01_SYSCALL_A64_SUBSET=1 \
		-c $< -o $@

$(TEST_BLD)/test_syscall_dispatch_subset.elf: \
		$(TEST_BLD)/test_syscall_dispatch_subset.o \
		$(TEST_BLD)/syscall_dispatch_a64_subset_production.o
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/compat_dispatch_production.o: $(TESTS_DIR)/kernel/syscall/compat/dispatch.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -c $< -o $@

$(TEST_BLD)/compat_table_x86_64_production.o: $(TESTS_DIR)/kernel/syscall/compat/table_x86_64.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -c $< -o $@

$(TEST_BLD)/compat_proc_production.o: $(TESTS_DIR)/kernel/syscall/compat/proc.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/compat_fs_production.o: $(TESTS_DIR)/kernel/syscall/compat/fs.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -c $< -o $@

$(TEST_BLD)/test_compat_linux.o: $(TEST_CASES)/test_compat_linux.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_compat_linux.elf: $(TEST_BLD)/test_compat_linux.o \
    $(TEST_BLD)/sched_resource_production.o \
    $(TEST_BLD)/syscall_dispatch_production.o \
    $(TEST_BLD)/compat_dispatch_production.o \
    $(TEST_BLD)/compat_table_x86_64_production.o \
    $(TEST_BLD)/compat_proc_production.o \
    $(TEST_BLD)/compat_fs_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test-compat-linux
test-compat-linux: $(TEST_BLD)/test_compat_linux.elf
	$<

$(TEST_BLD)/sched_resource_production.o: $(TESTS_DIR)/kernel/sched/resource.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/sys_resource_production.o: $(TESTS_DIR)/kernel/syscall/sys_resource.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/libc_resource_production.o: $(TESTS_DIR)/libc/resource/resource.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) $(LIBC_INC) -D'errno=(*__errno_location())' -include $(TEST_INC)/gfx_client_errno.h -c $< -o $@

# The test_nofile_limits case (ARCH-10 Task 5 resource limits + Task 6
# descriptor installation) is built by hosttests/mk/fs.mk — it links the
# production descriptor-installation objects.

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

$(TEST_BLD)/vma_production.o: $(TESTS_DIR)/kernel/arch/x86_64/memory/vma.c \
		$(TESTS_DIR)/kernel/include/memory/vma.h \
		$(TESTS_DIR)/kernel/include/memory/vm_area.h \
		$(TESTS_DIR)/kernel/include/sched/task.h \
		$(TEST_MOCK)/lifecycle/lifecycle_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(LIFECYCLE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/vm_area_lifecycle_production.o: $(TESTS_DIR)/kernel/memory/vm_area.c \
		$(TESTS_DIR)/kernel/include/memory/vm_area.h \
		$(TESTS_DIR)/kernel/include/memory/vma.h \
		$(TEST_MOCK)/lifecycle/lifecycle_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(LIFECYCLE_HOST_CFLAGS) -c $< -o $@

# mm_alloc moved out of vma.c (aarch64 M4 user-mm plan Task 1) — the
# lifecycle suite links the production mm.c object so the mm_t lifecycle
# stays covered by the same harness.
$(TEST_BLD)/mm_production.o: $(TESTS_DIR)/kernel/memory/mm.c \
		$(TESTS_DIR)/kernel/include/memory/mm.h \
		$(TESTS_DIR)/kernel/include/memory/vma.h \
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
		$(TEST_BLD)/vm_area_lifecycle_production.o \
		$(TEST_BLD)/mm_production.o \
		$(TEST_BLD)/lifecycle_stubs.o
	$(HOST_CC) -Wl,--wrap=kmalloc,--wrap=kfree -o $@ $^

# Fork / teardown 4 KiB ownership test
FORK_USER_MAP_HOST_CFLAGS := $(HOST_CFLAGS) $(LIBC_INC) \
                              -I$(TESTS_DIR)/kernel/include \
                              -include $(TEST_MOCK)/fork_user_map/fork_user_map_runtime.h \
                              -I$(TEST_MOCK) -I$(TEST_INC) \
                              -I$(TEST_MOCK)/fork_user_map

$(TEST_BLD)/vma_fork_user_map_production.o: $(TESTS_DIR)/kernel/arch/x86_64/memory/vma.c \
		$(TESTS_DIR)/kernel/include/memory/vma.h \
		$(TESTS_DIR)/kernel/include/memory/vm_area.h \
		$(TEST_MOCK)/fork_user_map/fork_user_map_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FORK_USER_MAP_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/vm_area_fork_user_map_production.o: $(TESTS_DIR)/kernel/memory/vm_area.c \
		$(TESTS_DIR)/kernel/include/memory/vm_area.h \
		$(TESTS_DIR)/kernel/include/memory/vma.h \
		$(TEST_MOCK)/fork_user_map/fork_user_map_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FORK_USER_MAP_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/mm_fork_user_map_production.o: $(TESTS_DIR)/kernel/memory/mm.c \
		$(TESTS_DIR)/kernel/include/memory/mm.h \
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
		$(TEST_BLD)/vm_area_fork_user_map_production.o \
		$(TEST_BLD)/mm_fork_user_map_production.o \
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

# EL1 scheduler context, constructor and layout regression tests.
$(eval $(call register_host_test,test_switch_complete,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_kthread_ctor,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_sched_layout_offsets,sched,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_switch_complete.elf \
    $(TEST_BLD)/test_kthread_ctor.elf \
    $(TEST_BLD)/test_sched_layout_offsets.elf

# aarch64 EL1 scheduler plan Phase 5 Task 4 (spec §10.1 items 1-2):
# host-compiles the REAL <arch/context.h> facade so the kernel's own
# compile-time layout asserts run in a host build too, then checks the
# x86 thread_t offsets against their RECORDED values (independent of the
# header's asserts — editing those alone cannot hide a drift).
$(TEST_BLD)/test_sched_layout_offsets.elf: $(TEST_CASES)/test_sched_layout_offsets.c \
		$(TESTS_DIR)/kernel/include/arch/context.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/context.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -I$(TEST_INC) $(LIBC_INC) -c $< -o $(TEST_BLD)/test_sched_layout_offsets.o
	$(HOST_CC) -o $@ $(TEST_BLD)/test_sched_layout_offsets.o

.PHONY: test-sched-layout-offsets
test-sched-layout-offsets: $(TEST_BLD)/test_sched_layout_offsets.elf
	$<

# aarch64 EL1 scheduler plan Phase 1, Task 2: common switch tail.
# Host-compiles the REAL kernel/sched/kthread.c against
# mock/switch_complete_test_runtime.h, which short-circuits sched/task.h +
# sched/internal.h + memory/slab.h and provides a test-controlled current
# plus spy task_finish_switch / kfree (defined in the test TU).  The spy
# finish POISONS prev's members, so the case proves the tail captures the
# objects to release BEFORE the on_cpu publish (spec §8.3: a waiter may
# free prev the moment task_finish_switch clears on_cpu) and never reads
# prev after it.
SWITCH_TAIL_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC) $(LIBC_INC)
SWITCH_TAIL_PROD_CFLAGS := $(SWITCH_TAIL_HOST_CFLAGS) \
                           -include $(TEST_MOCK)/switch_complete_test_runtime.h

$(TEST_BLD)/switch_complete_production.o: $(TESTS_DIR)/kernel/sched/kthread.c \
		$(TEST_MOCK)/switch_complete_test_runtime.h \
		$(TESTS_DIR)/kernel/include/sched/internal.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SWITCH_TAIL_PROD_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_switch_complete.o: $(TEST_CASES)/test_switch_complete.c \
		$(TEST_MOCK)/switch_complete_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FRAMEWORK_INC) $(SWITCH_TAIL_HOST_CFLAGS) -c $< -o $@

# Phase 2: the production kthread.o now also carries the constructor,
# which references the boot window — boot.o rides along (its window
# simply stays closed through the tail cases).
$(TEST_BLD)/test_switch_complete.elf: $(TEST_BLD)/test_switch_complete.o \
		$(TEST_BLD)/switch_complete_production.o \
		$(TEST_BLD)/sched_boot_production.o \
		$(TEST_BLD)/sched_resource_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test-switch-complete
test-switch-complete: $(TEST_BLD)/test_switch_complete.elf
	$<

# aarch64 EL1 scheduler plan Phase 2, Task 1: the arch-neutral kthread
# constructor + boot creation window.  Reuses the SAME production object
# as test_switch_complete (the REAL kernel/sched/kthread.c host-compiled
# against mock/switch_complete_test_runtime.h — now also carrying
# kthread_alloc_and_publish) and adds the REAL kernel/sched/boot.c (the
# window state machine) as a second production object.  The extended mock
# provides the fail-index slab, the mock percpu and the spy surface; the
# test drives the production constructor and the production window latch,
# never a copy of either.
$(TEST_BLD)/sched_boot_production.o: $(TESTS_DIR)/kernel/sched/boot.c \
		$(TEST_MOCK)/switch_complete_test_runtime.h \
		$(TESTS_DIR)/kernel/include/sched/boot.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SWITCH_TAIL_PROD_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_kthread_ctor.o: $(TEST_CASES)/test_kthread_ctor.c \
		$(TEST_MOCK)/switch_complete_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FRAMEWORK_INC) $(SWITCH_TAIL_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_kthread_ctor.elf: $(TEST_BLD)/test_kthread_ctor.o \
		$(TEST_BLD)/switch_complete_production.o \
		$(TEST_BLD)/sched_boot_production.o \
		$(TEST_BLD)/sched_resource_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test-kthread-ctor
test-kthread-ctor: $(TEST_BLD)/test_kthread_ctor.elf
	$<

# ── P1 Task 1: AP cooperative scheduling contracts ──────────────
# The common binary links the REAL core.c + balance.c with controllable
# arch-hook spies; two backend binaries link the REAL per-arch
# sched_notify.c objects (plain freestanding C, host-compilable).
$(eval $(call register_host_test,test_sched_notify,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_sched_notify_x86_backend,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_sched_notify_aarch64_backend,sched,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_sched_notify.elf \
    $(TEST_BLD)/test_sched_notify_x86_backend.elf \
    $(TEST_BLD)/test_sched_notify_aarch64_backend.elf

SCHED_NOTIFY_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)
SCHED_NOTIFY_PROD_CFLAGS := $(SCHED_NOTIFY_CFLAGS) \
    -include $(TEST_MOCK)/sched_notify_test_runtime.h

$(TEST_BLD)/sched_notify_core_production.o: $(TESTS_DIR)/kernel/sched/core.c \
		$(TEST_MOCK)/sched_notify_test_runtime.h \
		$(TESTS_DIR)/kernel/include/sched/internal.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SCHED_NOTIFY_PROD_CFLAGS) -c $< -o $@

$(TEST_BLD)/sched_notify_balance_production.o: $(TESTS_DIR)/kernel/sched/balance.c \
		$(TEST_MOCK)/sched_notify_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SCHED_NOTIFY_PROD_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_sched_notify.o: $(TEST_CASES)/test_sched_notify.c \
		$(TEST_MOCK)/sched_notify_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FRAMEWORK_INC) $(SCHED_NOTIFY_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_sched_notify.elf: $(TEST_BLD)/test_sched_notify.o \
		$(TEST_BLD)/sched_notify_core_production.o \
		$(TEST_BLD)/sched_notify_balance_production.o
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/sched_notify_x86_backend_production.o: \
		$(TESTS_DIR)/kernel/arch/x86_64/cpu/sched_notify.c \
		$(TEST_MOCK)/sched_notify_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SCHED_NOTIFY_PROD_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_sched_notify_x86_backend.o: \
		$(TEST_CASES)/test_sched_notify_x86_backend.c \
		$(TEST_MOCK)/sched_notify_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FRAMEWORK_INC) $(SCHED_NOTIFY_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_sched_notify_x86_backend.elf: \
		$(TEST_BLD)/test_sched_notify_x86_backend.o \
		$(TEST_BLD)/sched_notify_x86_backend_production.o
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/sched_notify_aarch64_backend_production.o: \
		$(TESTS_DIR)/kernel/arch/aarch64/cpu/sched_notify.c \
		$(TEST_MOCK)/sched_notify_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SCHED_NOTIFY_PROD_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_sched_notify_aarch64_backend.o: \
		$(TEST_CASES)/test_sched_notify_aarch64_backend.c \
		$(TEST_MOCK)/sched_notify_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FRAMEWORK_INC) $(SCHED_NOTIFY_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_sched_notify_aarch64_backend.elf: \
		$(TEST_BLD)/test_sched_notify_aarch64_backend.o \
		$(TEST_BLD)/sched_notify_aarch64_backend_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test-sched-notify
test-sched-notify: $(TEST_BLD)/test_sched_notify.elf \
                   $(TEST_BLD)/test_sched_notify_x86_backend.elf \
                   $(TEST_BLD)/test_sched_notify_aarch64_backend.elf
	$(TEST_BLD)/test_sched_notify.elf
	$(TEST_BLD)/test_sched_notify_x86_backend.elf
	$(TEST_BLD)/test_sched_notify_aarch64_backend.elf

# ── P1 Task 2: concurrency-safe scheduler observer ──────────────
# Compiles the REAL kernel/selftest/aarch64_sched_observer.c against a
# pthread-backed spin surface and drives it from real host threads.
$(eval $(call register_host_test,test_aarch64_sched_observer,sched,protocol-or-legacy))

TEST_BINS += $(TEST_BLD)/test_aarch64_sched_observer.elf

# No LIBC_INC: the OS01 libc ships a pthread.h stub that would shadow
# the host glibc header (same reason test_sync_primitives omits it).
OBSERVER_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC) -I$(TEST_INC) \
    -DOS01_SELFTEST=1
OBSERVER_PROD_CFLAGS := $(OBSERVER_CFLAGS) -pthread \
    -DOS01_HOST_SCHED_OBSERVER=1 \
    -include $(TEST_MOCK)/aarch64_sched_observer_runtime.h

$(TEST_BLD)/sched_observer_production.o: \
		$(TESTS_DIR)/kernel/selftest/aarch64_sched_observer.c \
		$(TESTS_DIR)/kernel/include/selftest/aarch64_sched_observer.h \
		$(TEST_MOCK)/aarch64_sched_observer_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(OBSERVER_PROD_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_aarch64_sched_observer.o: \
		$(TEST_CASES)/test_aarch64_sched_observer.c \
		$(TEST_MOCK)/aarch64_sched_observer_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FRAMEWORK_INC) $(OBSERVER_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_aarch64_sched_observer.elf: \
		$(TEST_BLD)/test_aarch64_sched_observer.o \
		$(TEST_BLD)/sched_observer_production.o
	$(HOST_CC) -pthread -o $@ $^

.PHONY: test-aarch64-sched-observer
test-aarch64-sched-observer: $(TEST_BLD)/test_aarch64_sched_observer.elf
	$<

# ── P2 Task 2: blocker protocol + wake producers ────────────────
# Compiles the REAL core.c against the blocker mock; every case drives
# the production blocker_wait / blocker_wake / task_wake /
# task_finish_switch — never a reimplementation.
$(eval $(call register_host_test,test_blocker_remote_wake,sched,protocol-or-legacy))

TEST_BINS += $(TEST_BLD)/test_blocker_remote_wake.elf

BLOCKER_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)
BLOCKER_PROD_CFLAGS := $(BLOCKER_CFLAGS) \
    -include $(TEST_MOCK)/blocker_runtime.h

$(TEST_BLD)/blocker_core_production.o: $(TESTS_DIR)/kernel/sched/core.c \
		$(TEST_MOCK)/blocker_runtime.h \
		$(TESTS_DIR)/kernel/include/sched/internal.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BLOCKER_PROD_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_blocker_remote_wake.o: $(TEST_CASES)/test_blocker_remote_wake.c \
		$(TEST_MOCK)/blocker_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FRAMEWORK_INC) $(BLOCKER_CFLAGS) -c $< -o $@

# --wrap=schedule: the spy counts calls while the production core.o
# keeps calling (the cases that need the REAL schedule() call
# __real_schedule through the wrapper).
$(TEST_BLD)/blocker_balance_production.o: $(TESTS_DIR)/kernel/sched/balance.c \
		$(TEST_MOCK)/blocker_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BLOCKER_PROD_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_blocker_remote_wake.elf: $(TEST_BLD)/test_blocker_remote_wake.o \
		$(TEST_BLD)/blocker_core_production.o \
		$(TEST_BLD)/blocker_balance_production.o
	$(HOST_CC) -Wl,--wrap=schedule -o $@ $^

.PHONY: test-blocker-remote-wake
test-blocker-remote-wake: $(TEST_BLD)/test_blocker_remote_wake.elf
	$<

# ── aarch64 per-CPU tick accounting (Task 1 + Task 2) ───────────
# Compiles the REAL kernel/arch/aarch64/platform/time.c,
# kernel/arch/aarch64/cpu/sched_tick.c, kernel/sched/fair.c and
# kernel/sched/accounting.c against mock/aarch64_sched_tick_runtime.h,
# which replaces ONLY the machine / GIC / DTB / IRQ / CPU-identity
# boundaries.  The configuration validation, the publish/acquire
# protocol, the TVAL -> CTL -> ISB -> read-back arm order, the
# ENABLE-clearing failure path, the ready / started / admission gates
# and the exact busy/idle accounting increments are the PRODUCTION
# algorithm — the mock never re-implements them.
$(eval $(call register_host_test,test_aarch64_sched_tick,sched,protocol-or-legacy))

TEST_BINS += $(TEST_BLD)/test_aarch64_sched_tick.elf

# No LIBC_INC: the fixture captures the log lines through the host's
# vsnprintf and the OS01 libc headers would shadow <stdio.h>.
SCHED_TICK_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC) -I$(TEST_INC)
SCHED_TICK_PROD_CFLAGS := $(SCHED_TICK_CFLAGS) \
    -include $(TEST_MOCK)/aarch64_sched_tick_runtime.h \
    -ffunction-sections -fdata-sections

# Private object names: the monitor plan already owns
# accounting_production.o — a shared name would clobber that object.
$(TEST_BLD)/aarch64_timer_production.o: \
		$(TESTS_DIR)/kernel/arch/aarch64/platform/time.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/timer.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/sched_tick.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/reg.h \
		$(TEST_MOCK)/aarch64_sched_tick_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SCHED_TICK_PROD_CFLAGS) -c $< -o $@

$(TEST_BLD)/aarch64_sched_tick_production.o: \
		$(TESTS_DIR)/kernel/arch/aarch64/cpu/sched_tick.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/sched_tick.h \
		$(TEST_MOCK)/aarch64_sched_tick_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SCHED_TICK_PROD_CFLAGS) -c $< -o $@

# The REAL update_curr(): fair.c's runqueue helpers are dropped by
# --gc-sections, but update_curr itself must never be stubbed.
$(TEST_BLD)/aarch64_fair_production.o: \
		$(TESTS_DIR)/kernel/sched/fair.c \
		$(TEST_MOCK)/aarch64_sched_tick_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SCHED_TICK_PROD_CFLAGS) -c $< -o $@

# The REAL per-CPU usage counters.
$(TEST_BLD)/aarch64_accounting_production.o: \
		$(TESTS_DIR)/kernel/sched/accounting.c \
		$(TEST_MOCK)/aarch64_sched_tick_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SCHED_TICK_PROD_CFLAGS) -c $< -o $@

# The real red-black tree: fair.c's enqueue/dequeue helpers reference
# it.  Compiled against the OS01 libc headers only — this object never
# includes <stdio.h>.
$(TEST_BLD)/aarch64_sched_tick_rbtree.o: \
		$(TESTS_DIR)/libc/rbtree/rbtree.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TESTS_DIR)/libc/include -c $< -o $@

# The test TU does NOT use FRAMEWORK_INC: the mock owns the whole
# host surface (percpu_t, task_t, the lock recorder and the current
# accessor), and test_platform.h would collide with it.
$(TEST_BLD)/test_aarch64_sched_tick.o: \
		$(TEST_CASES)/test_aarch64_sched_tick.c \
		$(TEST_MOCK)/aarch64_sched_tick_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SCHED_TICK_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_aarch64_sched_tick.elf: \
		$(TEST_BLD)/test_aarch64_sched_tick.o \
		$(TEST_BLD)/aarch64_timer_production.o \
		$(TEST_BLD)/aarch64_sched_tick_production.o \
		$(TEST_BLD)/aarch64_fair_production.o \
		$(TEST_BLD)/aarch64_accounting_production.o \
		$(TEST_BLD)/aarch64_sched_tick_rbtree.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

.PHONY: test-aarch64-sched-tick-host
test-aarch64-sched-tick-host: $(TEST_BLD)/test_aarch64_sched_tick.elf
	$<

# ── aarch64 schedule-charge regression (Task 2, step 2) ─────────
# The REAL kernel/sched/core.c + fair.c + balance.c, host-compiled
# through the blocker mock in its BLOCKER_RUNTIME_REAL_FAIR mode:
# update_curr() is the REAL implementation, so the assertion is about
# the real vruntime value and never about a call counter.
#
# The same sources are compiled TWICE:
#   - the aarch64 view (-D__aarch64__): core.c takes the aarch64
#     branch, so schedule() does NOT charge the vruntime;
#   - the x86_64 view: schedule() still charges it.
# The host system ABI macros are preserved (-U__x86_64__ would break
# the host libc) and the machine headers are masked by the preinclude.
$(eval $(call register_host_test,test_aarch64_sched_charge,sched,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_aarch64_sched_charge_aarch64.elf \
    $(TEST_BLD)/test_aarch64_sched_charge_x86.elf

CHARGE_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)
CHARGE_AARCH64_CFLAGS := $(CHARGE_HOST_CFLAGS) -D__aarch64__ \
    -DBLOCKER_RUNTIME_REAL_FAIR=1 \
    -include $(TEST_MOCK)/blocker_runtime.h
CHARGE_X86_CFLAGS := $(CHARGE_HOST_CFLAGS) \
    -DBLOCKER_RUNTIME_REAL_FAIR=1 \
    -include $(TEST_MOCK)/blocker_runtime.h

$(TEST_BLD)/aarch64_charge_rbtree.o: $(TESTS_DIR)/libc/rbtree/rbtree.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TESTS_DIR)/libc/include -c $< -o $@

$(TEST_BLD)/aarch64_charge_aarch64_core.o: \
		$(TESTS_DIR)/kernel/sched/core.c \
		$(TEST_MOCK)/blocker_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(CHARGE_AARCH64_CFLAGS) -c $< -o $@

$(TEST_BLD)/aarch64_charge_aarch64_fair.o: \
		$(TESTS_DIR)/kernel/sched/fair.c \
		$(TEST_MOCK)/blocker_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(CHARGE_AARCH64_CFLAGS) -c $< -o $@

$(TEST_BLD)/aarch64_charge_aarch64_balance.o: \
		$(TESTS_DIR)/kernel/sched/balance.c \
		$(TEST_MOCK)/blocker_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(CHARGE_AARCH64_CFLAGS) -c $< -o $@

$(TEST_BLD)/aarch64_charge_aarch64_test.o: \
		$(TEST_CASES)/test_aarch64_sched_charge.c \
		$(TEST_MOCK)/blocker_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FRAMEWORK_INC) $(CHARGE_HOST_CFLAGS) -DBLOCKER_RUNTIME_REAL_FAIR=1 -DEXPECT_SCHED_CHARGE=0 -c $< -o $@

$(TEST_BLD)/test_aarch64_sched_charge_aarch64.elf: \
		$(TEST_BLD)/aarch64_charge_aarch64_test.o \
		$(TEST_BLD)/aarch64_charge_aarch64_core.o \
		$(TEST_BLD)/aarch64_charge_aarch64_fair.o \
		$(TEST_BLD)/aarch64_charge_aarch64_balance.o \
		$(TEST_BLD)/aarch64_charge_rbtree.o
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/aarch64_charge_x86_core.o: \
		$(TESTS_DIR)/kernel/sched/core.c \
		$(TEST_MOCK)/blocker_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(CHARGE_X86_CFLAGS) -c $< -o $@

$(TEST_BLD)/aarch64_charge_x86_fair.o: \
		$(TESTS_DIR)/kernel/sched/fair.c \
		$(TEST_MOCK)/blocker_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(CHARGE_X86_CFLAGS) -c $< -o $@

$(TEST_BLD)/aarch64_charge_x86_balance.o: \
		$(TESTS_DIR)/kernel/sched/balance.c \
		$(TEST_MOCK)/blocker_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(CHARGE_X86_CFLAGS) -c $< -o $@

$(TEST_BLD)/aarch64_charge_x86_test.o: \
		$(TEST_CASES)/test_aarch64_sched_charge.c \
		$(TEST_MOCK)/blocker_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FRAMEWORK_INC) $(CHARGE_HOST_CFLAGS) -DBLOCKER_RUNTIME_REAL_FAIR=1 -DEXPECT_SCHED_CHARGE=1 -c $< -o $@

$(TEST_BLD)/test_aarch64_sched_charge_x86.elf: \
		$(TEST_BLD)/aarch64_charge_x86_test.o \
		$(TEST_BLD)/aarch64_charge_x86_core.o \
		$(TEST_BLD)/aarch64_charge_x86_fair.o \
		$(TEST_BLD)/aarch64_charge_x86_balance.o \
		$(TEST_BLD)/aarch64_charge_rbtree.o
	$(HOST_CC) -o $@ $^

.PHONY: test-aarch64-sched-charge-host
test-aarch64-sched-charge-host: \
		$(TEST_BLD)/test_aarch64_sched_charge_aarch64.elf \
		$(TEST_BLD)/test_aarch64_sched_charge_x86.elf
	$(TEST_BLD)/test_aarch64_sched_charge_aarch64.elf
	$(TEST_BLD)/test_aarch64_sched_charge_x86.elf

# ── BOOT-3 failure policy host tests ─────────────────────────────
$(eval $(call register_host_test,test_boot_failure_subsys,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_boot_failure_log,sched,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_boot_failure_subsys.elf \
    $(TEST_BLD)/test_boot_failure_log.elf

BOOT3_HOST_CFLAGS := -I$(TEST_MOCK)/boot_failure -I$(TEST_MOCK)/include $(HOST_CFLAGS) $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)

$(TEST_BLD)/boot3_subsys_production.o: $(TESTS_DIR)/kernel/subsys/subsys.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/boot3_test_subsys.o: $(TEST_CASES)/test_boot_failure_subsys.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_boot_failure_subsys.elf: \
		$(TEST_BLD)/boot3_test_subsys.o \
		$(TEST_BLD)/boot3_subsys_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test_boot_failure_subsys
test_boot_failure_subsys: $(TEST_BLD)/test_boot_failure_subsys.elf
	$<

$(TEST_BLD)/boot3_log_failure_production.o: $(TESTS_DIR)/kernel/core/boot_failure.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/boot3_log_aarch64_impl.o: $(TESTS_DIR)/kernel/arch/aarch64/runtime/log_impl.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/boot3_test_log.o: $(TEST_CASES)/test_boot_failure_log.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_boot_failure_log.elf: \
		$(TEST_BLD)/boot3_test_log.o \
		$(TEST_BLD)/boot3_log_failure_production.o \
		$(TEST_BLD)/boot3_log_aarch64_impl.o
	$(HOST_CC) -o $@ $^

.PHONY: test_boot_failure_log
test_boot_failure_log: $(TEST_BLD)/test_boot_failure_log.elf
	$<

$(eval $(call register_host_test,test_boot_stdio,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_boot_spawn_failure,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_boot_task_init,sched,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_boot_stdio.elf \
    $(TEST_BLD)/test_boot_spawn_failure.elf \
    $(TEST_BLD)/test_boot_task_init.elf

$(TEST_BLD)/boot3_stdio_production.o: $(TESTS_DIR)/kernel/sched/boot_stdio.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(TEST_BLD)/boot3_file_production.o: $(TESTS_DIR)/kernel/fs/file.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(TEST_BLD)/boot3_test_stdio.o: $(TEST_CASES)/test_boot_stdio.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_boot_stdio.elf: \
		$(TEST_BLD)/boot3_test_stdio.o \
		$(TEST_BLD)/boot3_stdio_production.o \
		$(TEST_BLD)/boot3_file_production.o \
		$(VFS_APPEND_LINK_STUB_OBJ)
	$(HOST_CC) -Wl,--gc-sections -Wl,--wrap=calloc -o $@ $^

.PHONY: test_boot_stdio
test_boot_stdio: $(TEST_BLD)/test_boot_stdio.elf
	$<

$(TEST_BLD)/boot3_exec_production.o: $(TESTS_DIR)/kernel/sched/exec.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(TEST_BLD)/boot3_test_spawn_failure.o: $(TEST_CASES)/test_boot_spawn_failure.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_boot_spawn_failure.elf: \
		$(TEST_BLD)/boot3_test_spawn_failure.o \
		$(TEST_BLD)/boot3_exec_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

.PHONY: test_boot_spawn_failure
test_boot_spawn_failure: $(TEST_BLD)/test_boot_spawn_failure.elf
	$<

$(TEST_BLD)/boot3_sched_boot_production.o: $(TESTS_DIR)/kernel/arch/x86_64/cpu/sched_boot.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(TEST_BLD)/boot3_test_task_init.o: $(TEST_CASES)/test_boot_task_init.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_boot_task_init.elf: \
		$(TEST_BLD)/boot3_test_task_init.o \
		$(TEST_BLD)/boot5_userspace_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

# ── BOOT-5 Task 2: initial spawn placement & publication ─────────
$(eval $(call register_host_test,test_boot_services_spawn,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_boot_services_sequence,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_boot_services_services,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_boot_services_late,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_boot_services_mechanisms,sched,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_boot_services_spawn.elf \
    $(TEST_BLD)/test_boot_services_sequence.elf \
    $(TEST_BLD)/test_boot_services_services.elf \
    $(TEST_BLD)/test_boot_services_late.elf \
    $(TEST_BLD)/test_boot_services_mechanisms.elf

$(TEST_BLD)/boot5_balance_production.o: $(TESTS_DIR)/kernel/sched/balance.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(TEST_BLD)/boot5_boot_production.o: $(TESTS_DIR)/kernel/sched/boot.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(TEST_BLD)/boot5_userspace_production.o: $(TESTS_DIR)/kernel/init/userspace.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -DOS01_SELFTEST=1 -ffunction-sections -fdata-sections -c $< -o $@

$(TEST_BLD)/boot5_services_production.o: $(TESTS_DIR)/kernel/init/services.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(TEST_BLD)/boot5_late_production.o: $(TESTS_DIR)/kernel/selftest/boot_late.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -DOS01_SELFTEST=1 -ffunction-sections -fdata-sections -c $< -o $@

$(TEST_BLD)/boot5_mechanisms_sched_boot.o: $(TESTS_DIR)/kernel/arch/x86_64/cpu/sched_boot.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(TEST_BLD)/boot5_mechanisms_task_arch.o: $(TESTS_DIR)/kernel/arch/x86_64/cpu/task_arch.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -ffunction-sections -fdata-sections -c $< -o $@

$(TEST_BLD)/test_boot_services_spawn.o: $(TEST_CASES)/test_boot_services_spawn.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_boot_services_spawn.elf: \
		$(TEST_BLD)/test_boot_services_spawn.o \
		$(TEST_BLD)/boot3_exec_production.o \
		$(TEST_BLD)/boot5_balance_production.o \
		$(TEST_BLD)/boot5_boot_production.o
	$(HOST_CC) -Wl,--gc-sections -Wl,--wrap=sched_notify_remote -Wl,--wrap=malloc -Wl,--wrap=calloc -o $@ $^

$(TEST_BLD)/test_boot_services_sequence.o: $(TEST_CASES)/test_boot_services_sequence.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -DOS01_SELFTEST=1 -c $< -o $@

$(TEST_BLD)/test_boot_services_sequence.elf: \
		$(TEST_BLD)/test_boot_services_sequence.o \
		$(TEST_BLD)/boot5_userspace_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

$(TEST_BLD)/test_boot_services_services.o: $(TEST_CASES)/test_boot_services_services.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_boot_services_services.elf: \
		$(TEST_BLD)/test_boot_services_services.o \
		$(TEST_BLD)/boot5_services_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

$(TEST_BLD)/test_boot_services_late.o: $(TEST_CASES)/test_boot_services_late.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -DOS01_SELFTEST=1 -c $< -o $@

$(TEST_BLD)/test_boot_services_late.elf: \
		$(TEST_BLD)/test_boot_services_late.o \
		$(TEST_BLD)/boot5_late_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

$(TEST_BLD)/test_boot_services_mechanisms.o: $(TEST_CASES)/test_boot_services_mechanisms.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT3_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_boot_services_mechanisms.elf: \
		$(TEST_BLD)/test_boot_services_mechanisms.o \
		$(TEST_BLD)/boot5_mechanisms_sched_boot.o \
		$(TEST_BLD)/boot5_mechanisms_task_arch.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

.PHONY: test_boot_services_spawn test_boot_services_sequence test_boot_services_services test_boot_services_late test_boot_services_mechanisms
test_boot_services_spawn: $(TEST_BLD)/test_boot_services_spawn.elf
	$<
test_boot_services_sequence: $(TEST_BLD)/test_boot_services_sequence.elf
	$<
test_boot_services_services: $(TEST_BLD)/test_boot_services_services.elf
	$<
test_boot_services_late: $(TEST_BLD)/test_boot_services_late.elf
	$<
test_boot_services_mechanisms: $(TEST_BLD)/test_boot_services_mechanisms.elf
	$<

.PHONY: test_boot_task_init
test_boot_task_init: $(TEST_BLD)/test_boot_task_init.elf
	$<

# ── BOOT-4 stage contract host tests ─────────────────────────────
$(eval $(call register_host_test,test_boot_contract_subsys,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_boot_contract_cpu_prepare,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_boot_contract_softirq,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_boot_contract_sequence,sched,protocol-or-legacy))
$(eval $(call register_host_test,test_boot_contract_checks,sched,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_boot_contract_subsys.elf \
    $(TEST_BLD)/test_boot_contract_cpu_prepare.elf \
    $(TEST_BLD)/test_boot_contract_softirq.elf \
    $(TEST_BLD)/test_boot_contract_sequence.elf \
    $(TEST_BLD)/test_boot_contract_checks.elf

BOOT4_HOST_CFLAGS := -I$(TEST_MOCK)/boot_failure -I$(TEST_MOCK)/include $(HOST_CFLAGS) $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)

$(TEST_BLD)/boot4_test_subsys.o: $(TEST_CASES)/test_boot_contract_subsys.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT4_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_boot_contract_subsys.elf: \
		$(TEST_BLD)/boot4_test_subsys.o \
		$(TEST_BLD)/boot3_subsys_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test_boot_contract_subsys
test_boot_contract_subsys: $(TEST_BLD)/test_boot_contract_subsys.elf
	$<

$(TEST_BLD)/boot4_test_cpu_prepare.o: $(TEST_CASES)/test_boot_contract_cpu_prepare.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT4_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_boot_contract_cpu_prepare.elf: \
		$(TEST_BLD)/boot4_test_cpu_prepare.o \
		$(TEST_BLD)/boot3_subsys_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test_boot_contract_cpu_prepare
test_boot_contract_cpu_prepare: $(TEST_BLD)/test_boot_contract_cpu_prepare.elf
	$<

$(TEST_BLD)/boot4_test_softirq.o: $(TEST_CASES)/test_boot_contract_softirq.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT2_REG_HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/boot4_softirq_production.o: $(TESTS_DIR)/kernel/intr/softirq.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT2_REG_HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/boot4_timer_production.o: $(TESTS_DIR)/kernel/time/timer.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT2_REG_HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_boot_contract_softirq.elf: \
		$(TEST_BLD)/boot4_test_softirq.o \
		$(TEST_BLD)/boot4_softirq_production.o \
		$(TEST_BLD)/boot4_timer_production.o \
		$(TEST_BLD)/boot2-context/runtime.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

.PHONY: test_boot_contract_softirq
test_boot_contract_softirq: $(TEST_BLD)/test_boot_contract_softirq.elf
	$<

$(TEST_BLD)/boot4_test_sequence.o: $(TEST_CASES)/test_boot_contract_sequence.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT4_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_boot_contract_sequence.elf: $(TEST_BLD)/boot4_test_sequence.o
	$(HOST_CC) -o $@ $^

.PHONY: test_boot_contract_sequence
test_boot_contract_sequence: $(TEST_BLD)/test_boot_contract_sequence.elf
	$<

$(TEST_BLD)/boot4_test_checks.o: $(TEST_CASES)/test_boot_contract_checks.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT4_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/boot4_x86_checks_production.o: $(TESTS_DIR)/kernel/arch/x86_64/platform/boot_contract.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT4_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_boot_contract_checks.elf: \
		$(TEST_BLD)/boot4_test_checks.o \
		$(TEST_BLD)/boot4_x86_checks_production.o
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/boot4_test_checks_a64.o: $(TEST_CASES)/test_boot_contract_checks.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT4_HOST_CFLAGS) -U__x86_64__ -D__aarch64__ -c $< -o $@

$(TEST_BLD)/boot4_a64_checks_production.o: $(TESTS_DIR)/kernel/arch/aarch64/platform/boot_contract.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BOOT4_HOST_CFLAGS) -U__x86_64__ -D__aarch64__ -c $< -o $@

$(TEST_BLD)/test_boot_contract_checks_a64.elf: \
		$(TEST_BLD)/boot4_test_checks_a64.o \
		$(TEST_BLD)/boot4_a64_checks_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test_boot_contract_checks
test_boot_contract_checks: $(TEST_BLD)/test_boot_contract_checks.elf $(TEST_BLD)/test_boot_contract_checks_a64.elf
	$(TEST_BLD)/test_boot_contract_checks.elf
	$(TEST_BLD)/test_boot_contract_checks_a64.elf
