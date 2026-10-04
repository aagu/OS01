# ── Profile-only contract ───────────────────────────────────
# The root Makefile is the only supported entry; hosttests/Makefile
# requires OS01_PROFILE_FILE at parse time (root `test` invokes it
# via os01_submake). Host tests are an OS01 build: every object and
# binary is written ONLY under the selected profile's host-test dir
# (HOST_TEST_BUILD_DIR) and profile clean removes it. Host compilation
# stays host-native (HOST_CC, no --target, no sysroot, no kernel link
# flags) but obeys profile output isolation.
ifeq ($(strip $(OS01_PROFILE_FILE)),)
$(error invoke from the repository root: make PROFILE=x86_64-clang <target>)
endif
ifeq ($(wildcard $(OS01_PROFILE_FILE)),)
$(error OS01_PROFILE_FILE='$(OS01_PROFILE_FILE)' does not exist; invoke from the repository root)
endif
include $(OS01_PROFILE_FILE)

TESTS_DIR := $(realpath ..)
TEST_SRC  := $(TESTS_DIR)/hosttests
TEST_BLD := $(HOST_TEST_BUILD_DIR)
TEST_MOCK := $(TEST_SRC)/mock
TEST_INC  := $(TEST_SRC)/include
TEST_CASES := $(TEST_SRC)/cases

HOST_CC   ?= clang

KERNEL_INC := -I$(TESTS_DIR)/kernel/include
LIBC_INC := -I$(TESTS_DIR)/libc/include
FRAMEWORK_INC := -I$(TEST_INC) -include $(TEST_MOCK)/test_platform.h

HOST_CFLAGS := -g -O0 -Wall -Wextra -Wno-unused-parameter \
               -Wno-address-of-packed-member -Wno-gnu-statement-expression \
               -Wno-macro-redefined -Wno-dangling-else \
               -Wno-non-literal-null-conversion -Wno-incompatible-library-redeclaration

POLL_HOST_CFLAGS := $(HOST_CFLAGS) -ffunction-sections -fdata-sections \
    -Wno-unused-function -D__SPINLOCK_H__ -DOS01_HOST_TEST \
    -include $(TEST_MOCK)/poll_test_runtime.h \
    -I$(TEST_MOCK)/include $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)

SYNC_HOST_CFLAGS := $(HOST_CFLAGS) -I$(TEST_INC) $(KERNEL_INC)

LIBC_SRC := $(TESTS_DIR)/libc

LIBC_OBJS := \
    $(TEST_BLD)/libc_memset.o \
    $(TEST_BLD)/libc_memcpy.o \
    $(TEST_BLD)/libc_memmove.o \
    $(TEST_BLD)/libc_strlen.o \
    $(TEST_BLD)/libc_strcmp.o \
    $(TEST_BLD)/libc_strncmp.o \
    $(TEST_BLD)/libc_strcpy.o \
    $(TEST_BLD)/libc_strdup.o \
    $(TEST_BLD)/libc_vsprintf.o \
    $(TEST_BLD)/libc_vsnprintf.o \
    $(TEST_BLD)/libc_sprintf.o \
    $(TEST_BLD)/libc_getopt.o \
    $(TEST_BLD)/libc_list.o \
    $(TEST_BLD)/libc_floatconv.o

MOCK_OBJS := $(TEST_BLD)/mock_kernel.o

TEST_BINS := \
    $(TEST_BLD)/test_libc_string.elf \
    $(TEST_BLD)/test_libc_list.elf \
    $(TEST_BLD)/test_libc_vsprintf.elf \
    $(TEST_BLD)/test_libc_fflush.elf \
    $(TEST_BLD)/test_libc_stdio_registry.elf \
    $(TEST_BLD)/test_libc_fread_fwrite_validate.elf \
    $(TEST_BLD)/test_libc_stdio_registry.elf \
    $(TEST_BLD)/test_libc_getopt.elf \
    $(TEST_BLD)/test_vfs_basic.elf \
    $(TEST_BLD)/test_fat32_basic.elf \
    $(TEST_BLD)/test_pmm_basic.elf \
    $(TEST_BLD)/test_pmm_ram_rel_index.elf \
    $(TEST_BLD)/test_slab_basic.elf \
    $(TEST_BLD)/test_slab_layout_compute.elf \
    $(TEST_BLD)/test_slab_lock_path.elf \
    $(TEST_BLD)/test_elf_validate.elf \
    $(TEST_BLD)/test_wait_basic.elf \
    $(TEST_BLD)/test_canon.elf \
    $(TEST_BLD)/test_terminal_core.elf \
    $(TEST_BLD)/test_tetris_logic.elf \
    $(TEST_BLD)/test_sync_primitives.elf \
    $(TEST_BLD)/test_poll_requested.elf \
    $(TEST_BLD)/test_poll_waitqueue_handshake.elf \
    $(TEST_BLD)/test_bootinfo_abi.elf \
    $(TEST_BLD)/test_runtime_udivti3.elf \
    $(TEST_BLD)/test_lwip_rand.elf \
    $(TEST_BLD)/test_gic_driver.elf \
    $(TEST_BLD)/test_aarch64_ipi_broadcast.elf \
    $(TEST_BLD)/test_aarch64_gic_target_probe.elf \
    $(TEST_BLD)/test_ap_work_protocol.elf \
    $(TEST_BLD)/test_foundational_primitives.elf \
    $(TEST_BLD)/test_gic_marker_lines.elf \
    $(TEST_BLD)/test_gic_probe.elf \
    $(TEST_BLD)/test_double_state_publish.elf \
    $(TEST_BLD)/test_clocksource.elf \
    $(TEST_BLD)/test_i8042_demux.elf \
    $(TEST_BLD)/test_mouse_init.elf \
    $(TEST_BLD)/test_mouse_proto.elf \
    $(TEST_BLD)/test_gfx_file_lifecycle.elf \
    $(TEST_BLD)/test_gfx_device.elf \
    $(TEST_BLD)/test_gfx_client.elf \
    $(TEST_BLD)/test_gfx_primitives.elf \
    $(TEST_BLD)/test_aarch64_sync_fault.elf \
    $(TEST_BLD)/test_elf_layout.elf \
    $(TEST_BLD)/test_elf_load_4k.elf \
    $(TEST_BLD)/test_process_image_lifecycle.elf \
    $(TEST_BLD)/test_brk_pages.elf \
    $(TEST_BLD)/test_user_protected_ranges.elf \
    $(TEST_BLD)/test_fork_user_map.elf \
    $(TEST_BLD)/test_user_write_cow.elf \
    $(TEST_BLD)/test_m1_runner.elf \
    $(TEST_BLD)/test_m1_layout.elf \
    $(TEST_BLD)/test_m1_reservation.elf \
    $(TEST_BLD)/test_m1_arena.elf $(TEST_BLD)/test_m1_arena_exhaust.elf \
    $(TEST_BLD)/test_arena_layout_chain.elf \
    $(TEST_BLD)/test_arena_2gib_boundary.elf \
    $(TEST_BLD)/test_arena_candidate_slab_bytes.elf \
    $(TEST_BLD)/test_arena_table_base_shift.elf \
    $(TEST_BLD)/test_aarch64_color_printk_abi.elf \
    $(TEST_BLD)/test_m1_tree.elf \
    $(TEST_BLD)/test_m1_contract.elf $(TEST_BLD)/test_m1_vmm.elf \
    $(TEST_BLD)/test_m1_install.elf $(TEST_BLD)/test_m1_cleanup.elf \
    $(TEST_BLD)/test_m1_publish.elf \
    $(TEST_BLD)/test_slab_idempotent_reservation.elf \
    $(TEST_BLD)/test_slab_basic_x86_count.elf \
    $(TEST_BLD)/test_percpu_layout.elf \
    $(TEST_BLD)/test_tlb_serial_protocol.elf \
    $(TEST_BLD)/test_x86_ipi_ready_publish.elf \
    $(TEST_BLD)/test_vmm_caller_audit.elf \
    $(TEST_BLD)/test_vmm_semantic_header.elf \
    $(TEST_BLD)/test_x86_vmm_backend.elf \
    $(TEST_BLD)/test_aarch64_pt_software_bits.elf \
    $(TEST_BLD)/test_aarch64_pt_locks.elf \
    $(TEST_BLD)/test_aarch64_pt_2m_block.elf \
    $(TEST_BLD)/test_aarch64_backend_4k.elf

.PHONY: all clean run test-runtime-udivti3 runtime-udivti3-link-probe test_lwip_rand test_gic_driver test_gic_marker_lines test_gic_probe test_clocksource test-libc-fread-fwrite-validate test-libc-stdio-registry test-gfx-client test-gfx-primitives

all: $(TEST_BINS)

run: $(TEST_BINS)
	@echo ""
	@echo "=============================================="
	@echo "  OS01 Host-Based Test Suite"
	@echo "=============================================="
	@failed=0; total=0; \
	for t in $(TEST_BINS); do \
	    name=$$(basename $$t .elf); \
	    echo ""; \
	    echo "======== $$name ========"; \
	    output=$$($$t 2>&1); \
	    rc=$$?; \
	    printf '%s\n' "$$output" | \
	        grep -E "===|Total|Failed|>>>|ALL" --color=never || true; \
	    total=$$((total + 1)); \
	    if [ "$${rc}" -ne 0 ]; then \
	        failed=$$((failed + 1)); \
	        if [ "$${rc}" -ne 1 ]; then \
	            echo "  >>> $$name CRASHED (rc=$$rc) <<<"; \
	        fi; \
	    fi; \
	    echo "  -> exit code: $$rc"; \
	done; \
	echo ""; \
	echo "=============================================="; \
	echo "  Suites: $$total | Failed: $$failed"; \
	echo "=============================================="; \
	exit $$failed

# Runtime fixtures.
$(TEST_BLD)/runtime_udivti3_link_probe.o: $(TEST_SRC)/../runtime/selftest/udivti3_link_probe.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) -ffreestanding -fno-builtin -c $< -o $@

runtime-udivti3-link-probe: $(TEST_BLD)/runtime_udivti3_link_probe.o
	$(HOST_CC) -nostdlib -Wl,-e,_start -o $(TEST_BLD)/runtime_udivti3-link-probe.elf $<

$(TEST_BLD)/test_runtime_udivti3.o: $(TEST_SRC)/../runtime/selftest/test_udivti3.c \
		$(TEST_SRC)/../runtime/selftest/udivti3_vectors.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TESTS_DIR)/runtime/include -I$(TESTS_DIR)/runtime/selftest -c $< -o $@

$(TEST_BLD)/runtime_udivti3_host.o: $(TESTS_DIR)/runtime/builtins/udivti3.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -fno-stack-protector -I$(TESTS_DIR)/runtime/include -c $< -o $@

$(TEST_BLD)/test_runtime_udivti3.elf: $(TEST_BLD)/test_runtime_udivti3.o $(TEST_BLD)/runtime_udivti3_host.o
	$(HOST_CC) -o $@ $^

test-runtime-udivti3: $(TEST_BLD)/test_runtime_udivti3.elf
	$<

# libc compilation
$(TEST_BLD)/libc_memset.o: $(LIBC_SRC)/string/memset.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/libc_memcpy.o: $(LIBC_SRC)/string/memcpy.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/libc_memmove.o: $(LIBC_SRC)/string/memmove.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/libc_strlen.o: $(LIBC_SRC)/string/strlen.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/libc_strcmp.o: $(LIBC_SRC)/string/strcmp.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/libc_strncmp.o: $(LIBC_SRC)/string/strncmp.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/libc_strcpy.o: $(LIBC_SRC)/string/strcpy.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/libc_strdup.o: $(LIBC_SRC)/string/strdup.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/libc_vsprintf.o: $(LIBC_SRC)/stdio/vsprintf.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/libc_vsnprintf.o: $(LIBC_SRC)/stdio/vsnprintf.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/libc_sprintf.o: $(LIBC_SRC)/stdio/sprintf.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/libc_getopt.o: $(LIBC_SRC)/unistd/getopt.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/libc_list.o: $(LIBC_SRC)/list/list.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -fgnu89-inline -c $< -o $@

$(TEST_BLD)/libc_floatconv.o: $(LIBC_SRC)/stdio/floatconv.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/libc_vasprintf.o: $(LIBC_SRC)/stdio/vasprintf.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/libc_printf.o: $(LIBC_SRC)/stdio/printf.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/libc_stdio_file.o: $(LIBC_SRC)/stdio/stdio_file.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

# P1-5 review round 2: stdio_extras.c is the second half of the FILE
# registry's truth source — fileno_unlocked / getc_unlocked /
# putc_unlocked / fgets_unlocked live here. The hosttest must link
# this object so the OS01 implementations (routed through file_to_fd())
# are exercised, NOT the host glibc's. Without this, the linker
# silently resolves the symbols to glibc and the test passes even if
# the OS01 implementations regress to the pre-fix bare-cast behaviour.
$(TEST_BLD)/libc_stdio_extras.o: $(LIBC_SRC)/stdio/stdio_extras.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

# printf/vasprintf/write_all test shim — overrides host write() with a scriptable,
# capturing, real-stdout-forwarding stub.
$(TEST_BLD)/stdio_test_shims.o: $(TEST_MOCK)/stdio_test_shims.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_MOCK)/include -I$(TEST_INC) -c $< -o $@

# Mock stubs — compiled as HOST binary (no custom headers)
$(TEST_BLD)/mock_kernel.o: $(TEST_MOCK)/mock_kernel.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) -g -O0 -c $< -o $@

# Test case compilation — all self-contained (no KERNEL_INC needed)
$(TEST_BLD)/%.o: $(TEST_CASES)/%.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

# getopt test links against libc_getopt.o whose globals (optarg/optind/...) are
# also declared (as tentative defs) by <getopt.h>; build with -fcommon so they merge.
$(TEST_BLD)/test_libc_getopt.o: $(TEST_CASES)/test_libc_getopt.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -fcommon $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

# vsprintf test exercises printf/vasprintf/write_all with the scripted write shim.
$(TEST_BLD)/test_libc_vsprintf.o: $(TEST_CASES)/test_libc_vsprintf.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_MOCK) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

# fflush test exercises printf + fflush + fopen/fclose + write-shim capture.
$(TEST_BLD)/test_libc_fflush.o: $(TEST_CASES)/test_libc_fflush.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_MOCK) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

# P2-8: stdio-registry cap / fdopen / slot-reuse tests — needs the mock
# shim header for shim_write_count() (currently unused but kept for
# symmetry with test_libc_fflush, which exercises the same code paths).
$(TEST_BLD)/test_libc_stdio_registry.o: $(TEST_CASES)/test_libc_stdio_registry.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_MOCK) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

# fread/fwrite stream-validation test — pipe-based fake mini_file_t, no shims.
$(TEST_BLD)/test_libc_fread_fwrite_validate.o: $(TEST_CASES)/test_libc_fread_fwrite_validate.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

# Focused poll test compiles and links the real production paths.
$(TEST_BLD)/test_poll_requested.o: $(TEST_CASES)/test_poll_requested.c
	$(HOST_CC) $(POLL_HOST_CFLAGS) -c $< -o $@

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

# clocksource globals referenced by inlined clocksource_read_ns() in the
# poll/select production objects; stubbed for the host harness.
$(TEST_BLD)/poll_clocksource_stub.o: $(TEST_MOCK)/poll_clocksource_stub.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(POLL_HOST_CFLAGS) -c $< -o $@

# Link rules
$(TEST_BLD)/test_libc_string.elf: $(TEST_BLD)/test_libc_string.o $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_bootinfo_abi.elf: $(TEST_BLD)/test_bootinfo_abi.o
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_bootinfo_abi.o: $(TEST_CASES)/test_bootinfo_abi.c \
		$(TESTS_DIR)/kernel/include/core/bootinfo.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TESTS_DIR)/kernel/include -c $< -o $@

$(TEST_BLD)/test_libc_list.elf: $(TEST_BLD)/test_libc_list.o $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_libc_vsprintf.elf: $(TEST_BLD)/test_libc_vsprintf.o $(LIBC_OBJS) \
		$(TEST_BLD)/libc_vasprintf.o $(TEST_BLD)/libc_printf.o \
		$(TEST_BLD)/libc_stdio_file.o $(TEST_BLD)/stdio_test_shims.o
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_libc_fflush.elf: $(TEST_BLD)/test_libc_fflush.o $(LIBC_OBJS) \
		$(TEST_BLD)/libc_vasprintf.o $(TEST_BLD)/libc_printf.o \
		$(TEST_BLD)/libc_stdio_file.o $(TEST_BLD)/stdio_test_shims.o
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_libc_fread_fwrite_validate.elf: \
		$(TEST_BLD)/test_libc_fread_fwrite_validate.o \
		$(TEST_BLD)/libc_stdio_file.o \
		$(TEST_BLD)/libc_vsprintf.o \
		$(TEST_BLD)/libc_printf.o \
		$(TEST_BLD)/libc_floatconv.o \
		$(TEST_BLD)/stdio_test_shims.o
	$(HOST_CC) -o $@ $^

test-libc-fread-fwrite-validate: $(TEST_BLD)/test_libc_fread_fwrite_validate.elf
	$<

# P2-8: registry-cap / fdopen / slot-reuse / unregistered-stream
# contract tests for the libc FILE layer (AAGU-6 batch).
$(TEST_BLD)/test_libc_stdio_registry.elf: \
		$(TEST_BLD)/test_libc_stdio_registry.o \
		$(TEST_BLD)/libc_stdio_file.o \
		$(TEST_BLD)/libc_stdio_extras.o \
		$(TEST_BLD)/libc_vsprintf.o \
		$(TEST_BLD)/libc_printf.o \
		$(TEST_BLD)/libc_floatconv.o \
		$(TEST_BLD)/stdio_test_shims.o
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_libc_getopt.elf: $(TEST_BLD)/test_libc_getopt.o $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_vfs_basic.elf: $(TEST_BLD)/test_vfs_basic.o $(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_fat32_basic.elf: $(TEST_BLD)/test_fat32_basic.o $(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_pmm_basic.elf: $(TEST_BLD)/test_pmm_basic.o $(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -o $@ $^

# PMM RAM-relative bits_map indexing regression — compiles the REAL
# kernel/memory/pmm.c. pmm.c's spinlock and arch_cpu_halt carry
# privileged inline asm (cli/sti/hlt), so those two headers are
# shadowed by mock headers placed BEFORE kernel/include on the path
# (PMM_SHADOW_INC); everything else resolves to the production
# headers. The test TU puts kernel/include first so <kernel/pmm.h>
# is the real one, not the (stale) test/include shadow.
PMM_SHADOW_INC := -I$(TEST_MOCK)/pmm_include

$(TEST_BLD)/pmm_production.o: $(TESTS_DIR)/kernel/memory/pmm.c \
		$(TEST_MOCK)/pmm_include/arch/spinlock.h \
		$(TEST_MOCK)/pmm_include/arch/cpu.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(PMM_SHADOW_INC) $(KERNEL_INC) -c $< -o $@

# The weak-default dispatcher (pmm_arch_normalize / zone_split) lives in
# kernel/memory/pmm_arch.c; this suite does not exercise it but the linker
# requires the symbols to resolve when compiling the real pmm.c.
$(TEST_BLD)/pmm_arch_default.o: $(TESTS_DIR)/kernel/memory/pmm_arch.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(PMM_SHADOW_INC) $(KERNEL_INC) -c $< -o $@

$(TEST_BLD)/test_pmm_ram_rel_index.o: $(TEST_CASES)/test_pmm_ram_rel_index.c \
		$(TESTS_DIR)/kernel/include/memory/pmm.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -I$(TEST_INC) -c $< -o $@

# pmm.c (production) calls pmm_layout_calculate (kernel/memory/pmm_boot.c).
# Link the calculator object alongside pmm.o so the new layout helper
# resolves; the calculator is also exercised by test_m1_layout.
$(TEST_BLD)/test_pmm_ram_rel_index.elf: $(TEST_BLD)/test_pmm_ram_rel_index.o \
		$(TEST_BLD)/pmm_production.o $(TEST_BLD)/pmm_arch_default.o \
		$(TEST_BLD)/pmm_boot_production.o
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_slab_basic.elf: $(TEST_BLD)/test_slab_basic.o $(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -o $@ $^

# Task 1 Step 1: slab_layout_compute() pure-function test.
# slab.h is now header-inline for the formula (no need to compile slab.c).
# Test uses KERNEL_INC for slab.h's transitive includes (memory/pmm.h).
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

# Task 1 Step 2: source-scan lock path test (v4 fix for v3 review item 12).
$(TEST_BLD)/test_slab_lock_path.o: $(TEST_CASES)/test_slab_lock_path.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -DOS01_KERNEL_SRC=\"$(TESTS_DIR)\" -I$(TEST_INC) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_slab_lock_path.elf: $(TEST_BLD)/test_slab_lock_path.o \
		$(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_elf_validate.elf: $(TEST_BLD)/test_elf_validate.o $(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -o $@ $^

# ELF layout validator (Task 1, user heap/ELF isolation plan): compiles
# the REAL kernel/fs/elf_layout.c so the test exercises the production
# validation contract (not a duplicate formula).  The test TU consumes
# <fs/elf.h> for the type/macro surface and <fs/elf_layout.h> for the
# validator interface; both headers are deliberately light after the
# 2026-10-01 fs/elf.h refactor (heavy types forward-declared) so this
# build needs no shadow-include machinery.
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

# Per-4 KiB ELF loader test (Task 2, user heap/ELF isolation plan):
# compiles the REAL kernel/fs/elf.c against the host harness.  The
# host stubs VFS / PMM / VMM under hosttests/mock/elf_load/ and the
# production elf_load is exercised end-to-end against a real PTE
# state machine (flat user-VA table with present-bit / flags / phys
# per slot — no PAGE_HUGE leaves possible).  Failure injection
# (alloc/read/map) lets the test verify that the loader's single
# rollback owner frees each created leaf exactly once.
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

# Process-image lifecycle test (Task 3, user heap/ELF isolation plan):
# compiles the REAL kernel/memory/vma.c against the host harness so
# mm_init_user_heap / vma_insert / vma_find / vma_free_all / mm_alloc
# are exercised against observable state.  Failure injection on
# kmalloc (-Wl,--wrap=kmalloc) proves mm_init_user_heap returns
# -ENOMEM and leaves mm unchanged when its VMA allocation fails.
#
# Source-level inspection of kernel/sched/task.c covers the staged
# spawn/exec lifecycle invariants (the task.c dependency tree —
# percpu, ipi, fpu, irq, ...) is too heavy to compile in a host
# harness; the actual end-to-end behavior is covered by
# `make OS01_SYSTEST=1 test-qemu SUITE=systest`).
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

# brk-page ownership test (Task 4, user heap/ELF isolation plan):
# compiles the REAL kernel/memory/vma.c against the brk host harness
# so mm_set_brk is exercised against observable state (flat indexed
# PTE table with PAGE_COW support, per-phys COW refcounting, TLB
# flush counter, alloc/free OOM injection).  Source-level inspection
# of kernel/arch/x86_64/intr/trap.c covers the SYS_brk delegation
# and the heap-fault path (trap.c is too heavy to host-link).
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

# Protected-ranges test (Task 5, user heap/ELF isolation plan):
# compiles the REAL kernel/memory/vma.c against the pranges host
# harness and DRIVES do_mmap / do_munmap / do_mprotect through the
# stub `current` (flat indexed PTE table, 4 KiB page pool, counter-
# backed arch_flush_tlb_all).  mm_user_range_protected has a weak
# RED fallback (always false) in pranges_stubs.c so the suite links
# and fails cleanly before the production predicate lands.
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

# Fork / teardown 4 KiB ownership test (Task 6, user heap/ELF
# isolation plan): compiles the REAL kernel/memory/vma.c against
# the fork_user_map host harness (flat indexed PTE table, 4 KiB
# page pool, COW refcounts, vfs_node refcounts) and exercises the
# production fork_vma_copy / vma_free_all / mm_init_user_heap.  Source-
# level inspection of kernel/sched/task.c covers the fork_mm_copy
# staged ordering and the do_fork teardown (task.c is too heavy to
# host-link).
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

# User-write COW privatization test (Task 7, user heap/ELF
# isolation plan): compiles the REAL kernel/memory/uaccess.c +
# vma.c against the uwrite_cow host harness and exercises
# prepare_user_write_range(_locked) + copy_to_user_ft_res.
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

# standalone test (no kernel/libc deps)
$(TEST_BLD)/test_wait_basic.elf: $(TEST_BLD)/test_wait_basic.o
	$(HOST_CC) -o $@ $<

# aarch64 EL1h sync diagnostic helper (Task 1).
# Compiles the REAL header kernel/include/arch/aarch64/sync_fault.h against
# host clang; the helper is a static inline so the test exercises the same
# truth table the kernel's trap.c will.
$(TEST_BLD)/test_aarch64_sync_fault.o: $(TEST_CASES)/test_aarch64_sync_fault.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/sync_fault.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(KERNEL_INC) -c $< -o $@

$(TEST_BLD)/test_aarch64_sync_fault.elf: $(TEST_BLD)/test_aarch64_sync_fault.o
	$(HOST_CC) -o $@ $<

# aarch64 M1 plan Task 1: checked PMM metadata layout calculator.
# Links the REAL kernel/memory/pmm_boot.c against the host TU. The
# production file has no kernel deps beyond <memory/pmm.h> (for the
# Page / Zone struct sizes) and <memory/pmm_boot.h>; both are in
# KERNEL_INC. test_m1_layout.c itself only uses <errno.h> from libc,
# <test_framework.h> from TEST_INC, and the production headers above.
PMM_BOOT_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC) -I$(TEST_INC) \
                        -include $(TEST_MOCK)/test_platform.h

$(TEST_BLD)/pmm_boot_production.o: $(TESTS_DIR)/kernel/memory/pmm_boot.c \
		$(TESTS_DIR)/kernel/include/memory/pmm_boot.h \
		$(TESTS_DIR)/kernel/include/memory/pmm.h \
		$(TESTS_DIR)/kernel/include/memory/memory_map.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PMM_BOOT_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_m1_layout.o: $(TEST_CASES)/test_m1_layout.c \
		$(TESTS_DIR)/kernel/include/memory/pmm_boot.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PMM_BOOT_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_m1_layout.elf: $(TEST_BLD)/test_m1_layout.o \
		$(TEST_BLD)/pmm_boot_production.o
	$(HOST_CC) -o $@ $^

# `make test-m1-host CASE=m1-layout` (root run.mk) reaches this rule
# via the focused dispatch in run.mk.
.PHONY: test-m1-layout
test-m1-layout: $(TEST_BLD)/test_m1_layout.elf $(TEST_BLD)/test_m1_runner.elf
	$(TEST_BLD)/test_m1_layout.elf
	$(TEST_BLD)/test_m1_runner.elf

# aarch64 M1 plan Task 2: range-based boot reservations +
# single-frame exact claim. Links the REAL pmm.c (PMM_SHADOW_INC so
# the production spinlock / arch_cpu_halt resolve to no-op host
# stubs), the REAL pmm_arch.c (weak-default strategy), and the REAL
# pmm_boot.c (layout calculator). The test TU itself does not pull
# in the spinlock header, so it only needs KERNEL_INC + TEST_INC.
#
# `make test-m1-host CASE=m1-reservation` (root run.mk) reaches this
# rule via the focused dispatch in run.mk.
PMM_RESERVATION_TEST_CFLAGS := $(HOST_CFLAGS) $(PMM_SHADOW_INC) $(KERNEL_INC) \
                               -I$(TEST_INC) $(FRAMEWORK_INC) $(LIBC_INC)

$(TEST_BLD)/test_m1_reservation.o: $(TEST_CASES)/test_m1_reservation.c \
		$(TESTS_DIR)/kernel/include/memory/pmm.h \
		$(TESTS_DIR)/kernel/include/memory/pmm_arch.h \
		$(TESTS_DIR)/kernel/include/memory/pmm_boot.h \
		$(TESTS_DIR)/kernel/include/memory/memory_map.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PMM_RESERVATION_TEST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_m1_reservation.elf: $(TEST_BLD)/test_m1_reservation.o \
		$(TEST_BLD)/pmm_production.o \
		$(TEST_BLD)/pmm_arch_default.o \
		$(TEST_BLD)/pmm_boot_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test-m1-reservation
test-m1-reservation: $(TEST_BLD)/test_m1_reservation.elf
	$<

# aarch64 M1 plan Task 3: early arena preflight planner
# (`aarch64_m1_plan` / `aarch64_m1_prepare`). Links the REAL
# kernel/arch/aarch64/memory/early_arena.c against the production
# pmm.c (PMM_SHADOW_INC), the weak-default pmm_arch.c (kept for
# link), and the production pmm_boot.c (calculator). The test TU
# asserts the pure planner: input validation, bucket counting,
# arena selection, layout consistency with the checked
# calculator, and the failure-no-side-effect canary.
#
# The production early_arena.c only pulls <errno.h>, <stddef.h>,
# <stdint.h>, <memory/memory_map.h>, <memory/pmm.h>, and
# <memory/pmm_boot.h>. It does NOT use the spinlock / arch_cpu_halt
# symbols, but the PMM_SHADOW_INC is still applied for the test TU
# and for the early_arena production object (the test TU touches
# PMMngr.start_brk to assert the canary).
#
# `make test-m1-host CASE=m1-arena` (root run.mk) reaches this rule
# via the focused dispatch in run.mk.
EARLY_ARENA_HOST_CFLAGS := $(HOST_CFLAGS) $(PMM_SHADOW_INC) $(KERNEL_INC) \
                           -I$(TEST_INC) $(LIBC_INC)

$(TEST_BLD)/early_arena_production.o: $(TESTS_DIR)/kernel/arch/aarch64/memory/early_arena.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/early_arena.h \
		$(TESTS_DIR)/kernel/include/memory/pmm.h \
		$(TESTS_DIR)/kernel/include/memory/pmm_boot.h \
		$(TESTS_DIR)/kernel/include/memory/memory_map.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(EARLY_ARENA_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_m1_arena.o: $(TEST_CASES)/test_m1_arena.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/early_arena.h \
		$(TESTS_DIR)/kernel/include/memory/pmm.h \
		$(TESTS_DIR)/kernel/include/memory/pmm_boot.h \
		$(TESTS_DIR)/kernel/include/memory/memory_map.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(EARLY_ARENA_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_m1_arena.elf: $(TEST_BLD)/test_m1_arena.o \
		$(TEST_BLD)/early_arena_production.o \
		$(TEST_BLD)/pmm_production.o \
		$(TEST_BLD)/pmm_arch_default.o \
		$(TEST_BLD)/pmm_boot_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test-m1-arena
test-m1-arena: $(TEST_BLD)/test_m1_arena.elf $(TEST_BLD)/test_m1_arena_exhaust.elf \
    $(TEST_BLD)/test_arena_layout_chain.elf \
    $(TEST_BLD)/test_arena_2gib_boundary.elf \
    $(TEST_BLD)/test_arena_candidate_slab_bytes.elf \
    $(TEST_BLD)/test_arena_table_base_shift.elf
	$(TEST_BLD)/test_m1_arena.elf
	$(TEST_BLD)/test_m1_arena_exhaust.elf
	$(TEST_BLD)/test_arena_layout_chain.elf
	$(TEST_BLD)/test_arena_2gib_boundary.elf
	$(TEST_BLD)/test_arena_candidate_slab_bytes.elf
	$(TEST_BLD)/test_arena_table_base_shift.elf

.PHONY: test-aarch64-color-printk-abi
test-aarch64-color-printk-abi: $(TEST_BLD)/test_aarch64_color_printk_abi.elf
	$(TEST_BLD)/test_aarch64_color_printk_abi.elf

# aarch64 M2/M3 plan Task 5: color_printk ABI contract. Links the REAL
# kernel/arch/aarch64/runtime/printk_stub.c against a kputs capture
# mock. The test TU includes the public <core/printk.h> so a signature
# mismatch (the old `void color_printk(const char *fmt, ...)` stub) is
# a compile error; the shadow <arch/aarch64/boot_log.h> lives in
# $(TEST_MOCK)/aarch64_color_printk and comes BEFORE $(KERNEL_INC).
AARCH64_PRINTK_STUB_CFLAGS := $(HOST_CFLAGS) -I$(TESTS_DIR)/kernel/include

$(TEST_BLD)/test_aarch64_color_printk_abi.o: $(TEST_CASES)/test_aarch64_color_printk_abi.c \
		$(TESTS_DIR)/kernel/include/core/printk.h \
		$(TEST_MOCK)/aarch64_color_printk/arch/aarch64/boot_log.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) \
		-I$(TEST_MOCK)/aarch64_color_printk $(KERNEL_INC) -c $< -o $@

$(TEST_BLD)/aarch64_printk_stub_production.o: \
		$(TESTS_DIR)/kernel/arch/aarch64/runtime/printk_stub.c \
		$(TESTS_DIR)/kernel/include/core/printk.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/boot_log.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(AARCH64_PRINTK_STUB_CFLAGS) -c $< -o $@

$(TEST_BLD)/kputs_mock.o: $(TEST_MOCK)/aarch64_color_printk/kputs_mock.c \
		$(TEST_MOCK)/aarch64_color_printk/arch/aarch64/boot_log.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_MOCK)/aarch64_color_printk -c $< -o $@

$(TEST_BLD)/test_aarch64_color_printk_abi.elf: \
		$(TEST_BLD)/test_aarch64_color_printk_abi.o \
		$(TEST_BLD)/aarch64_printk_stub_production.o \
		$(TEST_BLD)/kputs_mock.o
	$(HOST_CC) -o $@ $^

# aarch64 M2/M3 plan Task 3: arena layout-chain / 2 GiB boundary /
# candidate-scan-includes-slab-bytes. Each new TU reuses the production
# early_arena.o via the same EARLY_ARENA_HOST_CFLAGS / link line as the
# existing test_m1_arena.elf. The stub surface (g_log_level,
# _log_err_impl, color_printk, slab_init) lives in each TU — keeps the
# rules self-contained and matches test_m1_arena.c / _exhaust.c.
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

# aarch64 M2/M3 plan Task 4: table_base_pa shifted past the slab pages.
# Links the REAL early_arena.c (aarch64_m1_plan) AND the REAL
# runtime_tree.c (aarch64_runtime_tree_build) — the builder half drives
# aarch64_runtime_tree_build through injected fake pool ops whose PA
# cursor starts at arena.table_base_pa, proving the builder consumes
# pages only from [table_base_pa, table_end_pa) (zero-change across the
# base shift). Stub surface identical to test_arena_layout_chain.c.
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

# aarch64 M1 plan Task 4: pure runtime page-table builder and strict
# validator (`aarch64_runtime_tree_build` / `_validate`). Links the
# REAL kernel/arch/aarch64/memory/runtime_tree.c against the host
# TU. The production file only pulls <errno.h>, <stddef.h>,
# <stdint.h>, <stdbool.h>, <arch/aarch64/runtime_tree.h>,
# <arch/aarch64/early_arena.h>, <arch/aarch64/page_table.h>, and
# <memory/memory_map.h> — all in KERNEL_INC. The test TU uses its
# own fake-pool ops to provide monotonic-PA, host-alloc-backed page
# resolution.
#
# `make test-m1-host CASE=m1-tree` (root run.mk) reaches this rule
# via the focused dispatch in run.mk.
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

$(TEST_BLD)/test_m1_tree.o: $(TEST_CASES)/test_m1_tree.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/runtime_tree.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/early_arena.h \
		$(TESTS_DIR)/kernel/include/memory/memory_map.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(RUNTIME_TREE_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_m1_tree.elf: $(TEST_BLD)/test_m1_tree.o \
		$(TEST_BLD)/runtime_tree_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test-m1-tree
test-m1-tree: $(TEST_BLD)/test_m1_tree.elf
	$<

# aarch64 M1 plan Task 5: common boot direct-map contract
# (`arch_boot_direct_map_init` / `_ready` / `_ranges` from
# kernel/include/arch/boot_memory.h). Links the REAL
# kernel/arch/x86_64/memory/boot_direct_map.c (the x86 backend) —
# the test stubs vmm_init itself so we can inject OOM and validation
# failures without exercising vmm.c's calloc chain. The TU pulls
# <arch/boot_memory.h>, <memory/memory.h>, <memory/pmm.h>,
# <memory/vmm.h>, <memory/memory_map.h> — all in KERNEL_INC. The
# test TU adds FRAMEWORK_INC + LIBC_INC for <test_framework.h> and
# <errno.h>.
#
# `make test-m1-host CASE=m1-contract-x86` (root run.mk) reaches
# this rule via the focused dispatch in run.mk.
M1_CONTRACT_HOST_CFLAGS := $(HOST_CFLAGS) -DOS01_HOST_TEST -I$(TEST_MOCK)/m1_include $(KERNEL_INC) -I$(TEST_INC)

$(TEST_BLD)/boot_direct_map_production.o: \
		$(TESTS_DIR)/kernel/arch/x86_64/memory/boot_direct_map.c \
		$(TESTS_DIR)/kernel/include/arch/boot_memory.h \
		$(TESTS_DIR)/kernel/include/memory/memory.h \
		$(TESTS_DIR)/kernel/include/memory/memory_map.h \
		$(TESTS_DIR)/kernel/include/memory/pmm.h \
		$(TESTS_DIR)/kernel/include/memory/vmm.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(M1_CONTRACT_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_m1_contract.o: $(TEST_CASES)/test_m1_contract.c \
		$(TESTS_DIR)/kernel/include/arch/boot_memory.h \
		$(TESTS_DIR)/kernel/include/memory/memory.h \
		$(TESTS_DIR)/kernel/include/memory/memory_map.h \
		$(TESTS_DIR)/kernel/include/memory/pmm.h \
		$(TESTS_DIR)/kernel/include/memory/vmm.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(M1_CONTRACT_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_m1_contract.elf: $(TEST_BLD)/test_m1_contract.o \
		$(TEST_BLD)/boot_direct_map_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test-m1-contract-x86
test-m1-contract-x86: $(TEST_BLD)/test_m1_contract.elf $(TEST_BLD)/test_m1_vmm.elf \
    $(TEST_BLD)/test_m1_install.elf $(TEST_BLD)/test_m1_cleanup.elf \
    $(TEST_BLD)/test_m1_publish.elf
	$(TEST_BLD)/test_m1_contract.elf
	$(TEST_BLD)/test_m1_vmm.elf

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

# Canon line discipline test — compiles REAL kernel/tty/canon.c (pure logic)
$(TEST_BLD)/canon.o: $(TESTS_DIR)/kernel/tty/canon.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/kernel/include -c $< -o $@

$(TEST_BLD)/test_canon.o: $(TEST_CASES)/test_canon.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/kernel/include -c $< -o $@

$(TEST_BLD)/test_canon.elf: $(TEST_BLD)/test_canon.o $(TEST_BLD)/canon.o
	$(HOST_CC) -o $@ $^

# Terminal core test — compiles REAL user/terminal_core.c (pure logic)
$(TEST_BLD)/terminal_core.o: $(TESTS_DIR)/user/terminal_core.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/user -c $< -o $@

$(TEST_BLD)/test_terminal_core.o: $(TEST_CASES)/test_terminal_core.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/user -c $< -o $@

$(TEST_BLD)/test_terminal_core.elf: $(TEST_BLD)/test_terminal_core.o $(TEST_BLD)/terminal_core.o
	$(HOST_CC) -o $@ $^

# Tetris logic test — compiles REAL user/tetris_logic.c (pure C)
$(TEST_BLD)/tetris_logic.o: $(TESTS_DIR)/user/tetris_logic.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/user -c $< -o $@

$(TEST_BLD)/test_tetris_logic.o: $(TEST_CASES)/test_tetris_logic.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/user -c $< -o $@

$(TEST_BLD)/test_tetris_logic.elf: $(TEST_BLD)/test_tetris_logic.o $(TEST_BLD)/tetris_logic.o
	$(HOST_CC) -o $@ $^

POLL_PRODUCTION_OBJS := \
    $(TEST_BLD)/poll_production.o \
    $(TEST_BLD)/select_production.o \
    $(TEST_BLD)/file_poll_production.o \
    $(TEST_BLD)/tty_poll_production.o \
    $(TEST_BLD)/devfs_poll_production.o \
    $(TEST_BLD)/poll_clocksource_stub.o

$(TEST_BLD)/test_poll_requested.elf: $(TEST_BLD)/test_poll_requested.o $(POLL_PRODUCTION_OBJS)
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

# Poll waitqueue handshake RED/GREEN test (Task 3): exercises the
# new arm-重查-schedule handshake in do_poll_core with real-semantics
# wait_queue_* (enqueue/dequeue + state transitions) and a mock
# FD_PIPE whose readiness flips via a producer hook fired from the
# test-local wait_queue_arm (the single-threaded vantage point over
# the scan1→ARM→schedule handshake).  Race windows covered: wake
# between scan1 and ARM (lost on empty queue — old code's bug) and
# wake between ARM and schedule (must not re-sleep).
$(TEST_BLD)/test_poll_waitqueue_handshake.o: $(TEST_CASES)/test_poll_waitqueue_handshake.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(POLL_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_poll_waitqueue_handshake.elf: $(TEST_BLD)/test_poll_waitqueue_handshake.o $(POLL_PRODUCTION_OBJS)
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

# LWIP_RAND white-box fixture: host-compile the PRODUCTION
# kernel/net/lwip_sys_arch.c (it depends only on <random/random.h> +
# <stdint.h>) and link with -Wl,--wrap=get_random_bytes so the test
# asserts the real call path, not a mock of it.
LWIP_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC)

$(TEST_BLD)/lwip_rand_production.o: $(TESTS_DIR)/kernel/net/lwip_sys_arch.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(LWIP_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_lwip_rand.elf: $(TEST_BLD)/test_lwip_rand.o $(TEST_BLD)/lwip_rand_production.o
	$(HOST_CC) -Wl,--wrap=get_random_bytes -o $@ $^

test_lwip_rand: $(TEST_BLD)/test_lwip_rand.elf
	$<

# GICv2 driver hosttest: host-compile the PRODUCTION
# kernel/arch/aarch64/intr/gic_driver.c against mock-MMIO arrays (spec §4.1).
# gic_driver.c is dependency-free (no arch asm, no UART logging); the
# pt_regs type is forward-declared in <arch/aarch64/gic.h> and the test
# never defines a pt_regs object (R1-4: dispatch cases pass NULL).
GIC_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC)

$(TEST_BLD)/gic_driver_production.o: $(TESTS_DIR)/kernel/arch/aarch64/intr/gic_driver.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/gic.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GIC_HOST_CFLAGS) -c $< -o $@

# The test TU itself must see <arch/aarch64/gic.h>; the generic
# $(TEST_BLD)/%.o rule below lacks KERNEL_INC. Add it explicitly.
$(TEST_BLD)/test_gic_driver.o: $(TEST_CASES)/test_gic_driver.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/gic.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GIC_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_gic_driver.elf: $(TEST_BLD)/test_gic_driver.o $(TEST_BLD)/gic_driver_production.o
	$(HOST_CC) -o $@ $^

test_gic_driver: $(TEST_BLD)/test_gic_driver.elf
	$<

# aarch64 ipi_broadcast hosttest (M3 Task 7): compiles the PRODUCTION
# kernel/arch/aarch64/intr/ipi.c (logical vector -> SGI mapping) plus the
# production gic_driver.c against mock MMIO. The test TU supplies
# gic_dev_current() (gic.c is too heavy for host) and a longjmp kpanic hook.
AARCH64_IPI_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC)

$(TEST_BLD)/aarch64_ipi_production.o: $(TESTS_DIR)/kernel/arch/aarch64/intr/ipi.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/ipi.h \
		$(TESTS_DIR)/kernel/include/intr/ipi.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/dtb.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(AARCH64_IPI_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_aarch64_ipi_broadcast.o: $(TEST_CASES)/test_aarch64_ipi_broadcast.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/ipi.h \
		$(TESTS_DIR)/kernel/include/intr/ipi.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(AARCH64_IPI_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_aarch64_ipi_broadcast.elf: $(TEST_BLD)/test_aarch64_ipi_broadcast.o \
		$(TEST_BLD)/aarch64_ipi_production.o $(TEST_BLD)/gic_driver_production.o
	$(HOST_CC) -o $@ $^

test_aarch64_ipi_broadcast: $(TEST_BLD)/test_aarch64_ipi_broadcast.elf
	$<

# aarch64 gic_target_bit_init hosttest (M3 Task 8): compiles the PRODUCTION
# kernel/arch/aarch64/intr/ipi.c plus the production gic_driver.c against
# mock MMIO. The test TU mocks dtb_cpu_count()/dtb_gicd_base() (dtb.c is too
# heavy for host) and overrides the weak FATAL / out-of-range hooks.
$(TEST_BLD)/test_aarch64_gic_target_probe.o: \
		$(TEST_CASES)/test_aarch64_gic_target_probe.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/ipi.h \
		$(TESTS_DIR)/kernel/include/intr/ipi.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/dtb.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(AARCH64_IPI_HOST_CFLAGS) \
		-DOS01_KERNEL_SRC=\"$(TESTS_DIR)\" \
		$(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_aarch64_gic_target_probe.elf: \
		$(TEST_BLD)/test_aarch64_gic_target_probe.o \
		$(TEST_BLD)/aarch64_ipi_production.o $(TEST_BLD)/gic_driver_production.o
	$(HOST_CC) -o $@ $^

test_aarch64_gic_target_probe: $(TEST_BLD)/test_aarch64_gic_target_probe.elf
	$<

# ap_work protocol hosttest (M3 Task 10): compiles the PRODUCTION
# kernel/arch/aarch64/smp/ap_work.c against the host. The preinclude
# mock/ap_work_test_runtime.h short-circuits <arch/cpu.h> and
# <arch/aarch64/boot_log.h>; the test TU supplies kputs/kputu (captured
# into a buffer for the WORK-TIMEOUT FATAL line) and test-controlled
# arch_cycle_counter/arch_cpu_halt (longjmp hook proves the halt path).
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

test_ap_work_protocol: $(TEST_BLD)/test_ap_work_protocol.elf
	$<

# percpu_t layout pin hosttest (M3 Task 9): header-only TU that includes
# the PRODUCTION kernel/include/percpu/percpu.h and repeats at runtime the
# _Static_asserts that pin sizeof(percpu_t) == PERCPU_DATA_SIZE (the
# literal head.S strides percpu_data[] with) and the ipi_ready/tlb_ack_gen
# tail position that percpu_init()'s memset skips. No production objects —
# the whole contract lives in the header's struct definition.
# NOTE: deliberately NOT using $(FRAMEWORK_INC): its preinclude
# mock/test_platform.h typedefs its own (stale, 144-byte-class) percpu_t,
# which would shadow the real one this test exists to pin.
$(TEST_BLD)/test_percpu_layout.o: $(TEST_CASES)/test_percpu_layout.c \
		$(TESTS_DIR)/kernel/include/percpu/percpu.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_INC) $(KERNEL_INC) $(LIBC_INC) \
		-c $< -o $@

$(TEST_BLD)/test_percpu_layout.elf: $(TEST_BLD)/test_percpu_layout.o
	$(HOST_CC) -o $@ $^

test_percpu_layout: $(TEST_BLD)/test_percpu_layout.elf
	$<

# TLB shootdown serial-protocol hosttest (M2/M3 Task 12 Step 1): compiles
# the PRODUCTION kernel/memory/tlb.c against the host.  The preinclude
# mock/tlb_test_runtime.h short-circuits percpu/spinlock/mmu/cpu/ipi
# headers; the test TU supplies percpu_data[], num_cpus, the
# ipi_broadcast mock and overrides the weak tlb_shootdown_panic() hook
# with a longjmp capture (timeout → FATAL case).
TLB_SD_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC)
TLB_SD_PROD_CFLAGS := $(TLB_SD_HOST_CFLAGS) \
                      -include $(TEST_MOCK)/tlb_test_runtime.h

$(TEST_BLD)/tlb_production.o: $(TESTS_DIR)/kernel/memory/tlb.c \
		$(TEST_MOCK)/tlb_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(TLB_SD_PROD_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_tlb_serial_protocol.o: \
		$(TEST_CASES)/test_tlb_serial_protocol.c \
		$(TEST_MOCK)/tlb_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(TLB_SD_HOST_CFLAGS) -I$(TEST_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_tlb_serial_protocol.elf: \
		$(TEST_BLD)/test_tlb_serial_protocol.o \
		$(TEST_BLD)/tlb_production.o
	$(HOST_CC) -o $@ $^

test_tlb_serial_protocol: $(TEST_BLD)/test_tlb_serial_protocol.elf
	$<

# x86 ipi_ready publish-point hosttest (M2/M3 Task 12 Step 4): pure
# source-level scan (QEMU x86_64 regression is currently blocked by the
# known infra stall, so the publish invariants are asserted against the
# production source text — mirrors test_gic_marker_lines.c).
X86_PUBLISH_CFLAGS := $(HOST_CFLAGS) -DOS01_KERNEL_SRC=\"$(TESTS_DIR)\" \
                      -I$(TEST_INC) -include $(TEST_MOCK)/test_platform.h

$(TEST_BLD)/test_x86_ipi_ready_publish.o: \
		$(TEST_CASES)/test_x86_ipi_ready_publish.c \
		$(TESTS_DIR)/kernel/arch/x86_64/smp/boot.c \
		$(TESTS_DIR)/kernel/arch/x86_64/smp/smp.c \
		$(TESTS_DIR)/kernel/arch/x86_64/intr/apic/ipi.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(X86_PUBLISH_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_x86_ipi_ready_publish.elf: \
		$(TEST_BLD)/test_x86_ipi_ready_publish.o
	$(HOST_CC) -o $@ $^

test_x86_ipi_ready_publish: $(TEST_BLD)/test_x86_ipi_ready_publish.elf
	$<

# M3.1 audit gate (Task 13): source-scan half. Static assertion that no
# audited function both holds a lock and calls a vmm-change API, plus
# whitelist checks (tlb.c single non-nested lock, ipi.c lock-free,
# kernel/Makefile aarch64 source list). The nm half lives in
# mk/components/run.mk (test-aarch64-audit) — hosttests never exec.
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

test_vmm_caller_audit: $(TEST_BLD)/test_vmm_caller_audit.elf
	$<

# Public-vmm-header contract hosttest (M3.2 Task 14 Step 1): header-only
# TU pinning the semantic layer (VM_* + arch_vmm_* protos in
# memory/vmm.h) and the x86-private PTE layer (arch/x86_64/pte.h).
$(TEST_BLD)/test_vmm_semantic_header.o: \
		$(TEST_CASES)/test_vmm_semantic_header.c \
		$(TESTS_DIR)/kernel/include/memory/vmm.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/pte.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_INC) $(KERNEL_INC) \
		-c $< -o $@

$(TEST_BLD)/test_vmm_semantic_header.elf: $(TEST_BLD)/test_vmm_semantic_header.o
	$(HOST_CC) -o $@ $^

test_vmm_semantic_header: $(TEST_BLD)/test_vmm_semantic_header.elf
	$<

# aarch64 M3.2 Task 15: x86 vmm.c backend wrapper + free/COW logic
# preservation.  Links the REAL kernel/memory/vmm.c against the
# x86_vmm_backend harness (flat pre-filled PTE chain,
# counter-backed page_cow_put / free_4k_page mocks).  Asserts the
# new x86_vmm_map_4k_page / x86_vmm_unmap_4k_page_with_free /
# x86_vmm_query_4k_page API and the single-free-per-branch COW
# invariant.  --wrap=calloc is unused by the unmap path (the chain
# is pre-filled) but kept so vmm.c's calloc reference resolves at
# link time.
$(TEST_BLD)/x86_vmm_backend_production.o: $(TESTS_DIR)/kernel/memory/vmm.c \
		$(TESTS_DIR)/kernel/include/memory/vmm.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/pte.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(M1_CONTRACT_HOST_CFLAGS) $(LIBC_INC) \
		-ffunction-sections -fdata-sections -c $< -o $@

$(TEST_BLD)/test_x86_vmm_backend.o: $(TEST_CASES)/test_x86_vmm_backend.c \
		$(TESTS_DIR)/kernel/include/memory/vmm.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/pte.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(M1_CONTRACT_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) \
		-c $< -o $@

$(TEST_BLD)/test_x86_vmm_backend.elf: \
		$(TEST_BLD)/test_x86_vmm_backend.o \
		$(TEST_BLD)/x86_vmm_backend_production.o
	$(HOST_CC) -Wl,--gc-sections -Wl,--wrap=calloc -o $@ $^

# aarch64 M3.3 Task 16: page_table.c software-bit + block-encoding
# RED test. Compiles the REAL kernel/arch/aarch64/memory/page_table.c
# against host mocks. page_table.c contains aarch64 inline asm
# (tlbi vae1, dsb ish, isb) inside tlb_invalidate_local / dsb_ishst;
# the production file guards those asm blocks with #ifdef __aarch64__
# so it compiles cleanly on the x86 host. is_active_root() returns
# false on host (mock aarch64_read_ttbr1() returns 0 and real CR3
# never matches the test root pointer), so the guarded TLBI path
# is never reached in the host harness.
#
# The arch_get_page_table() in <arch/mmu.h> is #elif-guarded for
# __aarch64__ vs __x86_64__; on host it takes the x86 branch (mov
# CR3) and returns the host CR3 — guaranteed not to match our test
# root.
#
# vmm_gate_check() is stubbed in the test TU (page_table.c only
# references it as a symbol; the production vmm_gate.c is not
# linked here so we don't need percpu_data[] or dtb_cpu_count()).
AARCH64_PT_SW_HOST_CFLAGS := $(HOST_CFLAGS) -I$(TEST_MOCK)/m1_a64_include \
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

test_aarch64_pt_software_bits: $(TEST_BLD)/test_aarch64_pt_software_bits.elf
	$<

# aarch64 M3.3 Task 17: pt_locks static initializers + walk_to_l2(create)
# with ENOMEM rollback + source-scan lock-order check.  Compiles the
# REAL kernel/arch/aarch64/memory/page_table.c against the same host
# mocks as test_aarch64_pt_software_bits.c (vmm_gate_check stub,
# arch_get_page_table returns NULL so is_active_root never fires,
# alloc/free mocked with a heap-backed pool + a fail-count injection
# for the ENOMEM rollback case).
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

test_aarch64_pt_locks: $(TEST_BLD)/test_aarch64_pt_locks.elf
	$<

# aarch64 M3.3 Task 18: 2 MiB block primitive contract (spec §5.2 /
# §4.4.5). Compiles the REAL kernel/arch/aarch64/memory/page_table.c
# against the same host harness as test_aarch64_pt_locks.c
# (aarch64_read_ttbr1 returns 0, vmm_gate_check is a no-op, is_active_root
# always false so the guarded TLBI path is unreachable, alloc/free
# backed by a host-heap pool with a fail-counter for ENOMEM injection).
# The new aarch64_pt_map_2m_block / aarch64_pt_unmap_2m_block /
# aarch64_pt_read_l2_desc / aarch64_pt_split_block_2m primitives are
# exercised directly (no vmm_backend.c link — this test pins the
# primitive contract; the backend layer is covered separately by
# test_aarch64_backend_4k.elf).
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

test_aarch64_pt_2m_block: $(TEST_BLD)/test_aarch64_pt_2m_block.elf
	$<

# aarch64 M3.3 Task 18: arch_vmm_* backend 12-legal + 4-reject combo
# matrix (spec §4.2 / §4.4.4). Links the REAL page_table.c AND the
# REAL vmm_backend.c against the host harness. kernel_map is set
# directly by the test TU (it lives in vmm_backend.c as a global;
# same shape test_x86_vmm_backend.c uses for its pinned pgdir).
#
# The test override for vmm_gate_violation (weak default in
# vmm_gate.c spins forever) just bumps a counter — the host TU's
# vmm_gate_check stub never invokes it on the happy paths, but
# strays would surface as a counter increment rather than a hang.
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

test_aarch64_backend_4k: $(TEST_BLD)/test_aarch64_backend_4k.elf
	$<

# Foundational-primitives hosttest (M3 Task 7 Step 4): compiles the
# PRODUCTION kernel/arch/aarch64/memory/vmm_gate.c (root registry,
# smp_starting, ipi_ready_count, vmm gate). The preinclude
# vmm_gate_test_runtime.h short-circuits percpu/spinlock; the test TU
# supplies percpu_data[], a mock dtb_cpu_count() and overrides the weak
# vmm_gate_violation() hook with a longjmp capture.
VMM_GATE_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC)
# Production TU only: preinclude the stub BEFORE any kernel header.
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

# Link stubs for tests that pull production files calling into vmm_gate.c
# (test_m1_install compiles boot_direct_map.c, which publishes the M1 root).
$(TEST_BLD)/vmm_gate_test_stubs.o: $(TEST_MOCK)/vmm_gate_test_stubs.c \
		$(TEST_MOCK)/vmm_gate_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VMM_GATE_HOST_CFLAGS) -I$(TEST_MOCK) -c $< -o $@

test_foundational_primitives: $(TEST_BLD)/test_foundational_primitives.elf
	$<

# GICv2 marker lines: source-level RED check (Task 2.1). QEMU aarch64-uefi
# is currently blocked by thirdpart/{posix-uefi,mbedtls,lwip} submodule
# emptiness; the marker lines that the kernel would emit are checked at
# the source-file level instead (full RED on the missing markers, GREEN
# once Task 2.2 wires them up in main.c / irq_probe.c).
GIC_MARKER_CFLAGS := $(HOST_CFLAGS) -DOS01_KERNEL_SRC=\"$(TESTS_DIR)\" \
                     -I$(TEST_INC) -include $(TEST_MOCK)/test_platform.h

$(TEST_BLD)/test_gic_marker_lines.o: $(TEST_CASES)/test_gic_marker_lines.c \
		$(TESTS_DIR)/kernel/arch/aarch64/intr/gic.c \
		$(TESTS_DIR)/kernel/arch/aarch64/boot/main.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GIC_MARKER_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_gic_marker_lines.elf: $(TEST_BLD)/test_gic_marker_lines.o
	$(HOST_CC) -o $@ $<

test_gic_marker_lines: $(TEST_BLD)/test_gic_marker_lines.elf
	$<

# M3 Task 11: BSP double-state publish ordering (percpu_init / num_cpus /
# smp_starting_enter before smp_boot_aps; ipi_ready published only after
# IRQ unmask). Same source-scan pattern as test_gic_marker_lines: reads
# main.c / smp.c at runtime via OS01_KERNEL_SRC and asserts order by
# first-occurrence index.
$(TEST_BLD)/test_double_state_publish.o: $(TEST_CASES)/test_double_state_publish.c \
		$(TESTS_DIR)/kernel/arch/aarch64/boot/main.c \
		$(TESTS_DIR)/kernel/arch/aarch64/smp/smp.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GIC_MARKER_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_double_state_publish.elf: $(TEST_BLD)/test_double_state_publish.o
	$(HOST_CC) -o $@ $<

test_double_state_publish: $(TEST_BLD)/test_double_state_publish.elf
	$<

# GIC clobber-probe encoding contract — host-side RED for the QEMU TIMEOUT bug.
# Same compile pattern as test_gic_marker_lines: source-level check via
# OS01_KERNEL_SRC. The C wrapper (gic_send_sgi) is exercised against the
# production gic_driver.c (mock MMIO) AND the asm source is scanned for the
# known-buggy immediate literal.
$(TEST_BLD)/test_gic_probe.o: $(TEST_CASES)/test_gic_probe.c \
		$(TESTS_DIR)/kernel/arch/aarch64/intr/irq_probe.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/gic.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GIC_HOST_CFLAGS) $(GIC_MARKER_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_gic_probe.elf: $(TEST_BLD)/test_gic_probe.o $(TEST_BLD)/gic_driver_production.o
	$(HOST_CC) -o $@ $^

test_gic_probe: $(TEST_BLD)/test_gic_probe.elf
	$<

# Clocksource unit tests (aarch64 Timer integration Task 1.1 RED).
# Compiles REAL kernel/time/clocksource.c against the host runtime
# (clocksource_test_runtime.h short-circuits the aarch64 inline asm in
# <arch/cpu.h>, <arch/spinlock.h>, <arch/percpu.h>, <percpu/percpu.h>,
# <sched/task.h>). Link resolves arch_cycle_freq/arch_cycle_counter
# against the mock symbols in clocksource_test_stubs.c.
#
# NOTE: this rule does NOT use $(FRAMEWORK_INC) — that would -include
# test_platform.h and conflict with our percpu_t / this_cpu() definitions.
# Instead we -include clocksource_test_runtime.h directly, which owns the
# full host stub surface (including the pieces test_platform.h normally
# provides, copied here).
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

test_clocksource: $(TEST_BLD)/test_clocksource.elf
	$<

# i8042 demux/transaction hosttest (PS/2 mouse Task 4): host-compiles the
# REAL kernel/driver/i8042.c against a mock port model defined in the test
# TU (OBF FIFO with AUX flags, test-settable IBF, recorded writes, virtual
# cycle clock). i8042_test_runtime.h is force-included and shadows
# <arch/io.h>, <arch/spinlock.h> and <arch/cpu.h> via their include guards,
# so the production source compiles unmodified for the host.
I8042_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC) \
    -D_ARCH_IO_H -D_ARCH_SPINLOCK_H -D_ARCH_CPU_H \
    -include $(TEST_MOCK)/i8042_test_runtime.h

# Keyboard-over-i8042 (PS/2 mouse Task 5): the REAL kernel/driver/keyboard.c
# joins the same binary.  mock/kbd_include shadows <tty/tty.h>, <fs/vfs.h>,
# <fs/poll.h>, <intr/interrupt.h>, <intr/apic.h>, <core/debug.h>,
# <percpu/percpu.h> and <subsys/subsys.h> (placed BEFORE kernel/include on
# the path), so the production source compiles unmodified for the host; the
# mock port model above.  NOTE: the shadow dir must come BEFORE
# $(KERNEL_INC) so <percpu/percpu.h> etc. resolve to the mocks.
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
		$(TEST_BLD)/keyboard_production.o
	$(HOST_CC) -o $@ $^

test_i8042_demux: $(TEST_BLD)/test_i8042_demux.elf
	$<

$(TEST_BLD)/test_mouse_proto.elf: $(TEST_CASES)/test_mouse_proto.c $(TESTS_DIR)/kernel/driver/mouse_proto.c $(TESTS_DIR)/kernel/include/driver/mouse_proto.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -I$(TEST_INC) $< $(TESTS_DIR)/kernel/driver/mouse_proto.c -o $@

test_mouse_proto: $(TEST_BLD)/test_mouse_proto.elf
	$<

# Links the real phase-6 mouse driver against scriptable i8042 and IRQ
# transactions; the test observes command-byte and consumer state.
$(TEST_BLD)/test_mouse_init.elf: $(TEST_CASES)/test_mouse_init.c \
		$(TESTS_DIR)/kernel/driver/mouse.c \
		$(TESTS_DIR)/kernel/include/driver/mouse.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(KBD_HOST_CFLAGS) $< \
		$(TESTS_DIR)/kernel/driver/mouse.c -o $@

test_mouse_init: $(TEST_BLD)/test_mouse_init.elf
	$<

# Gfx file-lifecycle test (Task 1): host-compiles the REAL
# kernel/fs/devfs.c + kernel/fs/file.c against gfx_test_runtime.h,
# which shadows the heavyweight kernel headers (sched/task.h,
# tty/tty.h, tty/pty.h, driver/serial.h, ...) so the production
# sources compile unmodified.  The test TU provides the stub
# surface the production sources reference (kmalloc/kfree, current,
# schedule, get_dev_tty, ...).  Tested production symbols:
# devfs_register_chrdev, devfs_open_node, devfs_ioctl_file,
# devfs_release_file, file_alloc, file_get, file_put, fd_ioctl.
GFX_HOST_CFLAGS := $(HOST_CFLAGS) -ffunction-sections -fdata-sections \
    -Wno-unused-function -D__SPINLOCK_H__ -DOS01_HOST_TEST \
    -include $(TEST_MOCK)/gfx_test_runtime.h \
    -I$(TEST_MOCK)/include $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)

$(TEST_BLD)/gfx_devfs_production.o: $(TESTS_DIR)/kernel/fs/devfs.c \
		$(TEST_MOCK)/gfx_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/gfx_file_production.o: $(TESTS_DIR)/kernel/fs/file.c \
		$(TEST_MOCK)/gfx_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_gfx_file_lifecycle.o: $(TEST_CASES)/test_gfx_file_lifecycle.c \
		$(TEST_MOCK)/gfx_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_gfx_file_lifecycle.elf: $(TEST_BLD)/test_gfx_file_lifecycle.o \
		$(TEST_BLD)/gfx_devfs_production.o \
		$(TEST_BLD)/gfx_file_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

test-gfx-file-lifecycle: $(TEST_BLD)/test_gfx_file_lifecycle.elf
	$<

# Gfx device test (Task 2): host-compiles the REAL
# kernel/driver/gfx.c against gfx_test_runtime.h, which provides the
# heavy stubs the lifecycle test already declares (kmalloc/kfree,
# current, schedule, vfs_node_get/put, lwip, ...).  kernel/driver/fb.c
# is NOT compiled (VMA/VMM/scheduler headers are out of scope for
# this test); the test TU mocks fb_get_info and fb_write_row against
# a host-heap fb buffer so the gfx device's present contract is
# observed end-to-end.  Tested production symbols: gfx_ops (open,
# ioctl_file, release_file).
$(TEST_BLD)/gfx_gfx_production.o: $(TESTS_DIR)/kernel/driver/gfx.c \
		$(TESTS_DIR)/kernel/include/driver/gfx.h \
		$(TESTS_DIR)/kernel/include/uapi/gfx.h \
		$(TEST_MOCK)/gfx_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_gfx_device.o: $(TEST_CASES)/test_gfx_device.c \
		$(TESTS_DIR)/kernel/include/driver/gfx.h \
		$(TESTS_DIR)/kernel/include/uapi/gfx.h \
		$(TEST_MOCK)/gfx_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_gfx_device.elf: $(TEST_BLD)/test_gfx_device.o \
		$(TEST_BLD)/gfx_gfx_production.o \
		$(TEST_BLD)/gfx_devfs_production.o \
		$(TEST_BLD)/gfx_file_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

test-gfx-device: $(TEST_BLD)/test_gfx_device.elf
	$<

# Gfx client lifecycle test (Task 3, 2D graphics API plan): host-compiles
# the REAL libgfx/gfx.c (open/close/get_info/set_clip/present) against the
# HOST compiler — no --target, no sysroot, no -ffreestanding — and links
# it through -Wl,--wrap=open,--wrap=close,--wrap=ioctl,--wrap=malloc,
# --wrap=calloc,--wrap=free so the test TU observes libgfx's libc call
# pattern (every reference is routed through the wrap stubs; libc.so's
# internal callers resolve intra-library and don't flow through .elf's
# wrapped symbols).
#
# IMPORTANT: this rule deliberately does NOT add $(TEST_INC) to the
# include path.  hosttests/include/errno.h declares `extern int errno;`
# (OS01's non-TLS errno), but host glibc provides a TLS errno via
# `__errno_location`; linking libgfx_gfx.o + test_gfx_client.o against
# libc.so with the non-TLS reference yields the "TLS definition in
# libc.so section .tbss mismatches non-TLS reference" link error.
# Skipping $(TEST_INC) lets the host compiler pick up glibc's <errno.h>,
# which keeps libgfx and test on the same TLS-aware errno.  The test TU
# only needs <gfx.h> (from the libgfx source dir) and <uapi/gfx.h>
# (from kernel UAPI) — both delivered below.
#
# Force-include the errno ABI shim (gfx_client_errno.h) and define
# `errno` to glibc's TLS accessor so every `errno` reference in
# libgfx/gfx.c expands to `(*__errno_location())`.  Without these,
# the compiled TU holds a non-TLS int reference that the linker
# rejects.  The shim only declares `__errno_location`; the -D
# supplies the macro that turns each `errno` reference into a call.
#
# libgfx/gfx.c compiles against this CFLAGS.  The test TU also adds
# -I$(TEST_INC) for test_framework.h resolution (its source has
# `#include "test_framework.h"`); the errno ABI shim is force-included
# AFTER that include resolves, so the OS01 errno.h macro constants
# (EPERM, ENOENT, ENODEV, ...) come from hosttests/include and the
# `errno` reference itself still expands to glibc's TLS accessor.
GFX_CLIENT_PROD_CFLAGS := $(HOST_CFLAGS) \
    -I$(TESTS_DIR)/libgfx -I$(TESTS_DIR)/kernel/include \
    -D'errno=(*__errno_location())' \
    -include $(TEST_INC)/gfx_client_errno.h

GFX_CLIENT_TEST_CFLAGS := $(HOST_CFLAGS) \
    -I$(TESTS_DIR)/libgfx -I$(TESTS_DIR)/kernel/include \
    -I$(TEST_INC) \
    -D'errno=(*__errno_location())' \
    -include $(TEST_INC)/gfx_client_errno.h

# Back-compat alias for the existing rule.
GFX_CLIENT_HOST_CFLAGS := $(GFX_CLIENT_PROD_CFLAGS)

$(TEST_BLD)/libgfx_gfx_production.o: $(TESTS_DIR)/libgfx/gfx.c \
		$(TESTS_DIR)/libgfx/gfx.h \
		$(TESTS_DIR)/libgfx/internal.h \
		$(TESTS_DIR)/kernel/include/uapi/gfx.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_CLIENT_HOST_CFLAGS) -c $< -o $@

# The test TU also does NOT see test_platform.h (the OS01 host mock
# surface) — it only uses host libc functions (printf, exit, ...).
# test_framework.h is host-safe (no OS01 headers) and resolves via
# -I$(TEST_INC) above.
$(TEST_BLD)/test_gfx_client.o: $(TEST_CASES)/test_gfx_client.c \
		$(TESTS_DIR)/libgfx/gfx.h \
		$(TESTS_DIR)/kernel/include/uapi/gfx.h \
		$(TEST_INC)/test_framework.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_CLIENT_TEST_CFLAGS) -c $< -o $@

# __real_calloc is generated by --wrap=calloc; declare it explicitly so
# the wrapper compiles without an implicit-declaration warning.
$(TEST_BLD)/test_gfx_client.elf: $(TEST_BLD)/test_gfx_client.o \
		$(TEST_BLD)/libgfx_gfx_production.o
	$(HOST_CC) -Wl,--wrap=open,--wrap=close,--wrap=ioctl,--wrap=malloc,--wrap=calloc,--wrap=free -o $@ $^

test-gfx-client: $(TEST_BLD)/test_gfx_client.elf
	$<

# Gfx primitives test (Task 4, 2D graphics API plan): host-compiles
# the REAL libgfx/line.c + libgfx/sprite.c (the archive functions
# the spec §4 designates for Bresenham and sprite blits) and links
# them through the in-tree libgfx/internal.h so the test can read
# the opaque handle layout directly.  libgfx/gfx.c is NOT compiled
# here (it would drag open/ioctl/close; the primitives test is
# pure in-memory drawing — no fd, no syscall).
#
# The test exercises every primitive spec §4 contract: pixel /
# hline / vline / rect / fill_rect colour correctness, full
# clipping matrix (negative coords, oversized coords, partial
# overlap, complete outside, clip rectangle), Bresenham in all 8
# octants with both endpoints, INT32_MIN/MAX endpoints that must
# not overflow or loop, sprite opaque / color-key=0 (black also
# transparent) / mask MSB-first bit order / padded source and mask
# stride.  Sentinel pixel 0xDEADBEEF wraps the 7x5 grid so any
# out-of-bounds write shows up in an assert.
$(TEST_BLD)/libgfx_line_production.o: $(TESTS_DIR)/libgfx/line.c \
		$(TESTS_DIR)/libgfx/gfx.h \
		$(TESTS_DIR)/libgfx/internal.h \
		$(TESTS_DIR)/kernel/include/uapi/gfx.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_CLIENT_PROD_CFLAGS) -c $< -o $@

$(TEST_BLD)/libgfx_sprite_production.o: $(TESTS_DIR)/libgfx/sprite.c \
		$(TESTS_DIR)/libgfx/gfx.h \
		$(TESTS_DIR)/libgfx/internal.h \
		$(TESTS_DIR)/kernel/include/uapi/gfx.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_CLIENT_PROD_CFLAGS) -c $< -o $@

# Test TU: include path adds the in-tree libgfx/ for internal.h
# (internal.h is NEVER installed to the sysroot; tests reach it
# directly so they can read the handle layout, which is the same
# layout libgfx/gfx.c, libgfx/line.c and libgfx/sprite.c use).
$(TEST_BLD)/test_gfx_primitives.o: $(TEST_CASES)/test_gfx_primitives.c \
		$(TESTS_DIR)/libgfx/gfx.h \
		$(TESTS_DIR)/libgfx/internal.h \
		$(TESTS_DIR)/kernel/include/uapi/gfx.h \
		$(TEST_INC)/test_framework.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_CLIENT_TEST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_gfx_primitives.elf: $(TEST_BLD)/test_gfx_primitives.o \
		$(TEST_BLD)/libgfx_gfx_production.o \
		$(TEST_BLD)/libgfx_line_production.o \
		$(TEST_BLD)/libgfx_sprite_production.o
	$(HOST_CC) -o $@ $^

test-gfx-primitives: $(TEST_BLD)/test_gfx_primitives.elf
	$<

clean:
	rm -rf $(TEST_BLD)

$(TEST_BLD)/m1_vmm_production.o: $(TESTS_DIR)/kernel/memory/vmm.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(M1_CONTRACT_HOST_CFLAGS) $(LIBC_INC) -ffunction-sections -fdata-sections -c $< -o $@
$(TEST_BLD)/test_m1_vmm.o: $(TEST_CASES)/test_m1_vmm.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(M1_CONTRACT_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@
$(TEST_BLD)/test_m1_vmm.elf: $(TEST_BLD)/test_m1_vmm.o $(TEST_BLD)/m1_vmm_production.o
	$(HOST_CC) -Wl,--gc-sections -Wl,--wrap=calloc -o $@ $^

M1_A64_CFLAGS := $(HOST_CFLAGS) -DOS01_HOST_TEST -DOS01_SELFTEST=1 -I$(TEST_MOCK)/m1_a64_include -I$(TEST_MOCK)/m1_include $(KERNEL_INC)
$(TEST_BLD)/m1_install_production.o: $(TESTS_DIR)/kernel/arch/aarch64/memory/boot_direct_map.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(M1_A64_CFLAGS) -c $< -o $@
$(TEST_BLD)/test_m1_install.o: $(TEST_CASES)/test_m1_install.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(M1_A64_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@
$(TEST_BLD)/test_m1_install.elf: $(TEST_BLD)/test_m1_install.o $(TEST_BLD)/m1_install_production.o $(TEST_BLD)/runtime_tree_production.o $(TEST_BLD)/vmm_gate_production.o $(TEST_BLD)/vmm_gate_test_stubs.o
	$(HOST_CC) -o $@ $^
.PHONY: test-m1-install
test-m1-install: $(TEST_BLD)/test_m1_install.elf
	$(TEST_BLD)/test_m1_install.elf
	$(TEST_BLD)/test_m1_cleanup.elf

$(TEST_BLD)/m1_smoke_production.o: $(TESTS_DIR)/kernel/arch/aarch64/memory/m1_smoke.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(M1_A64_CFLAGS) -c $< -o $@
$(TEST_BLD)/test_m1_cleanup.o: $(TEST_CASES)/test_m1_cleanup.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(M1_A64_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@
$(TEST_BLD)/test_m1_cleanup.elf: $(TEST_BLD)/test_m1_cleanup.o $(TEST_BLD)/m1_smoke_production.o
	$(HOST_CC) -no-pie -o $@ $^
test-m1-install: $(TEST_BLD)/test_m1_cleanup.elf

$(TEST_BLD)/m1_selftest_production.o: $(TESTS_DIR)/kernel/arch/aarch64/memory/m1_selftest.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -DOS01_HOST_TEST -DOS01_SELFTEST=1 $(PMM_SHADOW_INC) -I$(TEST_MOCK)/m1_a64_include -I$(TEST_MOCK)/m1_include $(KERNEL_INC) -ffunction-sections -fdata-sections -c $< -o $@
$(TEST_BLD)/test_m1_publish.o: $(TEST_CASES)/test_m1_publish.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(M1_A64_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@
$(TEST_BLD)/test_m1_publish.elf: $(TEST_BLD)/test_m1_publish.o $(TEST_BLD)/m1_selftest_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^
.PHONY: test-m1-publish
test-m1-publish: $(TEST_BLD)/test_m1_publish.elf
	$<

# Every M1 test uses the failure-preserving runner.
$(patsubst %.elf,%.o,$(filter $(TEST_BLD)/test_m1_%.elf,$(TEST_BINS))): $(TEST_INC)/m1_test_runner.h

$(TEST_BLD)/test_m1_runner.elf: $(TEST_CASES)/test_m1_runner.c $(TEST_INC)/m1_test_runner.h $(TEST_INC)/test_framework.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $< -o $@

$(TEST_BLD)/early_arena_exhaust_production.o: $(TESTS_DIR)/kernel/arch/aarch64/memory/early_arena.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(EARLY_ARENA_HOST_CFLAGS) -DAARCH64_M1_ARENA_EXHAUST=1 -c $< -o $@
$(TEST_BLD)/test_m1_arena_exhaust.o: $(TEST_CASES)/test_m1_arena_exhaust.c \
		$(TEST_INC)/m1_test_runner.h \
		$(TESTS_DIR)/kernel/include/memory/slab.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(EARLY_ARENA_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@
$(TEST_BLD)/test_m1_arena_exhaust.elf: $(TEST_BLD)/test_m1_arena_exhaust.o $(TEST_BLD)/early_arena_exhaust_production.o $(TEST_BLD)/pmm_boot_production.o
	$(HOST_CC) -o $@ $^

# aarch64 M2/M3 plan Task 2: slab_init j-loop + 8-page loop
# idempotence vs boot reservations. Links the REAL
# kernel/memory/slab.c + pmm.c so the assertion observes the actual
# ++/-- on each touched frame. Production <arch/spinlock.h> and
# <arch/irq.h> carry privileged inline asm (lock decq / pushfq cli /
# mrs daif), so both are routed through host shadows under
# SLAB_SHADOW_INC; production <percpu/percpu.h> pulls in
# <sched/task.h> + <arch/percpu.h> (both unreachable on host),
# shadowed to a minimal surface. slab.c also pulls in
# <memory/memory.h> / <core/printk.h> / <core/debug.h> / <kernel.h>
# / <string.h> — all delivered by KERNEL_INC + LIBC_INC.
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

# Task 2 Step 3 — x86 baseline regression for slab_init frame arithmetic.
# Compiles the REAL kernel/memory/slab.c (same shadow machinery as
# test_slab_idempotent_reservation; shared slab_color_stub.o for the
# color_printk symbol the slab.c error paths reference). Single-test
# suite so the brief's "assert increment = 8 + j-loop hits" lands
# literally without competing fixtures.
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
