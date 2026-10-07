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
    $(TEST_BLD)/test_terminal_render.elf \
    $(TEST_BLD)/test_terminal_display.elf \
    $(TEST_BLD)/test_tetris_logic.elf \
    $(TEST_BLD)/test_sync_primitives.elf \
    $(TEST_BLD)/test_poll_requested.elf \
    $(TEST_BLD)/test_poll_waitqueue_handshake.elf \
    $(TEST_BLD)/test_bootinfo_abi.elf \
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
    $(TEST_BLD)/test_page_table_layout.elf \
    $(TEST_BLD)/test_page_table_reservation.elf \
    $(TEST_BLD)/test_early_arena.elf $(TEST_BLD)/test_early_arena_exhaust.elf \
    $(TEST_BLD)/test_arena_layout_chain.elf \
    $(TEST_BLD)/test_arena_2gib_boundary.elf \
    $(TEST_BLD)/test_arena_candidate_slab_bytes.elf \
    $(TEST_BLD)/test_arena_table_base_shift.elf \
    $(TEST_BLD)/test_aarch64_color_printk_abi.elf \
    $(TEST_BLD)/test_page_table_tree.elf \
    $(TEST_BLD)/test_page_table_contract.elf $(TEST_BLD)/test_page_table_vmm.elf \
    $(TEST_BLD)/test_page_table_install.elf $(TEST_BLD)/test_page_table_cleanup.elf \
    $(TEST_BLD)/test_page_table_publish.elf \
    $(TEST_BLD)/test_syscall_dispatch.elf \
    $(TEST_BLD)/test_compat_linux.elf \
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
    $(TEST_BLD)/test_aarch64_backend_4k.elf \
    $(TEST_BLD)/test_vmm_replace_protocol.elf \
    $(TEST_BLD)/test_vmm_prot_none.elf \
    $(TEST_BLD)/test_vmm_ack_wraparound.elf \
    $(TEST_BLD)/test_aarch64_pt_root_publish.elf \
    $(TEST_BLD)/test_aarch64_pt_split.elf \
    $(TEST_BLD)/test_smp_starting_gate.elf \
    $(TEST_BLD)/test_aarch64_arch_vmm_init.elf \
    $(TEST_BLD)/test_aarch64_scratch_probe_logic.elf \
    $(TEST_BLD)/test_block_device_ops.elf \
    $(TEST_BLD)/test_pci_driver_model.elf \
    $(TEST_BLD)/test_pci_backend_irq.elf \
    $(TEST_BLD)/test_ahci_lifecycle.elf \
    $(TEST_BLD)/test_device_boot.elf \
    $(TEST_BLD)/test_net_device.elf \
    $(TEST_BLD)/test_net_lwip.elf \
    $(TEST_BLD)/test_e1000_instances.elf \
    $(TEST_BLD)/test_virtio_net_instances.elf \
    $(TEST_BLD)/test_fb_uapi.elf \
    $(TEST_BLD)/test_fb_state.elf \
    $(TEST_BLD)/test_fb_writers.elf \
    $(TEST_BLD)/test_bga_resolution.elf \
    $(TEST_BLD)/test_bga_non_x86.elf \
    $(TEST_BLD)/test_fb_resolution_ioctl.elf \
    $(TEST_BLD)/test_pty_winsize.elf \
    $(TEST_BLD)/test_fb_resolution_hooks.elf \
    $(TEST_BLD)/test_setres.elf

.PHONY: all clean check-test-bins-dup run test-runtime-udivti3 runtime-udivti3-link-probe test_lwip_rand test_gic_driver test_gic_marker_lines test_gic_probe test_clocksource test-libc-fread-fwrite-validate test-libc-stdio-registry test-gfx-client test-gfx-primitives test_vmm_replace_protocol test_vmm_prot_none test_vmm_ack_wraparound test_aarch64_pt_root_publish test_aarch64_pt_split test_aarch64_arch_vmm_init test-block test-pci test-backend test-ahci test-device-boot test-net test-lwip test-e1000 test-virtio test-fb-uapi test-fb-state test-fb-writers test-bga test-fb-ioctl test-fb-hooks test-pty-winsize test-terminal-display test-clients

all: $(TEST_BINS)

check-test-bins-dup:
	@if [ "$$(echo $(TEST_BINS) | tr ' ' '\n' | sort | uniq -d)" != "" ]; then \
	    echo "ERROR: duplicate entries in TEST_BINS"; exit 1; \
	fi

# ── Host suite execution (test-framework plan, Task 7) ─────────────
# `run` no longer greps a binary's stdout: it hands the authoritative
# TEST_BINS list to qemutests/run_hosttests.py, which runs each binary with
# its own timeout and archive and validates the evidence (legacy adapter
# plus protocol v1 for migrated binaries).
#
# Binaries that intentionally print NOTHING on success (they report only on
# failure) cannot satisfy the legacy adapter's "nonempty suite evidence"
# rule — spec §6.2: an empty log cannot pass — so they are not ordinary
# TEST_BINS entries.  They stay in the build and are exercised by an
# explicit exit-status adapter in the recipe below, so `make test-host`
# keeps running them without ever treating an empty log as a pass.
HOST_TEST_TIMEOUT ?= 60

TEST_EXIT_STATUS_BINS := \
    $(TEST_BLD)/test_runtime_udivti3.elf \
    $(TEST_BLD)/test_page_table_runner.elf

run: $(TEST_BINS) $(TEST_EXIT_STATUS_BINS) check-test-bins-dup
	@echo ""
	@echo "=============================================="
	@echo "  OS01 Host-Based Test Suite"
	@echo "=============================================="
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) $(TEST_BINS)
	@echo ""
	@echo "  [hosttests] exit-status-only binaries (no stdout evidence)"
	@for t in $(TEST_EXIT_STATUS_BINS); do \
	    "$$t" >/dev/null 2>&1; rc=$$?; \
	    echo "  -> $$(basename $$t .elf): exit $$rc"; \
	    if [ "$$rc" -ne 0 ]; then \
	        echo "ERROR: $$t expected exit 0, got $$rc"; exit 1; \
	    fi; \
	done
	@echo "  [hosttests] all host test binaries passed"

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

# Focused boundary test links the production dispatcher.
$(TEST_BLD)/test_syscall_dispatch.elf: $(TEST_CASES)/test_syscall_dispatch.c $(TESTS_DIR)/kernel/syscall/dispatch.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) $^ -o $@

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

test-compat-linux: $(TEST_BLD)/test_compat_linux.elf
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

# NOTE: this TU is the *userland* libc formatter, not the kernel-side
# libk subset.  FRAMEWORK_INC force-includes test_platform.h, which
# defines __is_libk; since c85172c5/7ebc9d0a libc/stdio/vsprintf.c gates
# its float (%f/%e/%g) rendering behind `#ifndef __is_libk` (aarch64 libk
# builds with -mgeneral-regs-only and cannot use `double`).  Compiling the
# host test's copy in libk mode would silently strip float support and
# make test_libc_vsprintf assert the wrong thing.  Build it WITHOUT
# __is_libk so the harness exercises the real, full formatter (floatconv
# is linked via $(LIBC_OBJS)).
$(TEST_BLD)/libc_vsprintf.o: $(LIBC_SRC)/stdio/vsprintf.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

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
# KERNEL_INC. test_page_table_layout.c itself only uses <errno.h> from libc,
# <test_framework.h> from TEST_INC, and the production headers above.
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

# `make test-page-table-host CASE=page-table-layout` (root run.mk) reaches this rule
# via the focused dispatch in run.mk.
.PHONY: test-page-table-layout
test-page-table-layout: $(TEST_BLD)/test_page_table_layout.elf $(TEST_BLD)/test_page_table_runner.elf
	$(TEST_BLD)/test_page_table_layout.elf
	$(TEST_BLD)/test_page_table_runner.elf

# aarch64 M1 plan Task 2: range-based boot reservations +
# single-frame exact claim. Links the REAL pmm.c (PMM_SHADOW_INC so
# the production spinlock / arch_cpu_halt resolve to no-op host
# stubs), the REAL pmm_arch.c (weak-default strategy), and the REAL
# pmm_boot.c (layout calculator). The test TU itself does not pull
# in the spinlock header, so it only needs KERNEL_INC + TEST_INC.
#
# `make test-page-table-host CASE=page-table-reservation` (root run.mk) reaches this
# rule via the focused dispatch in run.mk.
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

# aarch64 M1 plan Task 3: early arena preflight planner
# (`aarch64_early_arena_plan` / `aarch64_early_arena_prepare`). Links the REAL
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
# `make test-page-table-host CASE=early-arena` (root run.mk) reaches this rule
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

.PHONY: test-aarch64-color-printk-abi
test-aarch64-color-printk-abi: $(TEST_BLD)/test_aarch64_color_printk_abi.elf
	$(TEST_BLD)/test_aarch64_color_printk_abi.elf

# color_printk / serial_printk ABI + behaviour. Links the REAL shared
# kernel/core/printk.c — the arch-only printk_stub.c was deleted in
# 42aef0fc and the console is now shared by x86_64 and aarch64
# (kernel/Makefile's aarch64 KERNEL_C_SOURCES lists `core/printk.c`).
# The test TU includes the public <core/printk.h> so a signature mismatch
# is a compile error; the production TU is compiled against the same
# header. The serial sink + framebuffer-absent stubs live in
# $(TEST_MOCK)/aarch64_color_printk/printk_capture.c, whose shadow
# <driver/serial.h> comes BEFORE $(KERNEL_INC).
PRINTK_ABI_HOST_CFLAGS := $(HOST_CFLAGS) $(FRAMEWORK_INC) \
	-I$(TEST_MOCK)/aarch64_color_printk $(KERNEL_INC)

$(TEST_BLD)/test_aarch64_color_printk_abi.o: $(TEST_CASES)/test_aarch64_color_printk_abi.c \
		$(TESTS_DIR)/kernel/include/core/printk.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PRINTK_ABI_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/printk_production.o: \
		$(TESTS_DIR)/kernel/core/printk.c \
		$(TESTS_DIR)/kernel/include/core/printk.h \
		$(TESTS_DIR)/kernel/include/driver/serial.h \
		$(TESTS_DIR)/kernel/include/driver/fb_state.h \
		$(TESTS_DIR)/kernel/include/driver/font.h \
		$(TEST_MOCK)/aarch64_color_printk/driver/serial.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PRINTK_ABI_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/printk_capture.o: $(TEST_MOCK)/aarch64_color_printk/printk_capture.c \
		$(TEST_MOCK)/aarch64_color_printk/driver/serial.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) \
		-I$(TEST_MOCK)/aarch64_color_printk $(KERNEL_INC) -c $< -o $@

$(TEST_BLD)/test_aarch64_color_printk_abi.elf: \
		$(TEST_BLD)/test_aarch64_color_printk_abi.o \
		$(TEST_BLD)/printk_production.o \
		$(TEST_BLD)/printk_capture.o
	$(HOST_CC) -o $@ $^

# aarch64 M2/M3 plan Task 3: arena layout-chain / 2 GiB boundary /
# candidate-scan-includes-slab-bytes. Each new TU reuses the production
# early_arena.o via the same EARLY_ARENA_HOST_CFLAGS / link line as the
# existing test_early_arena.elf. The stub surface (g_log_level,
# _log_err_impl, color_printk, slab_init) lives in each TU — keeps the
# rules self-contained and matches test_early_arena.c / _exhaust.c.
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
# Links the REAL early_arena.c (aarch64_early_arena_plan) AND the REAL
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
# `make test-page-table-host CASE=page-table-tree` (root run.mk) reaches this rule
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
# `make test-page-table-host CASE=page-table-contract-x86` (root run.mk) reaches
# this rule via the focused dispatch in run.mk.
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
	$(HOST_CC) -Wl,--wrap=malloc -o $@ $^

# Terminal render test — compiles REAL user/terminal_render.c + terminal_core.c + libgfx
$(TEST_BLD)/terminal_render.o: $(TESTS_DIR)/user/terminal_render.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/user -I$(TESTS_DIR)/libgfx -I$(TESTS_DIR)/kernel/include -c $< -o $@

$(TEST_BLD)/test_terminal_render.o: $(TEST_CASES)/test_terminal_render.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/user -I$(TESTS_DIR)/libgfx -I$(TESTS_DIR)/kernel/include -DREPO_ROOT=\"$(TESTS_DIR)\" -c $< -o $@

$(TEST_BLD)/test_terminal_render.elf: $(TEST_BLD)/test_terminal_render.o \
		$(TEST_BLD)/terminal_render.o \
		$(TEST_BLD)/terminal_core.o \
		$(TEST_BLD)/libgfx_gfx_production.o \
		$(TEST_BLD)/libgfx_line_production.o \
		$(TEST_BLD)/libgfx_sprite_production.o
	$(HOST_CC) -o $@ $^

test-terminal-render: $(TEST_BLD)/test_terminal_render.elf
	$<

# Terminal display transaction test — REAL user/terminal_display.c with an
# injected libgfx/FBIOGET_STATE backend; malloc is wrapped for ENOMEM injection.
# -DFB_RESOLUTION_TEST compiles the Task-9 fbtest ENOMEM hook so
# test_terminal_fbtest_enomem_once can drive it; the tests always install a
# stub op, so the real /dev/fbtest path is never taken on the host.
$(TEST_BLD)/terminal_display.o: $(TESTS_DIR)/user/terminal_display.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_CLIENT_HOST_CFLAGS) -DFB_RESOLUTION_TEST -I$(TEST_INC) -I$(TESTS_DIR)/user -c $< -o $@

$(TEST_BLD)/test_terminal_display.o: $(TEST_CASES)/test_terminal_display.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_CLIENT_HOST_CFLAGS) -I$(TEST_INC) -I$(TESTS_DIR)/user -c $< -o $@

$(TEST_BLD)/test_terminal_display.elf: $(TEST_BLD)/test_terminal_display.o \
		$(TEST_BLD)/terminal_display.o \
		$(TEST_BLD)/terminal_render.o \
		$(TEST_BLD)/terminal_core.o \
		$(TEST_BLD)/libgfx_gfx_production.o \
		$(TEST_BLD)/libgfx_line_production.o \
		$(TEST_BLD)/libgfx_sprite_production.o
	$(HOST_CC) -Wl,--wrap=malloc -o $@ $^

test-terminal-display: $(TEST_BLD)/test_terminal_core.elf $(TEST_BLD)/test_terminal_render.elf $(TEST_BLD)/test_terminal_display.elf
	$(TEST_BLD)/test_terminal_core.elf
	$(TEST_BLD)/test_terminal_render.elf
	$(TEST_BLD)/test_terminal_display.elf

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
# OS01_SELFTEST=1: this harness links the REAL page_table.c, whose
# aarch64_pt_read_l2_desc test helper (declaration + definition) is
# compiled only under OS01_SELFTEST — the same guard the kernel
# selftest build (-DKERNEL_SELFTEST → -DOS01_SELFTEST, kernel/Makefile)
# and kernel/random/random.c's test hook use.  No other header reachable
# from this harness carries OS01_SELFTEST-guarded code, so the define
# only exposes the test-only introspection surface.
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

# aarch64 M3.3 Task 19: 4-class replace protocol matrix (spec §4.4.3)
# with descriptor-level state assertions. Reuses the AARCH64_PT_SW
# harness and adds a backend layer test (arch_vmm_update_4k routing)
# + the memtype-flip (AttrIndx Normal↔Device) class that Task 16
# deferred. The brief requires linking REAL page_table.c +
# vmm_backend.c; the production objects are already built under
# $(TEST_BLD) by the rules above and re-used here.
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

test_vmm_replace_protocol: $(TEST_BLD)/test_vmm_replace_protocol.elf
	$<

# aarch64 M3.3 Task 19: PROT_NONE three-state contract via the
# *backend* layer (spec §4.3 / §4.4.4). The primitive layer is
# already pinned by test_aarch64_pt_software_bits.c; this file
# pins the same contract through arch_vmm_query_4k /
# arch_vmm_unmap_4k / arch_vmm_update_4k so vmm_backend.c
# regressions in vm_to_sw / perm_to_vm surface independently.
# Same harness as test_vmm_replace_protocol.c (links the SAME
# production objects).
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

test_vmm_prot_none: $(TEST_BLD)/test_vmm_prot_none.elf
	$<

# aarch64 M3.3 Task 19: ack generation algebraic wraparound contract
# (spec §6.4 step 4 / §8.2). Phase A (gen=UINT32_MAX → 0, handler
# fires → OK) is partially covered by test_tlb_serial_protocol.c
# case_wraparound; this file pins BOTH phases on a fresh test root
# so the wraparound guarantee is self-contained. Phase B (handler
# stuck → FATAL) is the load-bearing half — a `<`-wait regression
# would let Phase A and the generic timeout case pass while
# silently mis-handling a real SMP shootdown against a CPU whose
# gen is at UINT32_MAX. Uses the same harness as
# test_tlb_serial_protocol.c (preinclude mock/tlb_test_runtime.h,
# longjmp ping_test_override).
$(TEST_BLD)/test_vmm_ack_wraparound.o: \
		$(TEST_CASES)/test_vmm_ack_wraparound.c \
		$(TEST_MOCK)/tlb_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(TLB_SD_HOST_CFLAGS) -I$(TEST_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_vmm_ack_wraparound.elf: \
		$(TEST_BLD)/test_vmm_ack_wraparound.o \
		$(TEST_BLD)/tlb_production.o
	$(HOST_CC) -o $@ $^

test_vmm_ack_wraparound: $(TEST_BLD)/test_vmm_ack_wraparound.elf
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

# M3.5 Task 23: smp_starting gate + ipi_ready_publish_and_count integration
# (BSP + AP both publish through the SAME one-shot function). Links the
# REAL kernel/arch/aarch64/memory/vmm_gate.c (smp_starting, ipi_ready_count,
# vmm_gate_check) and source-scans kernel/arch/aarch64/memory/{page_table,
# vmm_backend}.c to confirm every public aarch64_pt_*/arch_vmm_* entry has
# vmm_gate_check() as the first meaningful statement (or is a documented
# exception: init, encoder, or _ext delegator wrapper). The atomic per-
# primitive semantics are pinned by test_foundational_primitives.c (21/21);
# the BSP/AP publish order is pinned by test_double_state_publish.c — this
# file extends both into the BSP+AP end-to-end flow and pins the gate-first
# contract on the VMM surface.
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

test_smp_starting_gate: $(TEST_BLD)/test_smp_starting_gate.elf
	$<

# M3.5 Task 24: arch_vmm_init production entry contract. Links the REAL
# page_table.c + REAL vmm_backend.c against a configurable
# aarch64_read_ttbr1 mock (g_mock_ttbr1). The test asserts the 5
# documented paths: valid → rc 0 + kernel_map pinned; pa=0 (raw or
# post-mask), unaligned, at-1TiB, above-1TiB → rc -EINVAL + kernel_map
# untouched. Reuses the AARCH64_PT_SW harness
# (page_table_aarch64_include/arch/mmu.h,spinlock.h stubs).
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

test_aarch64_arch_vmm_init: $(TEST_BLD)/test_aarch64_arch_vmm_init.elf
	$<

# M3.5 Task 25: M3-SHOOTDOWN-PROBE 7-step body harness (spec §7.3).
# The probe body lives as static inline in
# kernel/arch/aarch64/boot/shootdown_probe.h — one source of truth shared
# between the production TU (kernel/arch/aarch64/boot/shootdown_probe.c,
# which only provides the ops binding + the public entry point) and
# this hosttest TU. We do NOT link the production .c (it pulls in
# arch_vmm_*, percpu_data[], tlb_shootdown, etc. that the host has no
# surface of). The test compiles the same body via #include <arch/
# aarch64/boot/shootdown_probe.h>; we still need arch/mmu.h for the static
# header's ARCH_PAGE_OFFSET use.
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

test_aarch64_scratch_probe_logic: $(TEST_BLD)/test_aarch64_scratch_probe_logic.elf
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
	$(HOST_CC) -Wl,--wrap=open,--wrap=close,--wrap=ioctl,--wrap=malloc,--wrap=calloc,--wrap=free,--wrap=mmap,--wrap=munmap -o $@ $^

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

# setres CLI + long-running gfx client policy (QEMU resolution switcher
# Task 8).  Host-compiles the REAL user/setres_parse.c, user/setres.c
# (with -DSETRES_NO_MAIN so this TU owns main) and
# user/gfx_client_policy.c.  libc open/close/ioctl are wrapped so the
# test can assert the CLI's exact device ops (help opens NOTHING) and
# inject GET_MODES / GET_STATE / SET_MODE results.
#
# The errno ABI shim (GFX_CLIENT_*_CFLAGS) is reused verbatim from the
# libgfx client test: kernel/include is on the path first for
# <uapi/fb.h> and its freestanding <errno.h>, so `errno` must be
# redirected to glibc's TLS accessor.  user/ is added for the
# "setres_parse.h" / "gfx_client_policy.h" includes.
SETRES_HOST_CFLAGS := $(GFX_CLIENT_PROD_CFLAGS) -I$(TESTS_DIR)/user

$(TEST_BLD)/gfx_client_policy.o: $(TESTS_DIR)/user/gfx_client_policy.c \
		$(TESTS_DIR)/user/gfx_client_policy.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SETRES_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/setres_parse.o: $(TESTS_DIR)/user/setres_parse.c \
		$(TESTS_DIR)/user/setres_parse.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SETRES_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/setres_cli.o: $(TESTS_DIR)/user/setres.c \
		$(TESTS_DIR)/user/setres_parse.h \
		$(TESTS_DIR)/kernel/include/uapi/fb.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SETRES_HOST_CFLAGS) -DSETRES_NO_MAIN -c $< -o $@

$(TEST_BLD)/test_setres.o: $(TEST_CASES)/test_setres.c \
		$(TESTS_DIR)/user/gfx_client_policy.h \
		$(TESTS_DIR)/user/setres_parse.h \
		$(TESTS_DIR)/kernel/include/uapi/fb.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_CLIENT_TEST_CFLAGS) -I$(TESTS_DIR)/user -D_GNU_SOURCE -c $< -o $@

$(TEST_BLD)/test_setres.elf: $(TEST_BLD)/test_setres.o \
		$(TEST_BLD)/setres_cli.o \
		$(TEST_BLD)/setres_parse.o \
		$(TEST_BLD)/gfx_client_policy.o
	$(HOST_CC) -Wl,--wrap=open,--wrap=close,--wrap=ioctl -o $@ $^

test-clients: $(TEST_BLD)/test_setres.elf
	$<

clean:
	rm -rf $(TEST_BLD)

$(TEST_BLD)/page_table_vmm_production.o: $(TESTS_DIR)/kernel/memory/vmm.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PT_CONTRACT_HOST_CFLAGS) $(LIBC_INC) -ffunction-sections -fdata-sections -c $< -o $@
$(TEST_BLD)/test_page_table_vmm.o: $(TEST_CASES)/test_page_table_vmm.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PT_CONTRACT_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@
$(TEST_BLD)/test_page_table_vmm.elf: $(TEST_BLD)/test_page_table_vmm.o $(TEST_BLD)/page_table_vmm_production.o
	$(HOST_CC) -Wl,--gc-sections -Wl,--wrap=calloc -o $@ $^

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

# aarch64 M3.4 Task 20: root publish registry split-protocol contract.
# Reuses the PT_A64_CFLAGS / page_table_install_production.o (REAL
# boot_direct_map.c) + runtime_tree_production.o + vmm_gate_production.o
# + vmm_gate_test_stubs.o link line so the runtime tests drive the
# actual install path and observe the registry state (post-init
# is_published for the M1 root; NOT-published for a scratch root).
# Source-scan tests (read boot_direct_map.c via OS01_KERNEL_SRC) pin
# the publish-before-install order and the publish-failure → goto fail
# (no install_ttbr1 in the failure branch) contract. Cases 1–3
# (registry semantics: idempotent / overflow / zero-PA) are pinned by
# test_foundational_primitives.c (21/21) and NOT duplicated here.
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

# aarch64 M3.4 Task 21: split_block_2m protocol (spec §5.3).
# Reuses the same link line as test_aarch64_pt_root_publish — REAL
# page_table.c (split primitive) + REAL vmm_gate.c (root publish
# registry, so the published-root test can drive the real is_published
# check). The mock surface is the same AARCH64_PT_SW host harness
# (aarch64_read_ttbr1=0 / vmm_gate_check no-op / mock PMM pool).
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

$(TEST_BLD)/page_table_smoke_production.o: $(TESTS_DIR)/kernel/arch/aarch64/memory/page_table_smoke.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PT_A64_CFLAGS) -c $< -o $@
$(TEST_BLD)/test_page_table_cleanup.o: $(TEST_CASES)/test_page_table_cleanup.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PT_A64_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@
$(TEST_BLD)/test_page_table_cleanup.elf: $(TEST_BLD)/test_page_table_cleanup.o $(TEST_BLD)/page_table_smoke_production.o
	$(HOST_CC) -no-pie -o $@ $^
test-page-table-install: $(TEST_BLD)/test_page_table_cleanup.elf

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

# Every M1 test uses the failure-preserving runner.
$(patsubst %.elf,%.o,$(filter $(TEST_BLD)/test_page_table_%.elf,$(TEST_BINS))): $(TEST_INC)/page_table_test_runner.h

$(TEST_BLD)/test_page_table_runner.elf: $(TEST_CASES)/test_page_table_runner.c $(TEST_INC)/page_table_test_runner.h $(TEST_INC)/test_framework.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $< -o $@

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

test-device-boot: $(TEST_BLD)/test_device_boot.elf
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

test-virtio: $(TEST_BLD)/test_virtio_net_instances.elf
	$<

# Framebuffer mode control ABI layout test (QEMU resolution switcher Task 1)
$(TEST_BLD)/test_fb_uapi.o: $(TEST_CASES)/test_fb_uapi.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(KERNEL_INC) -I$(TEST_INC) -c $< -o $@

$(TEST_BLD)/test_fb_uapi.elf: $(TEST_BLD)/test_fb_uapi.o
	$(HOST_CC) -o $@ $^

test-fb-uapi: $(TEST_BLD)/test_fb_uapi.elf
	$<

# Framebuffer state coordinator & checked mapping test (QEMU resolution switcher Task 2)
FB_STATE_HOST_CFLAGS := $(HOST_CFLAGS) -DOS01_HOST_TEST -I$(TEST_MOCK) -include $(TEST_MOCK)/fb_resolution_runtime.h $(KERNEL_INC) -I$(TEST_INC)

$(TEST_BLD)/fb_resolution_runtime.o: $(TEST_MOCK)/fb_resolution_runtime.c $(TEST_MOCK)/fb_resolution_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -DOS01_HOST_TEST -I$(TEST_MOCK) $(KERNEL_INC) -I$(TEST_INC) -c $< -o $@

$(TEST_BLD)/fb_state.o: $(TESTS_DIR)/kernel/driver/fb_state.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_STATE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/fb_map.o: $(TESTS_DIR)/kernel/arch/x86_64/memory/fb_map.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_STATE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_fb_state.o: $(TEST_CASES)/test_fb_state.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_STATE_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_fb_state.elf: $(TEST_BLD)/test_fb_state.o $(TEST_BLD)/fb_state.o $(TEST_BLD)/fb_map.o $(TEST_BLD)/fb_resolution_runtime.o
	$(HOST_CC) -lpthread -o $@ $^

test-fb-state: $(TEST_BLD)/test_fb_state.elf
	$<

# Framebuffer writer admission, leases, and raw mmap rollback test (QEMU resolution switcher Task 3)
FB_WRITERS_HOST_CFLAGS := $(HOST_CFLAGS) -DOS01_HOST_TEST -I$(TEST_MOCK) -include $(TEST_MOCK)/fb_resolution_runtime.h $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)

$(TEST_BLD)/gfx_production.o: $(TESTS_DIR)/kernel/driver/gfx.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_WRITERS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/fb_production.o: $(TESTS_DIR)/kernel/driver/fb.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_WRITERS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/console_production.o: $(TESTS_DIR)/kernel/tty/console.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_WRITERS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_fb_writers.o: $(TEST_CASES)/test_fb_writers.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_WRITERS_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_fb_writers.elf: $(TEST_BLD)/test_fb_writers.o $(TEST_BLD)/fb_state.o $(TEST_BLD)/fb_map.o $(TEST_BLD)/fb_resolution_runtime.o $(TEST_BLD)/gfx_production.o $(TEST_BLD)/fb_production.o $(TEST_BLD)/console_production.o
	$(HOST_CC) -lpthread -o $@ $^

test-fb-writers: $(TEST_BLD)/test_fb_writers.elf
	$<

# PCI BGA backend probe and hardware transaction test (QEMU resolution switcher Task 4)
$(TEST_BLD)/bga_production.o: $(TESTS_DIR)/kernel/driver/bga.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_STATE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/bga_non_x86.o: $(TESTS_DIR)/kernel/driver/bga.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_STATE_HOST_CFLAGS) -DOS01_HOST_TEST_NON_X86 -c $< -o $@

$(TEST_BLD)/test_bga_resolution.o: $(TEST_CASES)/test_bga_resolution.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_STATE_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_bga_resolution.elf: $(TEST_BLD)/test_bga_resolution.o $(TEST_BLD)/bga_production.o $(TEST_BLD)/fb_state.o $(TEST_BLD)/fb_map.o $(TEST_BLD)/fb_resolution_runtime.o $(TEST_BLD)/pci_match_production.o
	$(HOST_CC) -lpthread -o $@ $^

$(TEST_BLD)/test_bga_non_x86.o: $(TEST_CASES)/test_bga_non_x86.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_STATE_HOST_CFLAGS) $(FRAMEWORK_INC) -DOS01_HOST_TEST_NON_X86 -c $< -o $@

$(TEST_BLD)/test_bga_non_x86.elf: $(TEST_BLD)/test_bga_non_x86.o $(TEST_BLD)/bga_non_x86.o $(TEST_BLD)/fb_state.o $(TEST_BLD)/fb_map.o $(TEST_BLD)/fb_resolution_runtime.o $(TEST_BLD)/pci_match_production.o
	$(HOST_CC) -lpthread -o $@ $^

test-bga: $(TEST_BLD)/test_bga_resolution.elf $(TEST_BLD)/test_bga_non_x86.elf
	$(TEST_BLD)/test_bga_resolution.elf
	$(TEST_BLD)/test_bga_non_x86.elf

# Framebuffer safe ioctl and SET state machine test (QEMU resolution switcher Task 5)
$(TEST_BLD)/test_fb_resolution_ioctl.o: $(TEST_CASES)/test_fb_resolution_ioctl.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_WRITERS_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_fb_resolution_ioctl.elf: $(TEST_BLD)/test_fb_resolution_ioctl.o $(TEST_BLD)/fb_state.o $(TEST_BLD)/fb_map.o $(TEST_BLD)/fb_resolution_runtime.o $(TEST_BLD)/fb_production.o $(TEST_BLD)/console_production.o $(TEST_BLD)/bga_production.o $(TEST_BLD)/pci_match_production.o
	$(HOST_CC) -lpthread -o $@ $^

test-fb-ioctl: $(TEST_BLD)/test_fb_resolution_ioctl.elf
	$<

# Controlled fault-injection surface (QEMU resolution switcher Task 9).
# Compiles the REAL kernel/driver/fb_test.c together with fb_state.c /
# bga.c / fb.c built with -DFB_RESOLUTION_TEST, so the armed-token
# consumption, readback filtering, lease hold and terminal-ENOMEM
# registry are all exercised end to end against the fake BGA transport.
# fb_state.c / bga.c / fb.c get distinct object names because their
# normal FB_STATE/FB_WRITERS objects are built WITHOUT the macro.
FB_HOOKS_HOST_CFLAGS := $(FB_WRITERS_HOST_CFLAGS) -DFB_RESOLUTION_TEST

$(TEST_BLD)/fb_state_hooks.o: $(TESTS_DIR)/kernel/driver/fb_state.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_HOOKS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/bga_hooks.o: $(TESTS_DIR)/kernel/driver/bga.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_HOOKS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/fb_test.o: $(TESTS_DIR)/kernel/driver/fb_test.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_HOOKS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_fb_resolution_hooks.o: $(TEST_CASES)/test_fb_resolution_hooks.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_HOOKS_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_fb_resolution_hooks.elf: $(TEST_BLD)/test_fb_resolution_hooks.o \
		$(TEST_BLD)/fb_state_hooks.o $(TEST_BLD)/bga_hooks.o \
		$(TEST_BLD)/fb_test.o $(TEST_BLD)/fb_map.o \
		$(TEST_BLD)/fb_resolution_runtime.o $(TEST_BLD)/fb_production.o \
		$(TEST_BLD)/console_production.o $(TEST_BLD)/pci_match_production.o
	$(HOST_CC) -lpthread -o $@ $^

test-fb-hooks: $(TEST_BLD)/test_fb_resolution_hooks.elf
	$<

# PTY master window resize notifications (QEMU resolution switcher Task 6)
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

test-pty-winsize: $(TEST_BLD)/test_pty_winsize.elf
	$<


