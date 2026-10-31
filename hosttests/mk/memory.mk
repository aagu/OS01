# ── Memory Subsystem Host Tests (spec §5) ───────────────────────

$(eval $(call register_host_test,test_pmm_basic,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_pmm_ram_rel_index,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_slab_basic,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_slab_basic_x86_count,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_slab_layout_compute,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_slab_lock_path,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_slab_idempotent_reservation,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_user_protected_ranges,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_user_write_cow,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_brk_pages,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_vmm_caller_audit,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_early_arena,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_early_arena_exhaust,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_arena_layout_chain,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_arena_2gib_boundary,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_arena_candidate_slab_bytes,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_arena_table_base_shift,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_page_table_tree,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_page_table_contract,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_page_table_vmm,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_page_table_install,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_page_table_cleanup,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_page_table_publish,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_page_table_reservation,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_page_table_layout,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_aarch64_pt_split,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_aarch64_pt_root_publish,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_aarch64_pt_software_bits,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_aarch64_pt_2m_block,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_aarch64_pt_locks,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_aarch64_arch_vmm_init,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_vmm_replace_protocol,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_vmm_prot_none,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_vmm_ack_wraparound,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_vmm_semantic_header,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_x86_vmm_backend,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_aarch64_backend_4k,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_aarch64_scratch_probe_logic,memory,protocol-or-legacy))
$(eval $(call register_host_test,test_foundational_primitives,memory,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_pmm_basic.elf \
    $(TEST_BLD)/test_pmm_ram_rel_index.elf \
    $(TEST_BLD)/test_slab_basic.elf \
    $(TEST_BLD)/test_slab_layout_compute.elf \
    $(TEST_BLD)/test_slab_lock_path.elf \
    $(TEST_BLD)/test_brk_pages.elf \
    $(TEST_BLD)/test_user_protected_ranges.elf \
    $(TEST_BLD)/test_user_write_cow.elf \
    $(TEST_BLD)/test_early_arena.elf \
    $(TEST_BLD)/test_early_arena_exhaust.elf \
    $(TEST_BLD)/test_page_table_layout.elf \
    $(TEST_BLD)/test_page_table_reservation.elf \
    $(TEST_BLD)/test_page_table_tree.elf \
    $(TEST_BLD)/test_page_table_contract.elf \
    $(TEST_BLD)/test_page_table_vmm.elf \
    $(TEST_BLD)/test_page_table_install.elf \
    $(TEST_BLD)/test_page_table_cleanup.elf \
    $(TEST_BLD)/test_page_table_publish.elf \
    $(TEST_BLD)/test_slab_idempotent_reservation.elf \
    $(TEST_BLD)/test_slab_basic_x86_count.elf \
    $(TEST_BLD)/test_vmm_caller_audit.elf \
    $(TEST_BLD)/test_vmm_semantic_header.elf \
    $(TEST_BLD)/test_x86_vmm_backend.elf \
    $(TEST_BLD)/test_aarch64_pt_software_bits.elf \
    $(TEST_BLD)/test_aarch64_pt_locks.elf \
    $(TEST_BLD)/test_aarch64_pt_2m_block.elf \
    $(TEST_BLD)/test_aarch64_backend_4k.elf \
    $(TEST_BLD)/test_vmm_replace_protocol.elf \
    $(TEST_BLD)/test_vmm_prot_none.elf \
    $(TEST_BLD)/test_vmm_ack_wraparound.elf \
    $(TEST_BLD)/test_foundational_primitives.elf \
    $(TEST_BLD)/test_aarch64_pt_root_publish.elf \
    $(TEST_BLD)/test_aarch64_pt_split.elf \
    $(TEST_BLD)/test_aarch64_arch_vmm_init.elf \
    $(TEST_BLD)/test_aarch64_scratch_probe_logic.elf \
    $(TEST_BLD)/test_arena_layout_chain.elf \
    $(TEST_BLD)/test_arena_2gib_boundary.elf \
    $(TEST_BLD)/test_arena_candidate_slab_bytes.elf \
    $(TEST_BLD)/test_arena_table_base_shift.elf

# Basic PMM and Slab tests
$(TEST_BLD)/test_pmm_basic.elf: $(TEST_BLD)/test_pmm_basic.o $(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_slab_basic.elf: $(TEST_BLD)/test_slab_basic.o $(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -o $@ $^

# PMM RAM-relative index
PMM_SHADOW_INC := -I$(TEST_MOCK)/pmm_include

$(TEST_BLD)/pmm_production.o: $(TESTS_DIR)/kernel/memory/pmm.c \
		$(TEST_MOCK)/pmm_include/arch/spinlock.h \
		$(TEST_MOCK)/pmm_include/arch/cpu.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(PMM_SHADOW_INC) $(KERNEL_INC) -c $< -o $@

$(TEST_BLD)/pmm_arch_default.o: $(TESTS_DIR)/kernel/memory/pmm_arch.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(PMM_SHADOW_INC) $(KERNEL_INC) -c $< -o $@

$(TEST_BLD)/test_pmm_ram_rel_index.o: $(TEST_CASES)/test_pmm_ram_rel_index.c \
		$(TESTS_DIR)/kernel/include/memory/pmm.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -I$(TEST_INC) -c $< -o $@

$(TEST_BLD)/test_pmm_ram_rel_index.elf: $(TEST_BLD)/test_pmm_ram_rel_index.o \
		$(TEST_BLD)/pmm_production.o $(TEST_BLD)/pmm_arch_default.o \
		$(TEST_BLD)/pmm_boot_production.o
	$(HOST_CC) -o $@ $^

# Slab layout compute
$(TEST_BLD)/test_slab_layout_compute.o: $(TEST_CASES)/test_slab_layout_compute.c \
		$(TESTS_DIR)/kernel/include/memory/slab.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -I$(TEST_INC) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/slab_test_stubs.o: $(TEST_MOCK)/slab_test_stubs.c \
		$(TESTS_DIR)/kernel/include/memory/slab.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_slab_layout_compute.elf: $(TEST_BLD)/test_slab_layout_compute.o \
		$(TEST_BLD)/slab_test_stubs.o \
		$(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -o $@ $^

# Slab lock path
$(TEST_BLD)/test_slab_lock_path.o: $(TEST_CASES)/test_slab_lock_path.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -DOS01_KERNEL_SRC=\"$(TESTS_DIR)\" -I$(TEST_INC) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_slab_lock_path.elf: $(TEST_BLD)/test_slab_lock_path.o \
		$(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -o $@ $^

# Brk pages
BRK_HOST_CFLAGS := $(HOST_CFLAGS) $(LIBC_INC) \
                   -I$(TESTS_DIR)/kernel/include \
                   -include $(TEST_MOCK)/brk/brk_runtime.h \
                   -I$(TEST_MOCK) -I$(TEST_INC) \
                   -I$(TEST_MOCK)/brk

$(TEST_BLD)/vma_brk_production.o: $(TESTS_DIR)/kernel/memory/vma.c \
		$(TESTS_DIR)/kernel/include/memory/vma.h \
		$(TEST_MOCK)/brk/brk_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BRK_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/brk_stubs.o: $(TEST_MOCK)/brk/brk_stubs.c \
		$(TEST_MOCK)/brk/brk_stubs.h \
		$(TEST_MOCK)/brk/brk_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BRK_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_brk_pages.o: $(TEST_CASES)/test_brk_pages.c \
		$(TEST_MOCK)/brk/brk_stubs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(BRK_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_brk_pages.elf: $(TEST_BLD)/test_brk_pages.o \
		$(TEST_BLD)/vma_brk_production.o \
		$(TEST_BLD)/brk_stubs.o
	$(HOST_CC) -Wl,--wrap=kmalloc,--wrap=kfree -o $@ $^

# Protected ranges
PRANGES_HOST_CFLAGS := $(HOST_CFLAGS) $(LIBC_INC) \
                       -I$(TESTS_DIR)/kernel/include \
                       -include $(TEST_MOCK)/pranges/pranges_runtime.h \
                       -I$(TEST_MOCK) -I$(TEST_INC) \
                       -I$(TEST_MOCK)/pranges

$(TEST_BLD)/vma_pranges_production.o: $(TESTS_DIR)/kernel/memory/vma.c \
		$(TESTS_DIR)/kernel/include/memory/vma.h \
		$(TEST_MOCK)/pranges/pranges_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PRANGES_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/pranges_stubs.o: $(TEST_MOCK)/pranges/pranges_stubs.c \
		$(TEST_MOCK)/pranges/pranges_stubs.h \
		$(TEST_MOCK)/pranges/pranges_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PRANGES_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_user_protected_ranges.o: $(TEST_CASES)/test_user_protected_ranges.c \
		$(TEST_MOCK)/pranges/pranges_stubs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PRANGES_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_user_protected_ranges.elf: $(TEST_BLD)/test_user_protected_ranges.o \
		$(TEST_BLD)/vma_pranges_production.o \
		$(TEST_BLD)/pranges_stubs.o
	$(HOST_CC) -o $@ $^

# User write COW
UWRITE_COW_HOST_CFLAGS := $(HOST_CFLAGS) $(LIBC_INC) \
                              -I$(TESTS_DIR)/kernel/include \
                              -include $(TEST_MOCK)/uwrite_cow/uwrite_cow_runtime.h \
                              -I$(TEST_MOCK) -I$(TEST_INC) \
                              -I$(TEST_MOCK)/uwrite_cow

$(TEST_BLD)/uaccess_uwrite_cow_production.o: $(TESTS_DIR)/kernel/memory/uaccess.c \
		$(TESTS_DIR)/kernel/include/memory/uaccess.h \
		$(TEST_MOCK)/uwrite_cow/uwrite_cow_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(UWRITE_COW_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/vma_uwrite_cow_production.o: $(TESTS_DIR)/kernel/memory/vma.c \
		$(TESTS_DIR)/kernel/include/memory/vma.h \
		$(TEST_MOCK)/uwrite_cow/uwrite_cow_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(UWRITE_COW_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/uwrite_cow_stubs.o: $(TEST_MOCK)/uwrite_cow/uwrite_cow_stubs.c \
		$(TEST_MOCK)/uwrite_cow/uwrite_cow_stubs.h \
		$(TEST_MOCK)/uwrite_cow/uwrite_cow_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(UWRITE_COW_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_user_write_cow.o: $(TEST_CASES)/test_user_write_cow.c \
		$(TEST_MOCK)/uwrite_cow/uwrite_cow_stubs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(UWRITE_COW_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_user_write_cow.elf: $(TEST_BLD)/test_user_write_cow.o \
		$(TEST_BLD)/uaccess_uwrite_cow_production.o \
		$(TEST_BLD)/vma_uwrite_cow_production.o \
		$(TEST_BLD)/uwrite_cow_stubs.o
	$(HOST_CC) -o $@ $^

# PMM boot layout calculator
PMM_BOOT_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC) -I$(TEST_INC) \
                        -include $(TEST_MOCK)/test_platform.h

$(TEST_BLD)/pmm_boot_production.o: $(TESTS_DIR)/kernel/memory/pmm_boot.c \
		$(TESTS_DIR)/kernel/include/memory/pmm_boot.h \
		$(TESTS_DIR)/kernel/include/memory/pmm.h \
		$(TESTS_DIR)/kernel/include/memory/memory_map.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PMM_BOOT_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_page_table_layout.o: $(TEST_CASES)/test_page_table_layout.c \
		$(TESTS_DIR)/kernel/include/memory/pmm_boot.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PMM_BOOT_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_page_table_layout.elf: $(TEST_BLD)/test_page_table_layout.o \
		$(TEST_BLD)/pmm_boot_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test-page-table-layout
test-page-table-layout: $(TEST_BLD)/test_page_table_layout.elf $(TEST_BLD)/test_page_table_runner.elf
	$(TEST_BLD)/test_page_table_layout.elf
	$(TEST_BLD)/test_page_table_runner.elf

# PMM boot reservation
PMM_RESERVATION_TEST_CFLAGS := $(HOST_CFLAGS) $(PMM_SHADOW_INC) $(KERNEL_INC) \
                               -I$(TEST_INC) $(FRAMEWORK_INC) $(LIBC_INC)

$(TEST_BLD)/test_page_table_reservation.o: $(TEST_CASES)/test_page_table_reservation.c \
		$(TESTS_DIR)/kernel/include/memory/pmm.h \
		$(TESTS_DIR)/kernel/include/memory/pmm_arch.h \
		$(TESTS_DIR)/kernel/include/memory/pmm_boot.h \
		$(TESTS_DIR)/kernel/include/memory/memory_map.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PMM_RESERVATION_TEST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_page_table_reservation.elf: $(TEST_BLD)/test_page_table_reservation.o \
		$(TEST_BLD)/pmm_production.o \
		$(TEST_BLD)/pmm_arch_default.o \
		$(TEST_BLD)/pmm_boot_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test-page-table-reservation
test-page-table-reservation: $(TEST_BLD)/test_page_table_reservation.elf
	$<

# Early arena
EARLY_ARENA_HOST_CFLAGS := $(HOST_CFLAGS) $(PMM_SHADOW_INC) $(KERNEL_INC) \
                           -I$(TEST_INC) $(LIBC_INC)

$(TEST_BLD)/early_arena_production.o: $(TESTS_DIR)/kernel/arch/aarch64/memory/early_arena.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/early_arena.h \
		$(TESTS_DIR)/kernel/include/memory/pmm.h \
		$(TESTS_DIR)/kernel/include/memory/pmm_boot.h \
		$(TESTS_DIR)/kernel/include/memory/memory_map.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(EARLY_ARENA_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_early_arena.o: $(TEST_CASES)/test_early_arena.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/early_arena.h \
		$(TESTS_DIR)/kernel/include/memory/pmm.h \
		$(TESTS_DIR)/kernel/include/memory/pmm_boot.h \
		$(TESTS_DIR)/kernel/include/memory/memory_map.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(EARLY_ARENA_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_early_arena.elf: $(TEST_BLD)/test_early_arena.o \
		$(TEST_BLD)/early_arena_production.o \
		$(TEST_BLD)/pmm_production.o \
		$(TEST_BLD)/pmm_arch_default.o \
		$(TEST_BLD)/pmm_boot_production.o
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/early_arena_exhaust_production.o: $(TESTS_DIR)/kernel/arch/aarch64/memory/early_arena.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(EARLY_ARENA_HOST_CFLAGS) -DAARCH64_EARLY_ARENA_EXHAUST=1 -c $< -o $@

$(TEST_BLD)/test_early_arena_exhaust.o: $(TEST_CASES)/test_early_arena_exhaust.c \
		$(TEST_INC)/page_table_test_runner.h \
		$(TESTS_DIR)/kernel/include/memory/slab.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(EARLY_ARENA_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_early_arena_exhaust.elf: $(TEST_BLD)/test_early_arena_exhaust.o $(TEST_BLD)/early_arena_exhaust_production.o $(TEST_BLD)/pmm_boot_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test-early-arena
test-early-arena: $(TEST_BLD)/test_early_arena.elf $(TEST_BLD)/test_early_arena_exhaust.elf \
    $(TEST_BLD)/test_arena_layout_chain.elf \
    $(TEST_BLD)/test_arena_2gib_boundary.elf \
    $(TEST_BLD)/test_arena_candidate_slab_bytes.elf \
    $(TEST_BLD)/test_arena_table_base_shift.elf
	$(TEST_BLD)/test_early_arena.elf
	$(TEST_BLD)/test_early_arena_exhaust.elf
	$(TEST_BLD)/test_arena_layout_chain.elf
	$(TEST_BLD)/test_arena_2gib_boundary.elf
	$(TEST_BLD)/test_arena_candidate_slab_bytes.elf
	$(TEST_BLD)/test_arena_table_base_shift.elf

$(TEST_BLD)/test_arena_layout_chain.o: $(TEST_CASES)/test_arena_layout_chain.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/early_arena.h \
		$(TESTS_DIR)/kernel/include/memory/pmm.h \
		$(TESTS_DIR)/kernel/include/memory/pmm_boot.h \
		$(TESTS_DIR)/kernel/include/memory/memory_map.h \
		$(TESTS_DIR)/kernel/include/memory/slab.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(EARLY_ARENA_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_arena_layout_chain.elf: $(TEST_BLD)/test_arena_layout_chain.o \
		$(TEST_BLD)/early_arena_production.o \
		$(TEST_BLD)/pmm_production.o \
		$(TEST_BLD)/pmm_arch_default.o \
		$(TEST_BLD)/pmm_boot_production.o
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_arena_2gib_boundary.o: $(TEST_CASES)/test_arena_2gib_boundary.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/early_arena.h \
		$(TESTS_DIR)/kernel/include/memory/pmm.h \
		$(TESTS_DIR)/kernel/include/memory/pmm_boot.h \
		$(TESTS_DIR)/kernel/include/memory/memory_map.h \
		$(TESTS_DIR)/kernel/include/memory/slab.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(EARLY_ARENA_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_arena_2gib_boundary.elf: $(TEST_BLD)/test_arena_2gib_boundary.o \
		$(TEST_BLD)/early_arena_production.o \
		$(TEST_BLD)/pmm_production.o \
		$(TEST_BLD)/pmm_arch_default.o \
		$(TEST_BLD)/pmm_boot_production.o
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_arena_candidate_slab_bytes.o: $(TEST_CASES)/test_arena_candidate_slab_bytes.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/early_arena.h \
		$(TESTS_DIR)/kernel/include/memory/pmm.h \
		$(TESTS_DIR)/kernel/include/memory/pmm_boot.h \
		$(TESTS_DIR)/kernel/include/memory/memory_map.h \
		$(TESTS_DIR)/kernel/include/memory/slab.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(EARLY_ARENA_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_arena_candidate_slab_bytes.elf: $(TEST_BLD)/test_arena_candidate_slab_bytes.o \
		$(TEST_BLD)/early_arena_production.o \
		$(TEST_BLD)/pmm_production.o \
		$(TEST_BLD)/pmm_arch_default.o \
		$(TEST_BLD)/pmm_boot_production.o
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_arena_table_base_shift.o: $(TEST_CASES)/test_arena_table_base_shift.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/early_arena.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/runtime_tree.h \
		$(TESTS_DIR)/kernel/include/memory/pmm.h \
		$(TESTS_DIR)/kernel/include/memory/pmm_boot.h \
		$(TESTS_DIR)/kernel/include/memory/memory_map.h \
		$(TESTS_DIR)/kernel/include/memory/slab.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(EARLY_ARENA_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_arena_table_base_shift.elf: $(TEST_BLD)/test_arena_table_base_shift.o \
		$(TEST_BLD)/early_arena_production.o \
		$(TEST_BLD)/pmm_production.o \
		$(TEST_BLD)/pmm_arch_default.o \
		$(TEST_BLD)/pmm_boot_production.o \
		$(TEST_BLD)/runtime_tree_production.o
	$(HOST_CC) -o $@ $^

# Runtime page-table tree builder
RUNTIME_TREE_HOST_CFLAGS := $(HOST_CFLAGS) $(PMM_SHADOW_INC) $(KERNEL_INC) \
                           -I$(TEST_INC)

$(TEST_BLD)/runtime_tree_production.o: \
		$(TESTS_DIR)/kernel/arch/aarch64/memory/runtime_tree.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/runtime_tree.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/early_arena.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/page_table.h \
		$(TESTS_DIR)/kernel/include/memory/memory_map.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(RUNTIME_TREE_HOST_CFLAGS) -Wframe-larger-than=1024 -Werror=frame-larger-than -c $< -o $@

$(TEST_BLD)/test_page_table_tree.o: $(TEST_CASES)/test_page_table_tree.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/runtime_tree.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/early_arena.h \
		$(TESTS_DIR)/kernel/include/memory/memory_map.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(RUNTIME_TREE_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_page_table_tree.elf: $(TEST_BLD)/test_page_table_tree.o \
		$(TEST_BLD)/runtime_tree_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test-page-table-tree
test-page-table-tree: $(TEST_BLD)/test_page_table_tree.elf
	$<

# Boot direct map contract (x86)
PT_CONTRACT_HOST_CFLAGS := $(HOST_CFLAGS) -DOS01_HOST_TEST -I$(TEST_MOCK)/page_table_include $(KERNEL_INC) -I$(TEST_INC)

$(TEST_BLD)/boot_direct_map_production.o: \
		$(TESTS_DIR)/kernel/arch/x86_64/memory/boot_direct_map.c \
		$(TESTS_DIR)/kernel/include/arch/boot_memory.h \
		$(TESTS_DIR)/kernel/include/memory/memory.h \
		$(TESTS_DIR)/kernel/include/memory/memory_map.h \
		$(TESTS_DIR)/kernel/include/memory/pmm.h \
		$(TESTS_DIR)/kernel/include/memory/vmm.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PT_CONTRACT_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_page_table_contract.o: $(TEST_CASES)/test_page_table_contract.c \
		$(TESTS_DIR)/kernel/include/arch/boot_memory.h \
		$(TESTS_DIR)/kernel/include/memory/memory.h \
		$(TESTS_DIR)/kernel/include/memory/memory_map.h \
		$(TESTS_DIR)/kernel/include/memory/pmm.h \
		$(TESTS_DIR)/kernel/include/memory/vmm.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PT_CONTRACT_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_page_table_contract.elf: $(TEST_BLD)/test_page_table_contract.o \
		$(TEST_BLD)/boot_direct_map_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test-page-table-contract-x86
test-page-table-contract-x86: $(TEST_BLD)/test_page_table_contract.elf $(TEST_BLD)/test_page_table_vmm.elf \
    $(TEST_BLD)/test_page_table_install.elf $(TEST_BLD)/test_page_table_cleanup.elf \
    $(TEST_BLD)/test_page_table_publish.elf
	$(TEST_BLD)/test_page_table_contract.elf
	$(TEST_BLD)/test_page_table_vmm.elf

# Page table VMM contract
$(TEST_BLD)/page_table_vmm_production.o: $(TESTS_DIR)/kernel/memory/vmm.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PT_CONTRACT_HOST_CFLAGS) $(LIBC_INC) -ffunction-sections -fdata-sections -c $< -o $@

$(TEST_BLD)/test_page_table_vmm.o: $(TEST_CASES)/test_page_table_vmm.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PT_CONTRACT_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_page_table_vmm.elf: $(TEST_BLD)/test_page_table_vmm.o $(TEST_BLD)/page_table_vmm_production.o
	$(HOST_CC) -Wl,--gc-sections -Wl,--wrap=calloc -o $@ $^

# aarch64 page table install & root publish
PT_A64_CFLAGS := $(HOST_CFLAGS) -DOS01_HOST_TEST -DOS01_SELFTEST=1 -I$(TEST_MOCK)/page_table_aarch64_include -I$(TEST_MOCK)/page_table_include $(KERNEL_INC)

$(TEST_BLD)/page_table_install_production.o: $(TESTS_DIR)/kernel/arch/aarch64/memory/boot_direct_map.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PT_A64_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_page_table_install.o: $(TEST_CASES)/test_page_table_install.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PT_A64_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_page_table_install.elf: $(TEST_BLD)/test_page_table_install.o $(TEST_BLD)/page_table_install_production.o $(TEST_BLD)/runtime_tree_production.o $(TEST_BLD)/vmm_gate_production.o $(TEST_BLD)/vmm_gate_test_stubs.o
	$(HOST_CC) -o $@ $^

.PHONY: test-page-table-install
test-page-table-install: $(TEST_BLD)/test_page_table_install.elf
	$(TEST_BLD)/test_page_table_install.elf
	$(TEST_BLD)/test_page_table_cleanup.elf

$(TEST_BLD)/test_aarch64_pt_root_publish.o: $(TEST_CASES)/test_aarch64_pt_root_publish.c \
		$(TESTS_DIR)/kernel/arch/aarch64/memory/boot_direct_map.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PT_A64_CFLAGS) -DOS01_KERNEL_SRC=\"$(TESTS_DIR)\" \
		$(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_aarch64_pt_root_publish.elf: $(TEST_BLD)/test_aarch64_pt_root_publish.o \
		$(TEST_BLD)/page_table_install_production.o \
		$(TEST_BLD)/runtime_tree_production.o \
		$(TEST_BLD)/vmm_gate_production.o \
		$(TEST_BLD)/vmm_gate_test_stubs.o
	$(HOST_CC) -o $@ $^

.PHONY: test-aarch64-pt-root-publish
test-aarch64-pt-root-publish: $(TEST_BLD)/test_aarch64_pt_root_publish.elf
	$(TEST_BLD)/test_aarch64_pt_root_publish.elf

# Page table cleanup
$(TEST_BLD)/page_table_smoke_production.o: $(TESTS_DIR)/kernel/arch/aarch64/memory/page_table_smoke.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PT_A64_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_page_table_cleanup.o: $(TEST_CASES)/test_page_table_cleanup.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PT_A64_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_page_table_cleanup.elf: $(TEST_BLD)/test_page_table_cleanup.o $(TEST_BLD)/page_table_smoke_production.o
	$(HOST_CC) -no-pie -o $@ $^

# Page table publish
$(TEST_BLD)/page_table_selftest_production.o: $(TESTS_DIR)/kernel/arch/aarch64/memory/page_table_selftest.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -DOS01_HOST_TEST -DOS01_SELFTEST=1 $(PMM_SHADOW_INC) -I$(TEST_MOCK)/page_table_aarch64_include -I$(TEST_MOCK)/page_table_include $(KERNEL_INC) -ffunction-sections -fdata-sections -c $< -o $@

$(TEST_BLD)/test_page_table_publish.o: $(TEST_CASES)/test_page_table_publish.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PT_A64_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_page_table_publish.elf: $(TEST_BLD)/test_page_table_publish.o $(TEST_BLD)/page_table_selftest_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

.PHONY: test-page-table-publish
test-page-table-publish: $(TEST_BLD)/test_page_table_publish.elf
	$<

# VMM audit gate
VMM_AUDIT_CFLAGS := $(HOST_CFLAGS) -DOS01_KERNEL_SRC=\"$(TESTS_DIR)\" \
                    -I$(TEST_INC) -include $(TEST_MOCK)/test_platform.h

$(TEST_BLD)/test_vmm_caller_audit.o: \
		$(TEST_CASES)/test_vmm_caller_audit.c \
		$(TESTS_DIR)/kernel/memory/slab.c \
		$(TESTS_DIR)/kernel/memory/vmm.c \
		$(TESTS_DIR)/kernel/memory/tlb.c \
		$(TESTS_DIR)/kernel/arch/aarch64/memory/early_arena.c \
		$(TESTS_DIR)/kernel/arch/aarch64/memory/page_table.c \
		$(TESTS_DIR)/kernel/arch/aarch64/intr/ipi.c \
		$(TESTS_DIR)/kernel/Makefile
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VMM_AUDIT_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_vmm_caller_audit.elf: \
		$(TEST_BLD)/test_vmm_caller_audit.o
	$(HOST_CC) -o $@ $^

.PHONY: test_vmm_caller_audit
test_vmm_caller_audit: $(TEST_BLD)/test_vmm_caller_audit.elf
	$<

# Public VMM header contract
$(TEST_BLD)/test_vmm_semantic_header.o: \
		$(TEST_CASES)/test_vmm_semantic_header.c \
		$(TESTS_DIR)/kernel/include/memory/vmm.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/pte.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_INC) $(KERNEL_INC) \
		-c $< -o $@

$(TEST_BLD)/test_vmm_semantic_header.elf: $(TEST_BLD)/test_vmm_semantic_header.o
	$(HOST_CC) -o $@ $^

.PHONY: test_vmm_semantic_header
test_vmm_semantic_header: $(TEST_BLD)/test_vmm_semantic_header.elf
	$<

# x86 VMM backend
$(TEST_BLD)/x86_vmm_backend_production.o: $(TESTS_DIR)/kernel/memory/vmm.c \
		$(TESTS_DIR)/kernel/include/memory/vmm.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/pte.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PT_CONTRACT_HOST_CFLAGS) $(LIBC_INC) \
		-ffunction-sections -fdata-sections -c $< -o $@

$(TEST_BLD)/test_x86_vmm_backend.o: $(TEST_CASES)/test_x86_vmm_backend.c \
		$(TESTS_DIR)/kernel/include/memory/vmm.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/pte.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PT_CONTRACT_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) \
		-c $< -o $@

$(TEST_BLD)/test_x86_vmm_backend.elf: \
		$(TEST_BLD)/test_x86_vmm_backend.o \
		$(TEST_BLD)/x86_vmm_backend_production.o
	$(HOST_CC) -Wl,--gc-sections -Wl,--wrap=calloc -o $@ $^

# aarch64 page table primitives & backend
AARCH64_PT_SW_HOST_CFLAGS := $(HOST_CFLAGS) -DOS01_SELFTEST=1 \
                              -I$(TEST_MOCK)/page_table_aarch64_include \
                              $(KERNEL_INC) $(LIBC_INC)

$(TEST_BLD)/aarch64_pt_production.o: \
		$(TESTS_DIR)/kernel/arch/aarch64/memory/page_table.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/page_table.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/vmm_backend.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/vmm_gate.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/boot_direct_map.h \
		$(TESTS_DIR)/kernel/include/memory/pmm.h \
		$(TESTS_DIR)/kernel/include/arch/mmu.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(AARCH64_PT_SW_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_aarch64_pt_software_bits.o: \
		$(TEST_CASES)/test_aarch64_pt_software_bits.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/page_table.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/vmm_backend.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(AARCH64_PT_SW_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_aarch64_pt_software_bits.elf: \
		$(TEST_BLD)/test_aarch64_pt_software_bits.o \
		$(TEST_BLD)/aarch64_pt_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test_aarch64_pt_software_bits
test_aarch64_pt_software_bits: $(TEST_BLD)/test_aarch64_pt_software_bits.elf
	$<

$(TEST_BLD)/test_aarch64_pt_locks.o: $(TEST_CASES)/test_aarch64_pt_locks.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/page_table.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/vmm_backend.h \
		$(TESTS_DIR)/kernel/include/arch/spinlock.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(AARCH64_PT_SW_HOST_CFLAGS) \
		-DOS01_KERNEL_SRC=\"$(TESTS_DIR)\" \
		$(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_aarch64_pt_locks.elf: \
		$(TEST_BLD)/test_aarch64_pt_locks.o \
		$(TEST_BLD)/aarch64_pt_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test_aarch64_pt_locks
test_aarch64_pt_locks: $(TEST_BLD)/test_aarch64_pt_locks.elf
	$<

$(TEST_BLD)/test_aarch64_pt_2m_block.o: $(TEST_CASES)/test_aarch64_pt_2m_block.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/page_table.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/vmm_backend.h \
		$(TESTS_DIR)/kernel/include/arch/spinlock.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(AARCH64_PT_SW_HOST_CFLAGS) \
		-DOS01_KERNEL_SRC=\"$(TESTS_DIR)\" \
		$(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_aarch64_pt_2m_block.elf: \
		$(TEST_BLD)/test_aarch64_pt_2m_block.o \
		$(TEST_BLD)/aarch64_pt_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test_aarch64_pt_2m_block
test_aarch64_pt_2m_block: $(TEST_BLD)/test_aarch64_pt_2m_block.elf
	$<

$(TEST_BLD)/vmm_backend_production.o: \
		$(TESTS_DIR)/kernel/arch/aarch64/memory/vmm_backend.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/page_table.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/vmm_backend.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/vmm_gate.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/boot_direct_map.h \
		$(TESTS_DIR)/kernel/include/memory/vmm.h \
		$(TESTS_DIR)/kernel/include/memory/pmm.h \
		$(TESTS_DIR)/kernel/include/arch/mmu.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(AARCH64_PT_SW_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_aarch64_backend_4k.o: $(TEST_CASES)/test_aarch64_backend_4k.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/page_table.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/vmm_backend.h \
		$(TESTS_DIR)/kernel/include/memory/vmm.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(AARCH64_PT_SW_HOST_CFLAGS) \
		-DOS01_KERNEL_SRC=\"$(TESTS_DIR)\" \
		$(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_aarch64_backend_4k.elf: \
		$(TEST_BLD)/test_aarch64_backend_4k.o \
		$(TEST_BLD)/aarch64_pt_production.o \
		$(TEST_BLD)/vmm_backend_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test_aarch64_backend_4k
test_aarch64_backend_4k: $(TEST_BLD)/test_aarch64_backend_4k.elf
	$<

$(TEST_BLD)/test_vmm_replace_protocol.o: $(TEST_CASES)/test_vmm_replace_protocol.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/page_table.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/vmm_backend.h \
		$(TESTS_DIR)/kernel/include/memory/vmm.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(AARCH64_PT_SW_HOST_CFLAGS) \
		-DOS01_KERNEL_SRC=\"$(TESTS_DIR)\" \
		$(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_vmm_replace_protocol.elf: \
		$(TEST_BLD)/test_vmm_replace_protocol.o \
		$(TEST_BLD)/aarch64_pt_production.o \
		$(TEST_BLD)/vmm_backend_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test_vmm_replace_protocol
test_vmm_replace_protocol: $(TEST_BLD)/test_vmm_replace_protocol.elf
	$<

$(TEST_BLD)/test_vmm_prot_none.o: $(TEST_CASES)/test_vmm_prot_none.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/page_table.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/vmm_backend.h \
		$(TESTS_DIR)/kernel/include/memory/vmm.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(AARCH64_PT_SW_HOST_CFLAGS) \
		-DOS01_KERNEL_SRC=\"$(TESTS_DIR)\" \
		$(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_vmm_prot_none.elf: \
		$(TEST_BLD)/test_vmm_prot_none.o \
		$(TEST_BLD)/aarch64_pt_production.o \
		$(TEST_BLD)/vmm_backend_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test_vmm_prot_none
test_vmm_prot_none: $(TEST_BLD)/test_vmm_prot_none.elf
	$<

# TLB shootdown infrastructure & ack wraparound
TLB_SD_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC)
TLB_SD_PROD_CFLAGS := $(TLB_SD_HOST_CFLAGS) \
                      -include $(TEST_MOCK)/tlb_test_runtime.h

$(TEST_BLD)/tlb_production.o: $(TESTS_DIR)/kernel/memory/tlb.c \
		$(TEST_MOCK)/tlb_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(TLB_SD_PROD_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_vmm_ack_wraparound.o: \
		$(TEST_CASES)/test_vmm_ack_wraparound.c \
		$(TEST_MOCK)/tlb_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(TLB_SD_HOST_CFLAGS) -I$(TEST_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_vmm_ack_wraparound.elf: \
		$(TEST_BLD)/test_vmm_ack_wraparound.o \
		$(TEST_BLD)/tlb_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test_vmm_ack_wraparound
test_vmm_ack_wraparound: $(TEST_BLD)/test_vmm_ack_wraparound.elf
	$<

# VMM gate foundational primitives
VMM_GATE_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC)
VMM_GATE_PROD_CFLAGS := $(VMM_GATE_HOST_CFLAGS) \
                        -include $(TEST_MOCK)/vmm_gate_test_runtime.h

$(TEST_BLD)/vmm_gate_production.o: $(TESTS_DIR)/kernel/arch/aarch64/memory/vmm_gate.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/vmm_gate.h \
		$(TEST_MOCK)/vmm_gate_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VMM_GATE_PROD_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_foundational_primitives.o: $(TEST_CASES)/test_foundational_primitives.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/vmm_gate.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/dtb.h \
		$(TEST_MOCK)/vmm_gate_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VMM_GATE_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_foundational_primitives.elf: $(TEST_BLD)/test_foundational_primitives.o \
		$(TEST_BLD)/vmm_gate_production.o
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/vmm_gate_test_stubs.o: $(TEST_MOCK)/vmm_gate_test_stubs.c \
		$(TEST_MOCK)/vmm_gate_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VMM_GATE_HOST_CFLAGS) -I$(TEST_MOCK) -c $< -o $@

.PHONY: test_foundational_primitives
test_foundational_primitives: $(TEST_BLD)/test_foundational_primitives.elf
	$<

# aarch64 pt split
$(TEST_BLD)/test_aarch64_pt_split.o: $(TEST_CASES)/test_aarch64_pt_split.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/page_table.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/vmm_backend.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/vmm_gate.h \
		$(TESTS_DIR)/kernel/include/arch/spinlock.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(AARCH64_PT_SW_HOST_CFLAGS) \
		-DOS01_KERNEL_SRC=\"$(TESTS_DIR)\" \
		$(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_aarch64_pt_split.elf: $(TEST_BLD)/test_aarch64_pt_split.o \
		$(TEST_BLD)/aarch64_pt_production.o \
		$(TEST_BLD)/vmm_gate_production.o \
		$(TEST_BLD)/vmm_gate_test_stubs.o
	$(HOST_CC) -o $@ $^

.PHONY: test-aarch64-pt-split
test-aarch64-pt-split: $(TEST_BLD)/test_aarch64_pt_split.elf
	$(TEST_BLD)/test_aarch64_pt_split.elf

# aarch64 arch_vmm_init
$(TEST_BLD)/test_aarch64_arch_vmm_init.o: \
		$(TEST_CASES)/test_aarch64_arch_vmm_init.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/page_table.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/vmm_backend.h \
		$(TESTS_DIR)/kernel/include/memory/vmm.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(AARCH64_PT_SW_HOST_CFLAGS) \
		-DOS01_KERNEL_SRC=\"$(TESTS_DIR)\" \
		$(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_aarch64_arch_vmm_init.elf: \
		$(TEST_BLD)/test_aarch64_arch_vmm_init.o \
		$(TEST_BLD)/aarch64_pt_production.o \
		$(TEST_BLD)/vmm_backend_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test_aarch64_arch_vmm_init
test_aarch64_arch_vmm_init: $(TEST_BLD)/test_aarch64_arch_vmm_init.elf
	$<

# Shootdown probe logic
SHOOTDOWN_PROBE_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC) \
                        -I$(TEST_MOCK)/page_table_aarch64_include \
                        -DOS01_KERNEL_SRC=\"$(TESTS_DIR)\"

$(TEST_BLD)/test_aarch64_scratch_probe_logic.o: \
		$(TEST_CASES)/test_aarch64_scratch_probe_logic.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/boot/shootdown_probe.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SHOOTDOWN_PROBE_HOST_CFLAGS) -I$(TEST_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_aarch64_scratch_probe_logic.elf: \
		$(TEST_BLD)/test_aarch64_scratch_probe_logic.o
	$(HOST_CC) -o $@ $^

.PHONY: test_aarch64_scratch_probe_logic
test_aarch64_scratch_probe_logic: $(TEST_BLD)/test_aarch64_scratch_probe_logic.elf
	$<

# Slab idempotent reservation & x86 count
SLAB_SHADOW_INC := -I$(TEST_MOCK)/slab_include

$(TEST_BLD)/slab_production.o: $(TESTS_DIR)/kernel/memory/slab.c \
		$(TESTS_DIR)/kernel/include/memory/slab.h \
		$(TESTS_DIR)/kernel/include/memory/pmm.h \
		$(TEST_MOCK)/slab_include/arch/spinlock.h \
		$(TEST_MOCK)/slab_include/arch/irq.h \
		$(TEST_MOCK)/slab_include/percpu/percpu.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(PMM_SHADOW_INC) $(SLAB_SHADOW_INC) $(KERNEL_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_slab_idempotent_reservation.o: $(TEST_CASES)/test_slab_idempotent_reservation.c \
		$(TESTS_DIR)/kernel/include/memory/pmm.h \
		$(TESTS_DIR)/kernel/include/memory/slab.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(PMM_SHADOW_INC) $(SLAB_SHADOW_INC) $(KERNEL_INC) \
		-I$(TEST_INC) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/slab_color_stub.o: $(TEST_MOCK)/slab_color_stub.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_slab_idempotent_reservation.elf: $(TEST_BLD)/test_slab_idempotent_reservation.o \
		$(TEST_BLD)/slab_production.o \
		$(TEST_BLD)/slab_color_stub.o \
		$(TEST_BLD)/pmm_production.o \
		$(TEST_BLD)/pmm_arch_default.o \
		$(TEST_BLD)/pmm_boot_production.o \
		$(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_slab_basic_x86_count.o: $(TEST_CASES)/test_slab_basic_x86_count.c \
		$(TESTS_DIR)/kernel/include/memory/pmm.h \
		$(TESTS_DIR)/kernel/include/memory/slab.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(PMM_SHADOW_INC) $(SLAB_SHADOW_INC) $(KERNEL_INC) \
		-I$(TEST_INC) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_slab_basic_x86_count.elf: $(TEST_BLD)/test_slab_basic_x86_count.o \
		$(TEST_BLD)/slab_production.o \
		$(TEST_BLD)/slab_color_stub.o \
		$(TEST_BLD)/pmm_production.o \
		$(TEST_BLD)/pmm_arch_default.o \
		$(TEST_BLD)/pmm_boot_production.o \
		$(LIBC_OBJS)
	$(HOST_CC) -o $@ $^
