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
    $(TEST_BLD)/test_lapic_nmi.elf \
    $(TEST_BLD)/test_libc_string.elf \
    $(TEST_BLD)/test_libc_list.elf \
    $(TEST_BLD)/test_libc_vsprintf.elf \
    $(TEST_BLD)/test_libc_fflush.elf \
    $(TEST_BLD)/test_libc_stdio_registry.elf \
    $(TEST_BLD)/test_libc_fread_fwrite_validate.elf \
    $(TEST_BLD)/test_libc_getopt.elf \
    $(TEST_BLD)/test_vfs_basic.elf \
    $(TEST_BLD)/test_vfs_fs_registry.elf \
    $(TEST_BLD)/test_vfs_iops_fops.elf \
    $(TEST_BLD)/test_vfs_mount_tree.elf \
    $(TEST_BLD)/test_vfs_dcache.elf \
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
    $(TEST_BLD)/test_user_log.elf \
    $(TEST_BLD)/test_user_log_off.elf \
    $(TEST_BLD)/test_tetris_logic.elf \
    $(TEST_BLD)/test_sync_primitives.elf \
    $(TEST_BLD)/test_mutex_handshake.elf \
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
    $(TEST_BLD)/test_gfx_fault_bridge.elf \
    $(TEST_BLD)/test_monitor_file.elf \
    $(TEST_BLD)/test_monitor_accounting.elf \
    $(TEST_BLD)/test_monitor_pmm.elf \
    $(TEST_BLD)/test_monitor_collect.elf \
    $(TEST_BLD)/test_monitor_model.elf \
    $(TEST_BLD)/test_monitor_render.elf \
    $(TEST_BLD)/test_thermal_fixup.elf \
    $(TEST_BLD)/test_thermal_affinity.elf \
    $(TEST_BLD)/test_thermal_decode.elf \
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
    $(TEST_BLD)/test_setres.elf \
    $(TEST_BLD)/test_display_owner.elf \
    $(TEST_BLD)/test_gui_platform.elf \
    $(TEST_BLD)/test_gui_lvgl_integration.elf \
    $(TEST_BLD)/test_devfs_input_dispatch.elf \
    $(TEST_BLD)/test_input_ps2_frame.elf \
    $(TEST_BLD)/test_input_route.elf \
    $(TEST_BLD)/test_gfx_input.elf \
    $(TEST_BLD)/test_gui_input_decode.elf \
    $(TEST_BLD)/test_gui_input_nonce.elf \
    $(TEST_BLD)/test_gui_input_platform.elf \
    $(TEST_BLD)/test_gui_input_lvgl.elf \
    $(TEST_BLD)/test_gui_theme.elf \
    $(TEST_BLD)/test_gui_theme_apply.elf \
    $(TEST_BLD)/test_desktop_state.elf \
    $(TEST_BLD)/test_desktop_shell.elf \
    $(TEST_BLD)/test_desktop_audit.elf \
    $(TEST_BLD)/test_gui_widget.elf \
    $(TEST_BLD)/test_intr_decode_x86.elf \
    $(TEST_BLD)/test_intr_decode_aarch64.elf \
    $(TEST_BLD)/test_intr_dispatch.elf \
    $(TEST_BLD)/test_intr_backend_aarch64.elf \
    $(TEST_BLD)/test_intr_backend_x86.elf \
    $(TEST_BLD)/test_intr_irq_x86.elf \
    $(TEST_BLD)/test_x86_intr_probe.elf

# Binaries that report solely through their exit code and print nothing on
# success (see the `run` section for the full rationale).  Defined here, NOT
# next to `run`, because `all` below lists them as prerequisites and Make
# expands an explicit rule's prerequisite list where it is parsed — a
# definition further down would be empty at that point.
TEST_EXIT_STATUS_BINS := \
    $(TEST_BLD)/test_runtime_udivti3.elf \
    $(TEST_BLD)/test_page_table_runner.elf

.PHONY: all clean check-test-bins-dup run test-runtime-udivti3 runtime-udivti3-link-probe test_lwip_rand test_gic_driver test_gic_marker_lines test_gic_probe test_clocksource test-libc-fread-fwrite-validate test-libc-stdio-registry test-gfx-client test-gfx-primitives test_vmm_replace_protocol test_vmm_prot_none test_vmm_ack_wraparound test_aarch64_pt_root_publish test_aarch64_pt_split test_aarch64_arch_vmm_init test-block test-pci test-backend test-ahci test-device-boot test-net test-lwip test-e1000 test-virtio test-fb-uapi test-fb-state test-fb-writers test-bga test-fb-ioctl test-fb-hooks test-pty-winsize test-terminal-display test-user-log test-clients test-display-owner test-framework-contract test-gui-platform test-gui-lvgl test-gui-devfs-input test-gui-input-frame test-gui-input-route test-gui-gfx-input test-gui-input-decode test-gui-input-nonce test-gui-input-platform test-gui-input-lvgl

all: $(TEST_BINS) $(TEST_EXIT_STATUS_BINS)

check-test-bins-dup:
	@if [ "$$(echo $(TEST_BINS) $(TEST_EXIT_STATUS_BINS) | tr ' ' '\n' | sort | uniq -d)" != "" ]; then \
	    echo "ERROR: duplicate entries in TEST_BINS/TEST_EXIT_STATUS_BINS"; exit 1; \
	fi

# ── Host suite execution (test-framework plan, Task 7) ─────────────
# `run` no longer greps a binary's stdout: it hands the authoritative
# TEST_BINS list to qemutests/run_hosttests.py, which runs each binary with
# its own timeout and archive and validates the evidence (legacy adapter
# plus protocol v1 for migrated binaries).
#
# Two binaries report solely through their exit code and print NOTHING on
# success, so they cannot satisfy the legacy adapter's "nonempty suite
# evidence" rule — spec §6.2: an empty log cannot pass.  They are listed in
# TEST_EXIT_STATUS_BINS and named to the runner via `--exit-status`, which
# runs them through the SAME ProcessSession/archive path as everything else
# (per-binary timeout; stdout.log / stderr.log / result.json still written)
# and accepts "exit 0 with empty stdout" for these ids ALONE.  Every other
# binary keeps the strict rule: an empty log is a FAIL.  Both groups run in
# one runner invocation, so a failure in either never hides the other.
#
#   * test_page_table_runner — a framework meta-test: it forks a child that
#     fails on purpose and passes only when the failure propagates
#     (WEXITSTATUS(child) == 1); the child's buffered output is discarded.
#   * test_runtime_udivti3 — an ordinary __udivti3 correctness fixture
#     (runtime/selftest/test_udivti3.c) that prints only on a vector
#     mismatch.  It is NOT a framework meta-test; rc=0 with no output is
#     its pass signal.
HOST_TEST_TIMEOUT ?= 60

# One `--exit-status <id>` per exempted binary (id = basename without .elf).
EXIT_STATUS_FLAGS := $(foreach b,$(TEST_EXIT_STATUS_BINS),--exit-status $(basename $(notdir $(b))))

run: $(TEST_BINS) $(TEST_EXIT_STATUS_BINS) check-test-bins-dup
	@echo ""
	@echo "=============================================="
	@echo "  OS01 Host-Based Test Suite"
	@echo "=============================================="
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) $(EXIT_STATUS_FLAGS) \
	    $(TEST_BINS) $(TEST_EXIT_STATUS_BINS)
	@echo "  [hosttests] all host test binaries passed"

# ── Negative-contract binary: framework exit-code integrity ─────────
# test_framework_contract.c is deliberately NOT in TEST_BINS or
# TEST_EXIT_STATUS_BINS — both runners expect exit 0.  This binary
# pins three test-framework contracts by being expected to FAIL:
#
#   1. A failing assertion propagates to the process exit status
#      (TEST_RESULTS() must NOT reset __test_stats; see commit
#      2d594ce0 for the original fix).
#   2. Assertion macros evaluate their arguments exactly once
#      (assert_eq / assert_str_eq / assert_mem_eq single-evaluation).
#   3. TEST_RESULTS() is a pure snapshot — it must not mutate the
#      stats, so a second consecutive summary reports the same totals.
#
# The Make target asserts the binary exits with code 1 (the contract
# for the framework).  Any future regression that breaks the contract
# flips the exit to 0 (or another code) and this target fails.
$(TEST_BLD)/test_framework_contract.elf: $(TEST_CASES)/test_framework_contract.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $(TEST_BLD)/test_framework_contract.o
	$(HOST_CC) -o $@ $(TEST_BLD)/test_framework_contract.o

test-framework-contract: $(TEST_BLD)/test_framework_contract.elf
	@echo "  [framework-contract] running $< (must exit EXACTLY 1: negative contract)"
	@out=`$<`; rc=$$?; \
	 if [ $$rc -eq 1 ]; then \
	     echo "  [framework-contract] PASS: exit 1 (assertion exit-code contract holds)"; \
	 else \
	     echo "  [framework-contract] FAIL: expected exit 1, got $$rc"; \
	     echo "$$out" | sed 's/^/    /'; \
	     case $$rc in \
	         0) echo "    hint: assertions no longer propagate (TEST_RESULTS may be resetting __test_stats)";; \
	         2) echo "    hint: assertion macro evaluated its argument more than once";; \
	         3) echo "    hint: TEST_RESULTS() reset __test_stats (the pre-2d594ce0 bug)";; \
	         *) echo "    hint: unexpected exit code; see test_framework_contract.c header for the contract";; \
	     esac; \
	     exit 1; \
	 fi

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

$(TEST_BLD)/test_vfs_fs_registry.o: $(TEST_CASES)/test_vfs_fs_registry.c \
		$(TESTS_DIR)/kernel/include/fs/vfs.h \
		$(TEST_MOCK)/vfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(VFS_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

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

# ── Interrupt dispatch phase 1, Task 1: normalized event decoders ──
# The intr host-test rules below list EVERY production header their object
# actually includes (transitively) as an explicit prerequisite.  This file
# has no header-dependency tracking (no -MMD), so an omitted header means an
# edit to it silently reuses a stale object — the stale-object false green
# this list exists to prevent.  The sets below were derived from the real
# include closure (`clang -M` with each rule's own flags), not guessed.
#
# NOTE: <arch/irq.h> is deliberately NOT listed anywhere below — none of the
# intr host-test objects include it (verified with `clang -M`); it is pulled
# in by kernel-side TUs (bad_event.c, trap.c, irq.c) that are not compiled
# here.  <arch/regs.h> IS listed, and the per-arch regs.h it selects
# (arch/x86_64/regs.h on the x86_64 host, arch/aarch64/regs.h through the
# aarch64 mock shim) is listed alongside it.
#
# The x86_64 decoder is compiled against the production arch/regs.h
# facade, which selects arch/x86_64/regs.h on an x86_64 host.
X86_DECODE_HOST_CFLAGS := $(HOST_CFLAGS) $(FRAMEWORK_INC) $(KERNEL_INC) \
                          $(LIBC_INC)

# The aarch64 decoder needs the PRODUCTION aarch64 pt_regs_t even
# though the host is x86_64.  mock/intr_dispatch/aarch64/arch/regs.h
# shadows the arch/regs.h facade (which would otherwise select the x86
# layout through __x86_64__) and re-includes arch/aarch64/regs.h; the
# mock directory therefore has to precede KERNEL_INC on the include
# path, and the production per-arch header is still resolved from
# kernel/include because the mock tree has no arch/aarch64/ directory.
A64_DECODE_HOST_CFLAGS := $(HOST_CFLAGS) $(FRAMEWORK_INC) \
                          -I$(TEST_MOCK)/intr_dispatch/aarch64 \
                          $(KERNEL_INC) $(LIBC_INC)

$(TEST_BLD)/x86_intr_decode_production.o: \
		$(TESTS_DIR)/kernel/arch/x86_64/intr/intr_decode.c \
		$(TESTS_DIR)/kernel/include/arch/x86_64/intr_decode.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/intr_vectors.h \
		$(TESTS_DIR)/kernel/include/intr/dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/regs.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(X86_DECODE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_intr_decode_x86.o: $(TEST_CASES)/test_intr_decode_x86.c \
		$(TESTS_DIR)/kernel/include/arch/x86_64/intr_decode.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/intr_vectors.h \
		$(TESTS_DIR)/kernel/include/intr/dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/regs.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(X86_DECODE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_intr_decode_x86.elf: $(TEST_BLD)/test_intr_decode_x86.o \
		$(TEST_BLD)/x86_intr_decode_production.o
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/aarch64_intr_decode_production.o: \
		$(TESTS_DIR)/kernel/arch/aarch64/intr/intr_decode.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/intr_decode.h \
		$(TESTS_DIR)/kernel/include/intr/dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/sync_fault.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/regs.h \
		$(TEST_MOCK)/intr_dispatch/aarch64/arch/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(A64_DECODE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_intr_decode_aarch64.o: \
		$(TEST_CASES)/test_intr_decode_aarch64.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/intr_decode.h \
		$(TESTS_DIR)/kernel/include/intr/dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/regs.h \
		$(TEST_MOCK)/intr_dispatch/aarch64/arch/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(A64_DECODE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_intr_decode_aarch64.elf: \
		$(TEST_BLD)/test_intr_decode_aarch64.o \
		$(TEST_BLD)/aarch64_intr_decode_production.o
	$(HOST_CC) -o $@ $^

# ── Interrupt dispatch phase 1, Task 2: common stateless dispatcher ──
# The test TU links the PRODUCTION dispatcher (kernel/intr/dispatch.c)
# and supplies STRONG spy definitions of the three architecture
# backends (arch_intr_handle_sync / arch_intr_handle_irq /
# arch_intr_bad_event).  The per-architecture bad_event.c backends are
# NOT linked here: they mask interrupts and park the CPU forever, which
# is not host-testable — the spy reproduces the same non-returning
# contract with longjmp.
#
# The production dispatcher includes <arch/intr.h>, which pulls in
# <intr/dispatch.h> and the arch/regs.h facade.  On an x86_64 HOST that
# facade selects the x86_64 pt_regs_t, so both TUs below agree on the
# frame layout.  libc/include is added with -idirafter (like the mutex
# harness) so the host's <setjmp.h> wins over the kernel libc's.
INTR_DISPATCH_HOST_CFLAGS := $(HOST_CFLAGS) $(FRAMEWORK_INC) $(KERNEL_INC) \
                             -idirafter $(TESTS_DIR)/libc/include

$(TEST_BLD)/intr_dispatch_production.o: $(TESTS_DIR)/kernel/intr/dispatch.c \
		$(TESTS_DIR)/kernel/include/intr/dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/intr.h \
		$(TESTS_DIR)/kernel/include/arch/regs.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(INTR_DISPATCH_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_intr_dispatch.o: $(TEST_CASES)/test_intr_dispatch.c \
		$(TESTS_DIR)/kernel/include/intr/dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/intr.h \
		$(TESTS_DIR)/kernel/include/arch/regs.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(INTR_DISPATCH_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_intr_dispatch.elf: \
		$(TEST_BLD)/test_intr_dispatch.o \
		$(TEST_BLD)/intr_dispatch_production.o
	$(HOST_CC) -o $@ $^

# ── Interrupt dispatch phase 1, Task 3: aarch64 routing backend ──
# Links the PRODUCTION ARM decoder (aarch64_intr_decode_production.o,
# built by the Task 1 rule above), the PRODUCTION aarch64 backend
# (kernel/arch/aarch64/intr/dispatch.c) and a copy of the PRODUCTION
# common dispatcher compiled against the aarch64 pt_regs_t (the Task 2
# intr_dispatch_production.o object was compiled against the x86_64
# facade; this binary needs every TU to agree on the aarch64 layout, so
# it gets its own object built with the same mock shim Task 1 used).
#
# The terminal ends are spied in the test TU: aarch64_el1_sync_fatal and
# arch_intr_bad_event escape via longjmp (the real ones park the CPU),
# gic_dev_dispatch counts calls and gic_dev_current returns a fake device
# (gic.c has heavy DTB/boot-log deps and is deliberately not linked).
#
# Like the Task 2 dispatcher harness, <setjmp.h> must come from the host:
# libc/include is added with -idirafter so it never wins over the system
# header while the kernel libc remains reachable for the other TUs.
A64_BACKEND_HOST_CFLAGS := $(HOST_CFLAGS) $(FRAMEWORK_INC) \
                          -I$(TEST_MOCK)/intr_dispatch/aarch64 \
                          $(KERNEL_INC) -idirafter $(TESTS_DIR)/libc/include

$(TEST_BLD)/aarch64_intr_backend_production.o: \
		$(TESTS_DIR)/kernel/arch/aarch64/intr/dispatch.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/intr_dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/intr_decode.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/gic.h \
		$(TESTS_DIR)/kernel/include/arch/intr.h \
		$(TESTS_DIR)/kernel/include/intr/dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/sync_fault.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/regs.h \
		$(TEST_MOCK)/intr_dispatch/aarch64/arch/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(A64_BACKEND_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/intr_dispatch_aarch64_production.o: \
		$(TESTS_DIR)/kernel/intr/dispatch.c \
		$(TESTS_DIR)/kernel/include/intr/dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/intr.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/regs.h \
		$(TEST_MOCK)/intr_dispatch/aarch64/arch/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(A64_BACKEND_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_intr_backend_aarch64.o: \
		$(TEST_CASES)/test_intr_backend_aarch64.c \
		$(TESTS_DIR)/kernel/include/arch/aarch64/intr_decode.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/intr_dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/gic.h \
		$(TESTS_DIR)/kernel/include/arch/intr.h \
		$(TESTS_DIR)/kernel/include/intr/dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/aarch64/regs.h \
		$(TEST_MOCK)/intr_dispatch/aarch64/arch/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(A64_BACKEND_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_intr_backend_aarch64.elf: \
		$(TEST_BLD)/test_intr_backend_aarch64.o \
		$(TEST_BLD)/aarch64_intr_backend_production.o \
		$(TEST_BLD)/aarch64_intr_decode_production.o \
		$(TEST_BLD)/intr_dispatch_aarch64_production.o
	$(HOST_CC) -o $@ $^

# ── Interrupt dispatch phase 1, Task 4: x86_64 routing backend ──
# Links the PRODUCTION x86 decoder (x86_intr_decode_production.o, built
# by the Task 1 rule above), the PRODUCTION x86 backend
# (kernel/arch/x86_64/intr/dispatch.c) and the PRODUCTION common
# dispatcher object built by the Task 2 rule (intr_dispatch_production.o
# — already compiled against the x86_64 arch/regs.h facade, which the
# x86_64 host selects natively, so every TU in this binary agrees on
# the frame layout and no mock shim is needed).
#
# The terminal ends are spied in the test TU: the four private business
# handlers (x86_handle_*) record their arguments and return normally so
# the routing contract can be asserted, and arch_intr_bad_event escapes
# via longjmp (the real one parks the CPU forever).
#
# Like the Task 2/3 harnesses, <setjmp.h> must come from the host:
# libc/include is added with -idirafter so it never wins over the
# system header while the kernel libc remains reachable.
X86_BACKEND_HOST_CFLAGS := $(HOST_CFLAGS) $(FRAMEWORK_INC) $(KERNEL_INC) \
                           -idirafter $(TESTS_DIR)/libc/include

$(TEST_BLD)/x86_intr_backend_production.o: \
		$(TESTS_DIR)/kernel/arch/x86_64/intr/dispatch.c \
		$(TESTS_DIR)/kernel/include/arch/x86_64/intr_dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/intr_decode.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/intr_vectors.h \
		$(TESTS_DIR)/kernel/include/arch/intr.h \
		$(TESTS_DIR)/kernel/include/intr/dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/regs.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(X86_BACKEND_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_intr_backend_x86.o: \
		$(TEST_CASES)/test_intr_backend_x86.c \
		$(TESTS_DIR)/kernel/include/arch/x86_64/intr_dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/intr_decode.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/intr_vectors.h \
		$(TESTS_DIR)/kernel/include/arch/intr.h \
		$(TESTS_DIR)/kernel/include/intr/dispatch.h \
		$(TESTS_DIR)/kernel/include/arch/regs.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(X86_BACKEND_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_intr_backend_x86.elf: \
		$(TEST_BLD)/test_intr_backend_x86.o \
		$(TEST_BLD)/x86_intr_backend_production.o \
		$(TEST_BLD)/x86_intr_decode_production.o \
		$(TEST_BLD)/intr_dispatch_production.o
	$(HOST_CC) -o $@ $^

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

# Terminal core test — compiles REAL user/terminal/terminal_core.c (pure logic)
$(TEST_BLD)/terminal_core.o: $(TESTS_DIR)/user/terminal/terminal_core.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/user/terminal -c $< -o $@

$(TEST_BLD)/test_terminal_core.o: $(TEST_CASES)/test_terminal_core.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/user/terminal -c $< -o $@

$(TEST_BLD)/test_terminal_core.elf: $(TEST_BLD)/test_terminal_core.o $(TEST_BLD)/terminal_core.o
	$(HOST_CC) -Wl,--wrap=malloc -o $@ $^

# Terminal render test — compiles REAL user/terminal/terminal_render.c + terminal_core.c + libgfx
$(TEST_BLD)/terminal_render.o: $(TESTS_DIR)/user/terminal/terminal_render.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/user/terminal -I$(TESTS_DIR)/libgfx -I$(TESTS_DIR)/kernel/include -c $< -o $@

$(TEST_BLD)/test_terminal_render.o: $(TEST_CASES)/test_terminal_render.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/user/terminal -I$(TESTS_DIR)/libgfx -I$(TESTS_DIR)/kernel/include -DREPO_ROOT=\"$(TESTS_DIR)\" -c $< -o $@

$(TEST_BLD)/test_terminal_render.elf: $(TEST_BLD)/test_terminal_render.o \
		$(TEST_BLD)/terminal_render.o \
		$(TEST_BLD)/terminal_core.o \
		$(TEST_BLD)/libgfx_gfx_production.o \
		$(TEST_BLD)/libgfx_line_production.o \
		$(TEST_BLD)/libgfx_sprite_production.o
	$(HOST_CC) -o $@ $^

test-terminal-render: $(TEST_BLD)/test_terminal_render.elf
	$<

# Terminal display transaction test — REAL user/terminal/terminal_display.c with an
# injected libgfx/FBIOGET_STATE backend; malloc is wrapped for ENOMEM injection.
# -DFB_RESOLUTION_TEST compiles the Task-9 fbtest ENOMEM hook so
# test_terminal_fbtest_enomem_once can drive it; the tests always install a
# stub op, so the real /dev/fbtest path is never taken on the host.
# -DUSER_DEBUG=1 compiles the [term-present] emit path in (the production
# default is USER_DEBUG=0, where the emit path does not exist at all).
$(TEST_BLD)/terminal_display.o: $(TESTS_DIR)/user/terminal/terminal_display.c \
		$(TESTS_DIR)/user/terminal/terminal_display.h \
		$(TESTS_DIR)/user/common/user_log.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_CLIENT_HOST_CFLAGS) -DFB_RESOLUTION_TEST -DUSER_DEBUG=1 \
		-I$(TEST_INC) -I$(TESTS_DIR)/user/terminal -I$(TESTS_DIR)/user/common -c $< -o $@

$(TEST_BLD)/test_terminal_display.o: $(TEST_CASES)/test_terminal_display.c \
		$(TESTS_DIR)/user/terminal/terminal_display.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_CLIENT_HOST_CFLAGS) -I$(TEST_INC) -I$(TESTS_DIR)/user/terminal -c $< -o $@

$(TEST_BLD)/test_terminal_display.elf: $(TEST_BLD)/test_terminal_display.o \
		$(TEST_BLD)/terminal_display.o \
		$(TEST_BLD)/user_log.o \
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

# ── User-space debug log facade (user/common/user_log.c) ────────────
# The log_debug() facade the terminal's [term-present] / [term-probe]
# records are emitted through.  Two builds of the same case:
#   * USER_DEBUG=1  — the sink routing / exact-text / error-propagation
#                     contract of the enabled path.
#   * USER_DEBUG=0  — the production default: the macro must be compiled
#                     out, so neither the format string nor its arguments
#                     are ever evaluated.
# NOTE: this TU deliberately does NOT get $(TEST_INC):
# hosttests/include/errno.h declares a non-TLS `extern int errno;` while
# host glibc provides a TLS errno, and linking the module against libc.so
# with a non-TLS reference fails with the "TLS definition ... mismatches
# non-TLS reference" link error.  Without $(TEST_INC) the module picks up
# glibc's <errno.h> and the two agree on the TLS accessor.
$(TEST_BLD)/user_log.o: $(TESTS_DIR)/user/common/user_log.c \
		$(TESTS_DIR)/user/common/user_log.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -DUSER_DEBUG=1 -I$(TESTS_DIR)/user/common -c $< -o $@

$(TEST_BLD)/test_user_log.o: $(TEST_CASES)/test_user_log.c \
		$(TESTS_DIR)/user/common/user_log.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) -DUSER_DEBUG=1 -I$(TESTS_DIR)/user/common -c $< -o $@

$(TEST_BLD)/test_user_log.elf: $(TEST_BLD)/test_user_log.o $(TEST_BLD)/user_log.o
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_user_log_off.o: $(TEST_CASES)/test_user_log.c \
		$(TESTS_DIR)/user/common/user_log.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) -I$(TESTS_DIR)/user/common -c $< -o $@

$(TEST_BLD)/test_user_log_off.elf: $(TEST_BLD)/test_user_log_off.o
	$(HOST_CC) -o $@ $^

test-user-log: $(TEST_BLD)/test_user_log.elf $(TEST_BLD)/test_user_log_off.elf
	$(TEST_BLD)/test_user_log.elf
	$(TEST_BLD)/test_user_log_off.elf

# Tetris logic test — compiles REAL user/tetris/tetris_logic.c (pure C)
$(TEST_BLD)/tetris_logic.o: $(TESTS_DIR)/user/tetris/tetris_logic.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/user/tetris -c $< -o $@

$(TEST_BLD)/test_tetris_logic.o: $(TEST_CASES)/test_tetris_logic.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/user/tetris -c $< -o $@

$(TEST_BLD)/test_tetris_logic.elf: $(TEST_BLD)/test_tetris_logic.o $(TEST_BLD)/tetris_logic.o
	$(HOST_CC) -o $@ $^

# GUI-3 Task 3: the desktop surface state machine.  Compiles the REAL
# user/desktop/desktop_state.c (pure C, no LVGL dependency) so the
# (state x intent) matrix can be exhausted on the host.
$(TEST_BLD)/desktop_state.o: $(TESTS_DIR)/user/desktop/desktop_state.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/user/desktop -c $< -o $@

$(TEST_BLD)/test_desktop_state.o: $(TEST_CASES)/test_desktop_state.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TESTS_DIR)/user/desktop -c $< -o $@

$(TEST_BLD)/test_desktop_state.elf: $(TEST_BLD)/test_desktop_state.o $(TEST_BLD)/desktop_state.o
	$(HOST_CC) -o $@ $^

POLL_PRODUCTION_OBJS := \
    $(TEST_BLD)/poll_production.o \
    $(TEST_BLD)/select_production.o \
    $(TEST_BLD)/file_poll_production.o \
    $(TEST_BLD)/tty_poll_production.o \
    $(TEST_BLD)/devfs_poll_production.o \
    $(TEST_BLD)/poll_clocksource_stub.o \
    $(TEST_BLD)/serial_sink_stub.o

# The host-side console sink for the production serial write path
# (kernel/fs/devfs.c serial_write).  The production object references
# serial_lock and write_serial_unlocked: the host build has no UART, so
# the sink discards the bytes (the host spinlock is a no-op).
$(TEST_BLD)/serial_sink_stub.o: $(TEST_MOCK)/serial_sink_stub.c \
		$(TEST_MOCK)/test_platform.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -D__SPINLOCK_H__ -DOS01_HOST_TEST \
		-include $(TEST_MOCK)/test_platform.h \
		$(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC) -c $< -o $@

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
		$(TEST_BLD)/keyboard_production.o \
		$(TEST_BLD)/input_route_kbd_production.o \
		$(TEST_BLD)/input_ps2_production.o
	$(HOST_CC) -o $@ $^

# The keyboard driver hands every hardware byte to the input route, so
# the route and its framer are linked into the fixtures that compile
# the real keyboard / mouse drivers.  The route is compiled with the
# same kbd shadows as the drivers.
$(TEST_BLD)/input_route_kbd_production.o: $(TESTS_DIR)/kernel/driver/input_route.c \
		$(TESTS_DIR)/kernel/include/driver/input_route.h \
		$(TESTS_DIR)/kernel/include/driver/input_ps2.h \
		$(TEST_MOCK)/kbd_include/fs/file.h \
		$(TEST_MOCK)/kbd_include/fs/poll.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(KBD_HOST_CFLAGS) -c $< -o $@

test_i8042_demux: $(TEST_BLD)/test_i8042_demux.elf
	$<

$(TEST_BLD)/test_mouse_proto.elf: $(TEST_CASES)/test_mouse_proto.c $(TESTS_DIR)/kernel/driver/mouse_proto.c $(TESTS_DIR)/kernel/include/driver/mouse_proto.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -I$(TEST_INC) $< $(TESTS_DIR)/kernel/driver/mouse_proto.c -o $@

test_mouse_proto: $(TEST_BLD)/test_mouse_proto.elf
	$<

# Links the real phase-6 mouse driver against scriptable i8042 and IRQ
# transactions; the test observes command-byte and consumer state.
# The mouse driver hands every event to the input route, so the route
# and its framer are linked in as well.
$(TEST_BLD)/test_mouse_init.elf: $(TEST_CASES)/test_mouse_init.c \
		$(TESTS_DIR)/kernel/driver/mouse.c \
		$(TESTS_DIR)/kernel/driver/input_route.c \
		$(TESTS_DIR)/kernel/driver/input_ps2.c \
		$(TESTS_DIR)/kernel/include/driver/mouse.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(KBD_HOST_CFLAGS) $< \
		$(TESTS_DIR)/kernel/driver/mouse.c \
		$(TESTS_DIR)/kernel/driver/input_route.c \
		$(TESTS_DIR)/kernel/driver/input_ps2.c -o $@

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
		$(TEST_BLD)/gfx_gfx_production.o \
		$(TEST_BLD)/gfx_devfs_production.o \
		$(TEST_BLD)/gfx_file_production.o \
		$(TEST_BLD)/serial_sink_stub.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

test-gfx-file-lifecycle: $(TEST_BLD)/test_gfx_file_lifecycle.elf
	$<

# Monitor plan Task 1: VFS per-file snapshot lifecycle hooks.  Reuses
# the gfx harness's production objects (kernel/fs/file.c + devfs.c via
# GFX_HOST_CFLAGS / gfx_test_runtime.h) so the fixture drives the REAL
# file_alloc / file_get / file_put / file_free / fd_read paths and the
# real devfs register/open/release protocol, not mocks of them.  The
# snapshot filesystem callbacks live in the test TU (the procfs
# counterpart is monitor plan Task 4).  Tested production symbols:
# vfs_file_open, vfs_file_read, vfs_file_release, fd_read,
# devfs_register_chrdev, devfs_open_node, devfs_release_file.
.PHONY: test-monitor-file
test-monitor-file: $(TEST_BLD)/test_monitor_file.elf
	$<

$(TEST_BLD)/test_monitor_file.o: $(TEST_CASES)/test_monitor_file.c \
		$(TEST_MOCK)/gfx_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_monitor_file.elf: $(TEST_BLD)/test_monitor_file.o \
		$(TEST_BLD)/gfx_devfs_production.o \
		$(TEST_BLD)/gfx_file_production.o \
		$(TEST_BLD)/serial_sink_stub.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

# Monitor plan Task 2: per-CPU tick accounting.  Host-compiles the REAL
# kernel/sched/accounting.c against mock/accounting_test_runtime.h,
# which short-circuits sched/task.h + percpu/percpu.h and provides a
# test-controlled cpu_id() / percpu_data[] / current, so the fixture
# drives the production accounting code (not a mock of it).  The test TU
# reuses test_platform.h's percpu_t / spinlock stubs (FRAMEWORK_INC
# preincludes it FIRST, so the mock header takes its test-TU branch);
# the production object must NOT see FRAMEWORK_INC — test_platform.h
# typedefs a stale percpu_t and a constant cpu_id().
ACC_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC) $(LIBC_INC)
ACC_PROD_CFLAGS := $(ACC_HOST_CFLAGS) \
                   -include $(TEST_MOCK)/accounting_test_runtime.h

$(TEST_BLD)/accounting_production.o: $(TESTS_DIR)/kernel/sched/accounting.c \
		$(TEST_MOCK)/accounting_test_runtime.h \
		$(TESTS_DIR)/kernel/include/sched/accounting.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(ACC_PROD_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_monitor_accounting.o: $(TEST_CASES)/test_monitor_accounting.c \
		$(TEST_MOCK)/accounting_test_runtime.h \
		$(TESTS_DIR)/kernel/include/sched/accounting.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FRAMEWORK_INC) $(ACC_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_monitor_accounting.elf: $(TEST_BLD)/test_monitor_accounting.o \
		$(TEST_BLD)/accounting_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test-monitor-accounting
test-monitor-accounting: $(TEST_BLD)/test_monitor_accounting.elf
	$<

# Monitor plan Task 3: locked PMM usage snapshot.  Links the REAL
# kernel/memory/pmm.c via the SAME PMM_SHADOW_INC harness as
# test_pmm_ram_rel_index (mock arch/spinlock.h + arch/cpu.h shadow the
# privileged-asm headers; everything else is the production header).
# The fixture hand-builds PMMngr (zone counters + bits_map) instead of
# driving pmm_init's boot_context path — out of scope for a snapshot
# test — and parks a static backing buffer at the Phy_To_Virt image of
# the pool page so alloc_4k_page's new-pool path writes into real host
# memory.  Asserted: the 3-pages/1-in-use fixture T=6291456 /
# U=2097152 / F=4194304, T==U+F across page alloc/free, in-pool 4KB
# alloc/free leaving U untouched, T=0 → zero stats, Σzone summing.
$(TEST_BLD)/test_monitor_pmm.o: $(TEST_CASES)/test_monitor_pmm.c \
		$(TEST_MOCK)/pmm_include/arch/spinlock.h \
		$(TESTS_DIR)/kernel/include/memory/pmm.h \
		$(TESTS_DIR)/kernel/include/memory/memory.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -I$(TEST_INC) -c $< -o $@

$(TEST_BLD)/test_monitor_pmm.elf: $(TEST_BLD)/test_monitor_pmm.o \
		$(TEST_BLD)/pmm_production.o \
		$(TEST_BLD)/pmm_arch_default.o \
		$(TEST_BLD)/pmm_boot_production.o
	$(HOST_CC) -o $@ $^

.PHONY: test-monitor-pmm
test-monitor-pmm: $(TEST_BLD)/test_monitor_pmm.elf
	$<

# Monitor plan Task 4: task identity + procfs snapshot producers.
# Host-compiles the REAL kernel/fs/procfs_snap.c (the lock-taking
# snapshot producers + /proc root task readdir), kernel/fs/procfs_fmt.c
# (pure formatting + Name encoding), kernel/sched/identity.c (birth_id/
# start_ns/comm lifecycle) and kernel/arch/thermal.c (weak thermal
# defaults + per-CPU cache) against mock/procfs_test_runtime.h, which
# shadows sched/task.h / percpu / slab and provides a test-controlled
# task list, monotonic clock, kmalloc fail-injection and a
# task_list_lock observation hook that can simulate the winning
# concurrent delete (unlink + poison + free inside the acquisition).
# The fixture stubs sched_accounting_read / pmm_get_stats — the real
# units keep their own Task-2/Task-3 harnesses (test-monitor-accounting,
# test-monitor-pmm).  Asserted: fork birth/runtime/comm protocol, exec
# rename rules, Name encoding (63 control bytes → 252, one line,
# backslash doubling, \xHH, exact-fit EOVERFLOW), status/stat/thermal/
# meminfo formats (Version: 1 + SampleNs everywhere, meminfo kB),
# snapshot stability across task disappearance, -ENOMEM/-EOVERFLOW
# rollback, readdir zombie visibility + concurrent-delete safety.
PROCFS_HOST_CFLAGS := $(HOST_CFLAGS) -ffunction-sections -fdata-sections \
    -Wno-unused-function -D__SPINLOCK_H__ -DOS01_HOST_TEST \
    -include $(TEST_MOCK)/procfs_test_runtime.h \
    -I$(TEST_MOCK)/include $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)

# The pure formatter TU needs no mock surface at all.
FMT_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC) $(LIBC_INC)

$(TEST_BLD)/procfs_snap_production.o: $(TESTS_DIR)/kernel/fs/procfs_snap.c \
		$(TEST_MOCK)/procfs_test_runtime.h \
		$(TESTS_DIR)/kernel/include/fs/procfs_fmt.h \
		$(TESTS_DIR)/kernel/include/fs/procfs.h \
		$(TESTS_DIR)/kernel/include/arch/thermal.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PROCFS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/procfs_fmt_production.o: $(TESTS_DIR)/kernel/fs/procfs_fmt.c \
		$(TESTS_DIR)/kernel/include/fs/procfs_fmt.h \
		$(TESTS_DIR)/kernel/include/arch/thermal.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FMT_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/identity_production.o: $(TESTS_DIR)/kernel/sched/identity.c \
		$(TEST_MOCK)/procfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PROCFS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/thermal_production.o: $(TESTS_DIR)/kernel/arch/thermal.c \
		$(TESTS_DIR)/kernel/include/arch/thermal.h \
		$(TEST_MOCK)/procfs_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PROCFS_HOST_CFLAGS) -c $< -o $@

# ── monitor thermal plan, Task 1: instruction-scoped safe RDMSR ──
# Host fixture test_thermal_fixup.elf links the REAL production
# kernel/arch/x86_64/cpu/msr_safe.{S,c} — there is no host-side
# clone.  The preprocessor needs KERNEL_INC so .S can pull
# <arch/x86_64/linkage.h>; the C side needs the same plus the host
# mock surface so spinlock/clocksource/... stubs are visible.
MSR_HOST_CFLAGS := $(HOST_CFLAGS) -ffunction-sections -fdata-sections \
                   -Wno-unused-function -D__SPINLOCK_H__ -DOS01_HOST_TEST \
                   -I$(TEST_MOCK)/include $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)

$(TEST_BLD)/msr_safe_production.o: $(TESTS_DIR)/kernel/arch/x86_64/cpu/msr_safe.S \
		$(TESTS_DIR)/kernel/include/arch/x86_64/linkage.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/msr_safe.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(MSR_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/msr_safe_fixup_production.o: $(TESTS_DIR)/kernel/arch/x86_64/cpu/msr_safe_fixup.c \
		$(TESTS_DIR)/kernel/include/arch/x86_64/msr_safe.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(MSR_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_thermal_fixup.o: $(TEST_CASES)/test_thermal_fixup.c \
		$(TESTS_DIR)/kernel/include/arch/x86_64/msr_safe.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/regs.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(MSR_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_thermal_fixup.elf: $(TEST_BLD)/test_thermal_fixup.o \
		$(TEST_BLD)/msr_safe_production.o \
		$(TEST_BLD)/msr_safe_fixup_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

.PHONY: test-thermal-fixup
test-thermal-fixup: $(TEST_BLD)/test_thermal_fixup.elf
	$<

# ── monitor thermal plan, Task 2: BSP-pinned kernel threads ──
# Host fixture test_thermal_affinity.elf compiles against
# <sched/task.h> + <sched/internal.h> + <arch/x86_64/msr_safe.h>
# (regression on Task 1).  The runtime scheduler checks live in
# kernel/selftest/test_thermal_affinity.c (in-kernel).
AFFINITY_HOST_CFLAGS := $(HOST_CFLAGS) -ffunction-sections -fdata-sections \
                        -Wno-unused-function -D__SPINLOCK_H__ -DOS01_HOST_TEST \
                        -I$(TEST_MOCK)/include $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)

$(TEST_BLD)/test_thermal_affinity.o: $(TEST_CASES)/test_thermal_affinity.c \
		$(TESTS_DIR)/kernel/include/sched/task.h \
		$(TESTS_DIR)/kernel/include/sched/internal.h \
		$(TESTS_DIR)/kernel/include/arch/x86_64/msr_safe.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(AFFINITY_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_thermal_affinity.elf: $(TEST_BLD)/test_thermal_affinity.o \
		$(TEST_BLD)/msr_safe_production.o \
		$(TEST_BLD)/msr_safe_fixup_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

.PHONY: test-thermal-affinity
test-thermal-affinity: $(TEST_BLD)/test_thermal_affinity.elf
	$<

# ── monitor thermal plan, Task 3: pure IA32_THERM_STATUS decoder ──
# Pure decode lives in kernel/arch/x86_64/cpu/thermal_decode.c so
# both the kernel build and the host fixture can link it without
# pulling in the rest of the backend (which needs kernel symbols).
THERMAL_DECODE_HOST_CFLAGS := $(HOST_CFLAGS) -ffunction-sections -fdata-sections \
                              -Wno-unused-function -D__SPINLOCK_H__ -DOS01_HOST_TEST \
                              -I$(TEST_MOCK)/include $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)

$(TEST_BLD)/thermal_decode_production.o: \
		$(TESTS_DIR)/kernel/arch/x86_64/cpu/thermal_decode.c \
		$(TESTS_DIR)/kernel/include/arch/thermal.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(THERMAL_DECODE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_thermal_decode.o: $(TEST_CASES)/test_thermal_decode.c \
		$(TESTS_DIR)/kernel/include/arch/thermal.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(THERMAL_DECODE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_thermal_decode.elf: $(TEST_BLD)/test_thermal_decode.o \
		$(TEST_BLD)/thermal_decode_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

.PHONY: test-thermal-decode
test-thermal-decode: $(TEST_BLD)/test_thermal_decode.elf
	$<

$(TEST_BLD)/test_monitor_procfs.o: $(TEST_CASES)/test_monitor_procfs.c \
		$(TEST_MOCK)/procfs_test_runtime.h \
		$(TESTS_DIR)/kernel/include/fs/procfs_fmt.h \
		$(TESTS_DIR)/kernel/include/fs/procfs.h \
		$(TESTS_DIR)/kernel/include/sched/accounting.h \
		$(TESTS_DIR)/kernel/include/memory/pmm.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PROCFS_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_monitor_procfs.elf: $(TEST_BLD)/test_monitor_procfs.o \
		$(TEST_BLD)/procfs_snap_production.o \
		$(TEST_BLD)/procfs_fmt_production.o \
		$(TEST_BLD)/identity_production.o \
		$(TEST_BLD)/thermal_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

.PHONY: test-monitor-procfs
test-monitor-procfs: $(TEST_BLD)/test_monitor_procfs.elf
	$<

# Monitor plan Task 6: user-space collection + strict parsing.  The
# fixture drives the REAL user/monitor/collect.c (freestanding userland
# code, OS01 libc headers) against a fixture-owned fake procfs.  The
# production TU's libc calls are renamed onto mon_test_* shims with -D
# (open/read/close/opendir/readdir/closedir/malloc/free), giving the
# fixture an interposition seam WITHOUT relying on host-libc symbol
# overriding: the fixture serves per-path text or -ENOENT, chunks
# reads, counts open fds (NOFILE discipline) and injects allocation
# failures.  Asserted: §6 validity rules (required/duplicate/overflow/
# illegal -> -EINVAL, unknown keys ignored, order/whitespace
# independent, Version must be 1), meminfo kB->bytes with T=0 invalid,
# Name decode (\\ and \xHH only, no decoded NUL, <=63 bytes, name is a
# copy), thermal fixed reason table + unknown marker + State-decided
# usability, PID all-decimal dedup, ENOENT skip, per-PID
# close-before-next-open with max_open_fds <= baseline+2, overlong
# files invalid, per-metric isolation, OOM round rollback (-ENOMEM).
MONITOR_COLLECT_CFLAGS := $(HOST_CFLAGS) $(LIBC_INC) -I$(TESTS_DIR)/user \
    -Dopen=mon_test_open -Dclose=mon_test_close -Dread=mon_test_read \
    -Dopendir=mon_test_opendir -Dreaddir=mon_test_readdir \
    -Dclosedir=mon_test_closedir \
    -Dmalloc=mon_test_malloc -Dfree=mon_test_free

$(TEST_BLD)/monitor_collect_production.o: $(TESTS_DIR)/user/monitor/collect.c \
		$(TESTS_DIR)/user/monitor/collect.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(MONITOR_COLLECT_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_monitor_collect.o: $(TEST_CASES)/test_monitor_collect.c \
		$(TESTS_DIR)/user/monitor/collect.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) \
	  -I$(TESTS_DIR)/user -c $< -o $@

$(TEST_BLD)/test_monitor_collect.elf: $(TEST_BLD)/test_monitor_collect.o \
		$(TEST_BLD)/monitor_collect_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

.PHONY: test-monitor-collect
test-monitor-collect: $(TEST_BLD)/test_monitor_collect.elf
	$<

# Monitor plan Task 7: delta model + bounded history.  The fixture
# drives the REAL user/monitor/model.c (freestanding userland code, OS01
# libc headers) with hand-built monitor_sample rounds.  The production
# TU's malloc/free are renamed onto mon_model_* shims with -D (the same
# interposition seam as monitor_collect_production.o), so the fixture
# can inject an allocation failure and prove an OOM round returns
# -ENOMEM WITHOUT destroying the previous display rows or the delta
# baseline.  Asserted (spec §2.3/§5.3): dual-core busy deltas 100/0 +
# idle 0/100 -> total 5000 / process 10000, all-busy -> 10000, one-decal
# rounding (1/3 -> 3333, 2/3 -> 6667), first round / zero denominator /
# counter regression / epoch change / differing tick_hz -> invalid with
# the new counters kept as the rebuilt baseline, new-onlined cores
# never contribute their accumulated history to the period delta,
# migration keeps runtime, exec same-birth continues the delta,
# PID-same-value new birth invalid, vanished PIDs removed (history
# with them), 60-slot ring wraparound with empty points (valid bit
# clear) for missing rounds and no stale values copied as current,
# near-UINT64_MAX counters delta without overflow (compare-then-
# subtract + 128-bit scaled intermediates, remainder via n-(n/d)*d),
# process display clamped at 10000 with the diagnostic 128-bit raw
# kept, sorting (processes CPU% desc / PID asc / invalid last; CPUs
# CPU% desc) and the init/update/destroy ownership contract.
MONITOR_MODEL_CFLAGS := $(HOST_CFLAGS) $(LIBC_INC) -I$(TESTS_DIR)/user \
    -Dmalloc=mon_model_malloc -Dfree=mon_model_free

$(TEST_BLD)/monitor_model_production.o: $(TESTS_DIR)/user/monitor/model.c \
		$(TESTS_DIR)/user/monitor/model.h \
		$(TESTS_DIR)/user/monitor/collect.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(MONITOR_MODEL_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_monitor_model.o: $(TEST_CASES)/test_monitor_model.c \
		$(TESTS_DIR)/user/monitor/model.h \
		$(TESTS_DIR)/user/monitor/collect.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) \
	  -I$(TESTS_DIR)/user -c $< -o $@

$(TEST_BLD)/test_monitor_model.elf: $(TEST_BLD)/test_monitor_model.o \
		$(TEST_BLD)/monitor_model_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

.PHONY: test-monitor-model
test-monitor-model: $(TEST_BLD)/test_monitor_model.elf
	$<

# Monitor plan Task 8: terminal dashboard, sampling loop and terminal
# lifecycle.  Drives the REAL user/monitor/render.c (all output through
# a renamed fprintf -> mon_test_fprintf) and user/monitor/monitor.c
# (renamed isatty/tcgetattr/tcsetattr/ioctl/poll/clock_gettime/signal/
# read + monitor_collect/monitor_sample_destroy, errno -> mon_test_errno,
# and main -> monitor_main so the fixture keeps its own entry point).
# model.c is linked link the real delta engine, so the loop's first
# round renders "--" and its second renders a true "50.0%".  Asserted
# (plan Task 8 Step 1): 5000 -> "50.0%" / invalid -> "--" / one-decimal
# rounding, the "PMM 占用" MiB label, Source-driven temperature labels,
# stale expiry markers, Name ESC re-escaping (no raw control byte ever
# reaches the terminal), narrow-terminal truncation that never splits an
# escape sequence, the single exit path (termios_restored &&
# cursor_restored) for q/SIGINT/SIGTERM/EOF/init-failure, no uninitialized
# termios write-back, a 3-second overrun producing exactly one catch-up
# sample, and finite plain --samples output with unknown-argument
# rejection.
MONITOR_RENDER_CFLAGS := $(HOST_CFLAGS) $(LIBC_INC) -I$(TESTS_DIR)/user
MONITOR_MAIN_CFLAGS := $(MONITOR_RENDER_CFLAGS) \
    -Dmain=monitor_main \
    -Disatty=mon_test_isatty \
    -Dtcgetattr=mon_test_tcgetattr \
    -Dtcsetattr=mon_test_tcsetattr \
    -Dioctl=mon_test_ioctl \
    -Dpoll=mon_test_poll \
    -Dclock_gettime=mon_test_clock_gettime \
    -Dsignal=mon_test_signal \
    -Dread=mon_test_read \
    -Dmonitor_collect=mon_test_collect \
    -Dmonitor_sample_destroy=mon_test_sample_destroy \
    -Derrno=mon_test_errno
MONITOR_RENDER_PROD_CFLAGS := $(MONITOR_RENDER_CFLAGS) -Dfprintf=mon_test_fprintf -Dfflush=mon_test_fflush

$(TEST_BLD)/monitor_render_production.o: $(TESTS_DIR)/user/monitor/render.c \
		$(TESTS_DIR)/user/monitor/render.h \
		$(TESTS_DIR)/user/monitor/model.h \
		$(TESTS_DIR)/user/monitor/collect.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(MONITOR_RENDER_PROD_CFLAGS) -c $< -o $@

$(TEST_BLD)/monitor_main_production.o: $(TESTS_DIR)/user/monitor/monitor.c \
		$(TESTS_DIR)/user/monitor/render.h \
		$(TESTS_DIR)/user/monitor/model.h \
		$(TESTS_DIR)/user/monitor/collect.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(MONITOR_MAIN_CFLAGS) -c $< -o $@

$(TEST_BLD)/monitor_render_model.o: $(TESTS_DIR)/user/monitor/model.c \
		$(TESTS_DIR)/user/monitor/model.h \
		$(TESTS_DIR)/user/monitor/collect.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(MONITOR_RENDER_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_monitor_render.o: $(TEST_CASES)/test_monitor_render.c \
		$(TESTS_DIR)/user/monitor/render.h \
		$(TESTS_DIR)/user/monitor/model.h \
		$(TESTS_DIR)/user/monitor/collect.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) \
	  -I$(TESTS_DIR)/user -c $< -o $@

$(TEST_BLD)/test_monitor_render.elf: $(TEST_BLD)/test_monitor_render.o \
		$(TEST_BLD)/monitor_render_production.o \
		$(TEST_BLD)/monitor_main_production.o \
		$(TEST_BLD)/monitor_render_model.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

.PHONY: test-monitor-render
test-monitor-render: $(TEST_BLD)/test_monitor_render.elf
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

# Partial-present Task 1 Step 2: host SIGSEGV bridge for the
# in-process inline-memcpy fault tests.  Compiled with HOST_CC /
# HOST_CFLAGS only — NO KERNEL_INC, NO LIBC_INC, NO force-include,
# NO GFX_HOST_CFLAGS, NO production-header chain.  The bridge .c is
# the ONLY TU in the binary that talks to glibc's signal ABI; the
# test TU includes <signal.h> only for the sigprocmask probe that
# confirms restore() puts the host SIGSEGV mask back to the
# pre-install value.  Linked ONLY into test_gfx_device.elf (the
# gfx_device tests are the only ones that exercise fault_jmp).
$(TEST_BLD)/gfx_fault_bridge.o: $(TEST_MOCK)/gfx_fault_bridge.c \
		$(TEST_MOCK)/gfx_fault_bridge.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

# Bridge self-test: links the bridge and the test TU only — does NOT
# pull in any production code.  Lets us verify install/restore +
# longjmp-on-fault before the new rect-present tests depend on it.
# Mirrors the GFX_CLIENT_*_CFLAGS pattern: errno must expand to glibc's
# TLS accessor (hosttests/include/errno.h declares a non-TLS reference,
# which trips the .tbss/.data mismatch at link time).
$(TEST_BLD)/test_gfx_fault_bridge.o: $(TEST_CASES)/test_gfx_fault_bridge.c \
		$(TEST_MOCK)/gfx_fault_bridge.h \
		$(TEST_INC)/test_framework.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_MOCK) -I$(TEST_INC) \
		-D'errno=(*__errno_location())' \
		-include $(TEST_INC)/gfx_client_errno.h -c $< -o $@

$(TEST_BLD)/test_gfx_fault_bridge.elf: $(TEST_BLD)/test_gfx_fault_bridge.o \
		$(TEST_BLD)/gfx_fault_bridge.o
	$(HOST_CC) -o $@ $^

test-gfx-fault-bridge: $(TEST_BLD)/test_gfx_fault_bridge.elf
	$<

$(TEST_BLD)/test_gfx_device.o: $(TEST_CASES)/test_gfx_device.c \
		$(TESTS_DIR)/kernel/include/driver/gfx.h \
		$(TESTS_DIR)/kernel/include/uapi/gfx.h \
		$(TEST_MOCK)/gfx_test_runtime.h \
		$(TEST_MOCK)/gfx_fault_bridge.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TEST_MOCK) -c $< -o $@

$(TEST_BLD)/test_gfx_device.elf: $(TEST_BLD)/test_gfx_device.o \
		$(TEST_BLD)/gfx_gfx_production.o \
		$(TEST_BLD)/gfx_devfs_production.o \
		$(TEST_BLD)/gfx_file_production.o \
		$(TEST_BLD)/gfx_fault_bridge.o \
		$(TEST_BLD)/serial_sink_stub.o
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
# Task 8).  Host-compiles the REAL user/setres/setres_parse.c, user/setres/setres.c
# (with -DSETRES_NO_MAIN so this TU owns main) and
# user/common/gfx_client_policy.c.  libc open/close/ioctl are wrapped so the
# test can assert the CLI's exact device ops (help opens NOTHING) and
# inject GET_MODES / GET_STATE / SET_MODE results.
#
# The errno ABI shim (GFX_CLIENT_*_CFLAGS) is reused verbatim from the
# libgfx client test: kernel/include is on the path first for
# <uapi/fb.h> and its freestanding <errno.h>, so `errno` must be
# redirected to glibc's TLS accessor.  user/ is added for the
# "setres_parse.h" / "gfx_client_policy.h" includes.
SETRES_HOST_CFLAGS := $(GFX_CLIENT_PROD_CFLAGS) -I$(TESTS_DIR)/user/setres -I$(TESTS_DIR)/user/common

$(TEST_BLD)/gfx_client_policy.o: $(TESTS_DIR)/user/common/gfx_client_policy.c \
		$(TESTS_DIR)/user/common/gfx_client_policy.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SETRES_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/setres_parse.o: $(TESTS_DIR)/user/setres/setres_parse.c \
		$(TESTS_DIR)/user/setres/setres_parse.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SETRES_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/setres_cli.o: $(TESTS_DIR)/user/setres/setres.c \
		$(TESTS_DIR)/user/setres/setres_parse.h \
		$(TESTS_DIR)/kernel/include/uapi/fb.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SETRES_HOST_CFLAGS) -DSETRES_NO_MAIN -c $< -o $@

$(TEST_BLD)/test_setres.o: $(TEST_CASES)/test_setres.c \
		$(TESTS_DIR)/user/common/gfx_client_policy.h \
		$(TESTS_DIR)/user/setres/setres_parse.h \
		$(TESTS_DIR)/kernel/include/uapi/fb.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_CLIENT_TEST_CFLAGS) -I$(TESTS_DIR)/user/setres -I$(TESTS_DIR)/user/common -D_GNU_SOURCE -c $< -o $@

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

# Display ownership coordinator & exclusive writer leases test (Display Ownership Task 1)
$(TEST_BLD)/test_display_owner.o: $(TEST_CASES)/test_display_owner.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_STATE_HOST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_display_owner.elf: $(TEST_BLD)/test_display_owner.o $(TEST_BLD)/fb_state.o $(TEST_BLD)/fb_map.o $(TEST_BLD)/fb_resolution_runtime.o
	$(HOST_CC) -lpthread -o $@ $^

test-display-owner: $(TEST_BLD)/test_display_owner.elf
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

$(TEST_BLD)/fb_test.o: $(TESTS_DIR)/kernel/driver/fb_test.c \
		$(TEST_MOCK)/fb_resolution_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FB_HOOKS_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_fb_resolution_hooks.o: $(TEST_CASES)/test_fb_resolution_hooks.c \
		$(TEST_MOCK)/fb_resolution_runtime.h
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

# ── GUI platform host tests ─────────────────────────────────────
GUI_HOST_DIR := $(TEST_BLD)/gui
GUI_LVGL_DIR := $(GUI_HOST_DIR)/lvgl
GUI_DETERMINISTIC_DIR := $(GUI_HOST_DIR)/deterministic
GUI_INTEGRATION_DIR := $(GUI_HOST_DIR)/integration

include $(TESTS_DIR)/thirdpart/lvgl.manifest

GUI_LVGL_SOURCES := $(shell find $(TESTS_DIR)/$(LVGL_SOURCE)/src -name '*.c')
GUI_LVGL_OBJS := $(patsubst $(TESTS_DIR)/$(LVGL_SOURCE)/src/%.c,$(GUI_LVGL_DIR)/obj/%.o,$(GUI_LVGL_SOURCES))
GUI_LVGL_ARCHIVE := $(GUI_LVGL_DIR)/liblvgl-host.a

# GUI-3: the Material theme unit (token table + LVGL theme).  Every host
# test that links the REAL libgui platform must also link these two
# translation units, because gui_platform_create() applies the theme and
# gui_platform_destroy() releases it.
GUI_THEME_OBJS := $(GUI_INTEGRATION_DIR)/theme.o $(GUI_INTEGRATION_DIR)/theme_apply.o

$(GUI_HOST_DIR)/lvgl-version.stamp: $(TESTS_DIR)/thirdpart/lvgl.manifest
	@test -e "$(TESTS_DIR)/$(LVGL_SOURCE)/.git" || { echo "ERROR: $(LVGL_SOURCE) not a git submodule (.git missing)"; exit 1; }
	@test -f "$(TESTS_DIR)/$(LVGL_SOURCE)/lvgl.h" || { echo "ERROR: $(LVGL_SOURCE) missing lvgl.h"; exit 1; }
	@head=$$(git -C "$(TESTS_DIR)/$(LVGL_SOURCE)" rev-parse --verify HEAD 2>/dev/null) || { echo "ERROR: failed to get git commit for $(LVGL_SOURCE)"; exit 1; }; \
	if [ "$$head" != "$(LVGL_REVISION)" ]; then \
	  echo "ERROR: $(LVGL_SOURCE) revision $$head does not match manifest $(LVGL_REVISION)"; exit 1; \
	fi
	@mkdir -p $(dir $@)
	@touch $@

$(GUI_HOST_DIR)/include/uapi/gfx.h: $(TESTS_DIR)/kernel/include/uapi/gfx.h
	@mkdir -p $(dir $@)
	ln -sf $< $@

$(GUI_HOST_DIR)/include/uapi/fb.h: $(TESTS_DIR)/kernel/include/uapi/fb.h
	@mkdir -p $(dir $@)
	ln -sf $< $@

$(GUI_HOST_DIR)/include/uapi/mouse.h: $(TESTS_DIR)/kernel/include/uapi/mouse.h
	@mkdir -p $(dir $@)
	ln -sf $< $@

GUI_HOST_UAPI_HEADERS := $(GUI_HOST_DIR)/include/uapi/gfx.h $(GUI_HOST_DIR)/include/uapi/fb.h \
                        $(GUI_HOST_DIR)/include/uapi/mouse.h

GUI_HOST_LVGL_CFLAGS := -O2 -g $(filter-out -O0,$(HOST_CFLAGS)) \
    -I$(TESTS_DIR)/thirdpart \
    -I$(TESTS_DIR)/thirdpart/lvgl \
    -I$(TESTS_DIR)/config \
    -DLV_CONF_INCLUDE_SIMPLE

$(GUI_LVGL_OBJS): | $(GUI_HOST_DIR)/lvgl-version.stamp

$(GUI_LVGL_DIR)/obj/%.o: $(TESTS_DIR)/$(LVGL_SOURCE)/src/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_LVGL_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_LVGL_ARCHIVE): $(GUI_LVGL_OBJS)
	@mkdir -p $(dir $@)
	ar rcs $@ $^

GUI_HOST_CFLAGS := $(HOST_CFLAGS) -DOS01_HOST_TEST \
    -I$(GUI_HOST_DIR)/include \
    -I$(TESTS_DIR)/libgui/include \
    -I$(TESTS_DIR)/libgui/internal \
    -I$(TESTS_DIR)/libgfx \
    -I$(TESTS_DIR)/user/common \
    -I$(TESTS_DIR)/thirdpart \
    -I$(TESTS_DIR)/thirdpart/lvgl \
    -I$(TESTS_DIR)/config \
    -iquote $(TEST_INC) \
    -iquote $(TEST_MOCK) \
    -DLV_CONF_INCLUDE_SIMPLE \
    -DOS01_BUILD_PROFILE=\"$(PROFILE)\"

$(GUI_DETERMINISTIC_DIR)/platform.o: $(TESTS_DIR)/libgui/src/platform.c $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_DETERMINISTIC_DIR)/tick.o: $(TESTS_DIR)/libgui/src/tick.c $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_DETERMINISTIC_DIR)/gfx_client_policy.o: $(TESTS_DIR)/user/common/gfx_client_policy.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

# The libgui input unit (GUI-2 Task 6).  Every GUI host binary links it,
# because the platform TU owns the input lifecycle (step / terminal /
# destroy) and therefore references the input hooks.  The unit itself is
# device-free until an input is created: a binary that never creates an
# input keeps the GUI-1 behaviour exactly.
GUI_INPUT_UNIT_DIR := $(GUI_HOST_DIR)/input_unit
GUI_INPUT_UNIT_OBJS := $(GUI_INPUT_UNIT_DIR)/input.o \
                       $(GUI_INPUT_UNIT_DIR)/input_lvgl.o \
                       $(GUI_INPUT_UNIT_DIR)/input_focus.o \
                       $(GUI_INPUT_UNIT_DIR)/input_ps2.o \
                       $(GUI_INPUT_UNIT_DIR)/cursor_icon.o

$(GUI_INPUT_UNIT_DIR)/%.o: $(TESTS_DIR)/libgui/src/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

# The libgui widget unit (GUI-2b Task 1).  Like the input unit, every GUI
# host binary that links the REAL platform.o must also link it: platform.c
# owns the native widget registry lifetime (created with the platform,
# released after the display is gone).
GUI_WIDGET_DIR := $(GUI_HOST_DIR)/widget
GUI_WIDGET_UNIT_OBJS := $(GUI_WIDGET_DIR)/widget.o

$(GUI_WIDGET_DIR)/widget.o: $(TESTS_DIR)/libgui/src/widget.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_DETERMINISTIC_DIR)/gui_test_runtime.o: $(TEST_MOCK)/gui_test_runtime.c $(TEST_MOCK)/gui_test_runtime.h $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_DETERMINISTIC_DIR)/test_gui_platform.o: $(TEST_CASES)/test_gui_platform.c $(TEST_MOCK)/gui_test_runtime.h $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

GUI_PLATFORM_WRAPS := \
    -Wl,--wrap=clock_gettime \
    -Wl,--wrap=calloc \
    -Wl,--wrap=mmap \
    -Wl,--wrap=munmap \
    -Wl,--wrap=open \
    -Wl,--wrap=read \
    -Wl,--wrap=close \
    -Wl,--wrap=gfx_open \
    -Wl,--wrap=gfx_get_info \
    -Wl,--wrap=gfx_acquire \
    -Wl,--wrap=gfx_present \
    -Wl,--wrap=gfx_release \
    -Wl,--wrap=gfx_close \
    -Wl,--wrap=gfx_sprite_blit \
    -Wl,--wrap=gfx_bind_input \
    -Wl,--wrap=gfx_unbind_input \
    -Wl,--wrap=gfx_get_display_state \
    -Wl,--wrap=lv_timer_handler \
    -Wl,--wrap=lv_display_flush_ready \
    -Wl,--wrap=lv_display_create

$(TEST_BLD)/test_gui_platform.elf: \
		$(GUI_DETERMINISTIC_DIR)/test_gui_platform.o \
		$(GUI_DETERMINISTIC_DIR)/gui_test_runtime.o \
		$(GUI_DETERMINISTIC_DIR)/platform.o \
		$(GUI_WIDGET_UNIT_OBJS) \
		$(GUI_THEME_OBJS) \
		$(GUI_DETERMINISTIC_DIR)/tick.o \
		$(GUI_DETERMINISTIC_DIR)/gfx_client_policy.o \
		$(GUI_INPUT_UNIT_OBJS) \
		$(GUI_LVGL_ARCHIVE)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_PLATFORM_WRAPS) -o $@ $^ -lpthread -lm

.PHONY: test-gui-platform
test-gui-platform: $(TEST_BLD)/test_gui_platform.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

$(GUI_INTEGRATION_DIR)/platform.o: $(TESTS_DIR)/libgui/src/platform.c $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -DGUI_INTEGRATION_TEST -MMD -MP -c $< -o $@

$(GUI_INTEGRATION_DIR)/tick.o: $(TESTS_DIR)/libgui/src/tick.c $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -DGUI_INTEGRATION_TEST -MMD -MP -c $< -o $@

$(GUI_INTEGRATION_DIR)/gfx_client_policy.o: $(TESTS_DIR)/user/common/gfx_client_policy.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -DGUI_INTEGRATION_TEST -MMD -MP -c $< -o $@

$(GUI_INTEGRATION_DIR)/gui_test_runtime.o: $(TEST_MOCK)/gui_test_runtime.c $(TEST_MOCK)/gui_test_runtime.h $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -DGUI_INTEGRATION_TEST -MMD -MP -c $< -o $@

$(GUI_INTEGRATION_DIR)/test_gui_lvgl_integration.o: $(TEST_CASES)/test_gui_lvgl_integration.c $(TEST_MOCK)/gui_test_runtime.h $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -DGUI_INTEGRATION_TEST -MMD -MP -c $< -o $@

GUI_INTEGRATION_WRAPS := \
    -Wl,--wrap=clock_gettime \
    -Wl,--wrap=calloc \
    -Wl,--wrap=mmap \
    -Wl,--wrap=munmap \
    -Wl,--wrap=open \
    -Wl,--wrap=read \
    -Wl,--wrap=close \
    -Wl,--wrap=gfx_open \
    -Wl,--wrap=gfx_get_info \
    -Wl,--wrap=gfx_acquire \
    -Wl,--wrap=gfx_present \
    -Wl,--wrap=gfx_release \
    -Wl,--wrap=gfx_close \
    -Wl,--wrap=gfx_sprite_blit \
    -Wl,--wrap=gfx_bind_input \
    -Wl,--wrap=gfx_unbind_input \
    -Wl,--wrap=gfx_get_display_state

$(TEST_BLD)/test_gui_lvgl_integration.elf: \
		$(GUI_INTEGRATION_DIR)/test_gui_lvgl_integration.o \
		$(GUI_INTEGRATION_DIR)/gui_test_runtime.o \
		$(GUI_INTEGRATION_DIR)/platform.o \
		$(GUI_WIDGET_UNIT_OBJS) \
		$(GUI_THEME_OBJS) \
		$(GUI_INTEGRATION_DIR)/tick.o \
		$(GUI_INTEGRATION_DIR)/gfx_client_policy.o \
		$(GUI_INPUT_UNIT_OBJS) \
		$(GUI_LVGL_ARCHIVE)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_INTEGRATION_WRAPS) -o $@ $^ -lpthread -lm

.PHONY: test-gui-lvgl
test-gui-lvgl: $(TEST_BLD)/test_gui_lvgl_integration.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── GUI-3 Task 1: Material design token table ────────────────────
# The token table (libgui/src/theme.c) is pure C with no LVGL dependency,
# so this case links ONLY the token table plus the test TU: the design
# system's values are verified without dragging in the LVGL archive.  The
# assertions cover the spec §9.1 contract: well-formed color roles,
# on_*/base pairing, strictly decreasing font scale, strictly increasing
# space and radius scales, monotonic elevation shadows, the elevation
# surface sequence, and -1 for every out-of-range enum.
GUI_THEME_UNIT_DIR := $(GUI_HOST_DIR)/theme_unit
GUI_THEME_UNIT_OBJS := $(GUI_THEME_UNIT_DIR)/theme.o

$(GUI_THEME_UNIT_DIR)/%.o: $(TESTS_DIR)/libgui/src/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_THEME_UNIT_DIR)/test_gui_theme.o: $(TEST_CASES)/test_gui_theme.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(TEST_BLD)/test_gui_theme.elf: \
		$(GUI_THEME_UNIT_DIR)/test_gui_theme.o \
		$(GUI_THEME_UNIT_OBJS)
	@mkdir -p $(dir $@)
	$(HOST_CC) -o $@ $^

.PHONY: test-gui-theme
test-gui-theme: $(TEST_BLD)/test_gui_theme.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── GUI-3 Task 2: Material theme applied to LVGL ──────────────────
# Links the REAL libgui/src/theme.c (token table) + theme_apply.c (LVGL
# theme) + the REAL platform and the REAL LVGL archive, so the assertions
# observe the production theme rather than a stub.  Covers: the theme is
# attached to the display and re-application is idempotent; container /
# button / label styles come from the token table; the focus ring is a
# 2px primary outline; a NULL platform yields NULL + EINVAL.
$(GUI_INTEGRATION_DIR)/theme.o: $(TESTS_DIR)/libgui/src/theme.c $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -DGUI_INTEGRATION_TEST -MMD -MP -c $< -o $@

$(GUI_INTEGRATION_DIR)/theme_apply.o: $(TESTS_DIR)/libgui/src/theme_apply.c $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -DGUI_INTEGRATION_TEST -MMD -MP -c $< -o $@

$(GUI_INTEGRATION_DIR)/test_gui_theme_apply.o: $(TEST_CASES)/test_gui_theme_apply.c $(TEST_MOCK)/gui_test_runtime.h $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -DGUI_INTEGRATION_TEST -MMD -MP -c $< -o $@

$(TEST_BLD)/test_gui_theme_apply.elf: \
		$(GUI_INTEGRATION_DIR)/test_gui_theme_apply.o \
		$(GUI_INTEGRATION_DIR)/gui_test_runtime.o \
		$(GUI_INTEGRATION_DIR)/platform.o \
		$(GUI_WIDGET_UNIT_OBJS) \
		$(GUI_THEME_OBJS) \
		$(GUI_INTEGRATION_DIR)/tick.o \
		$(GUI_INTEGRATION_DIR)/gfx_client_policy.o \
		$(GUI_INTEGRATION_DIR)/theme.o \
		$(GUI_INTEGRATION_DIR)/theme_apply.o \
		$(GUI_INPUT_UNIT_OBJS) \
		$(GUI_LVGL_ARCHIVE)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_INTEGRATION_WRAPS) -o $@ $^ -lpthread -lm

.PHONY: test-gui-theme-apply
test-gui-theme-apply: $(TEST_BLD)/test_gui_theme_apply.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── GUI-3 Task 3: desktop surface state machine ───────────────────
# The state machine is pure C (user/desktop/desktop_state.c), so this case
# needs neither LVGL nor the platform: only the real state machine plus
# the test TU.
.PHONY: test-gui-desktop-state
test-gui-desktop-state: $(TEST_BLD)/test_desktop_state.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# GUI-3 Task 4: the desktop shell.  Links the REAL desktop shell, registry
# and state machine against the REAL LVGL and the REAL offscreen platform,
# so the pixel assertions observe the production surfaces.  The shell needs
# the LVGL headers and the libgui public headers.
GUI_DESKTOP_DIR := $(GUI_HOST_DIR)/desktop
GUI_DESKTOP_OBJS := \
    $(GUI_DESKTOP_DIR)/desktop_shell.o \
    $(GUI_DESKTOP_DIR)/desktop_apps.o \
    $(GUI_DESKTOP_DIR)/desktop_registry.o \
    $(GUI_DESKTOP_DIR)/desktop_state.o

$(GUI_DESKTOP_DIR)/%.o: $(TESTS_DIR)/user/desktop/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_DESKTOP_DIR)/test_desktop_shell.o: $(TEST_CASES)/test_desktop_shell.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -I$(TESTS_DIR)/user/desktop -MMD -MP -c $< -o $@

$(TEST_BLD)/test_desktop_shell.elf: \
		$(GUI_DESKTOP_DIR)/test_desktop_shell.o \
		$(GUI_DESKTOP_OBJS) \
		$(GUI_INTEGRATION_DIR)/gui_test_runtime.o \
		$(GUI_INTEGRATION_DIR)/platform.o \
		$(GUI_WIDGET_UNIT_OBJS) \
		$(GUI_INTEGRATION_DIR)/tick.o \
		$(GUI_INTEGRATION_DIR)/gfx_client_policy.o \
		$(GUI_THEME_OBJS) \
		$(GUI_INPUT_UNIT_OBJS) \
		$(GUI_LVGL_ARCHIVE)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_INTEGRATION_WRAPS) -o $@ $^ -lpthread -lm

.PHONY: test-gui-desktop-shell
test-gui-desktop-shell: $(TEST_BLD)/test_desktop_shell.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── GUI-3 Task 5: desktop source audit ───────────────────────────
# The design-system discipline ("colors come from the token table") and
# the "no window system" promise are constraints on the SHAPE of the
# code, not on its behaviour, so they are audited statically.  The
# audit reads the real sources from the repository (the path comes from
# the build system, never from the runtime cwd).
$(TEST_BLD)/test_desktop_audit.o: $(TEST_CASES)/test_desktop_audit.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) \
	    -DGUI_DESKTOP_SRC=\"$(TESTS_DIR)/user/desktop\" -c $< -o $@

$(TEST_BLD)/test_desktop_audit.elf: $(TEST_BLD)/test_desktop_audit.o
	$(HOST_CC) -o $@ $^

.PHONY: test-gui-desktop-audit
test-gui-desktop-audit: $(TEST_BLD)/test_desktop_audit.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── GUI-2b Task 1: the native widget registry ─────────────────────
# The widget fixture itself: the host-private native fixture plus the
# public-contract test case.
$(GUI_WIDGET_DIR)/gui_native_fixture.o: $(TEST_MOCK)/gui_native_fixture.c \
		$(TEST_MOCK)/gui_native_fixture.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_WIDGET_DIR)/test_gui_widget.o: $(TEST_CASES)/test_gui_widget.c \
		$(TEST_MOCK)/gui_native_fixture.h
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(TEST_BLD)/test_gui_widget.elf: \
		$(GUI_WIDGET_DIR)/test_gui_widget.o \
		$(GUI_WIDGET_DIR)/gui_native_fixture.o \
		$(GUI_WIDGET_UNIT_OBJS) \
		$(GUI_INTEGRATION_DIR)/gui_test_runtime.o \
		$(GUI_INTEGRATION_DIR)/platform.o \
		$(GUI_THEME_OBJS) \
		$(GUI_INTEGRATION_DIR)/tick.o \
		$(GUI_INTEGRATION_DIR)/gfx_client_policy.o \
		$(GUI_INPUT_UNIT_OBJS) \
		$(GUI_LVGL_ARCHIVE)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_INTEGRATION_WRAPS) -o $@ $^ -lpthread -lm

# The publication boundary of the widget API: a PURE type consumer (only
# the published headers) must compile as C11 and as C++17, and the
# NEGATIVE control — a value handle passed where a void* is expected —
# must NOT compile.  A handle that converts to void* could be forged.
.PHONY: gui-widget-public-types
gui-widget-public-types:
	@mkdir -p $(GUI_WIDGET_DIR)
	@echo "  [gui-widget] public type consumer: C11"
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -fsyntax-only \
	    -I$(TESTS_DIR)/libgui/include \
	    $(TEST_CASES)/gui_widget_public_consumer.c
	@echo "  [gui-widget] public type consumer: C++17"
	$(HOST_CC) -std=c++17 -Wall -Wextra -Werror -fsyntax-only -x c++ \
	    -I$(TESTS_DIR)/libgui/include \
	    $(TEST_CASES)/gui_widget_public_consumer.c
	@echo "  [gui-widget] negative control: the value handle must NOT convert to void*"
	@if $(HOST_CC) -std=c11 -Wall -Wextra \
	    -Werror=incompatible-pointer-types -Werror=int-conversion \
	    -fsyntax-only -I$(TESTS_DIR)/libgui/include \
	    $(TEST_CASES)/gui_widget_public_negative.c \
	    >$(GUI_WIDGET_DIR)/negative_c.log 2>&1; then \
	    echo "ERROR: the value handle was accepted as a void* — the identity is forgeable" >&2; \
	    exit 1; \
	fi
	@grep -q "gui_widget_t" $(GUI_WIDGET_DIR)/negative_c.log || { \
	    echo "ERROR: the negative control failed for the wrong reason" >&2; \
	    cat $(GUI_WIDGET_DIR)/negative_c.log >&2; exit 1; }
	@if $(HOST_CC) -std=c++17 -Wall -Wextra -fsyntax-only -x c++ \
	    -I$(TESTS_DIR)/libgui/include \
	    $(TEST_CASES)/gui_widget_public_negative.c \
	    >$(GUI_WIDGET_DIR)/negative_cpp.log 2>&1; then \
	    echo "ERROR: the value handle was accepted as a void* (C++) — the identity is forgeable" >&2; \
	    exit 1; \
	fi
	@grep -q "gui_widget_t" $(GUI_WIDGET_DIR)/negative_cpp.log || { \
	    echo "ERROR: the negative control failed for the wrong reason (C++)" >&2; \
	    cat $(GUI_WIDGET_DIR)/negative_cpp.log >&2; exit 1; }
	@echo "  [gui-widget] public type consumer: PASS"

.PHONY: test-gui-widget
test-gui-widget: gui-widget-public-types $(TEST_BLD)/test_gui_widget.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $(TEST_BLD)/test_gui_widget.elf

# GUI-2 plan Task 1: file-aware devfs read/poll dispatch.  Host-compiles
# the REAL kernel/fs/devfs.c + kernel/fs/file.c + kernel/fs/poll.c
# (POLL_HOST_CFLAGS / poll_test_runtime.h) and links them against the
# narrow mocks of the layers below that the fixture provides, so the
# assertions observe the production dispatch (devfs_register_chrdev →
# devfs read/poll bridge → fd_read / fd_poll) and not a stub's return
# value.  Tested production symbols: devfs_register_chrdev,
# devfs_get_private, devfs_poll_file, fd_read, fd_poll.
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

# GUI-2 plan Task 2: pure PS/2 Set 1 scan-sequence framer.  The framer
# is pure (no locks, no device IO, no TTY, no display), so the test needs
# no device mock at all: it host-compiles the REAL
# kernel/driver/input_ps2.c and drives it byte by byte.  The assertions
# observe the production framer, never a stub's return value.
# Tested production symbols: input_ps2_frame_reset, input_ps2_frame_feed.
.PHONY: test-gui-input-frame
test-gui-input-frame: $(TEST_BLD)/test_input_ps2_frame.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

$(TEST_BLD)/test_input_ps2_frame.o: $(TEST_CASES)/test_input_ps2_frame.c \
		$(TESTS_DIR)/kernel/include/driver/input_ps2.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) $(KERNEL_INC) -c $< -o $@

$(TEST_BLD)/input_ps2_production.o: $(TESTS_DIR)/kernel/driver/input_ps2.c \
		$(TESTS_DIR)/kernel/include/driver/input_ps2.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -c $< -o $@

$(TEST_BLD)/test_input_ps2_frame.elf: $(TEST_BLD)/test_input_ps2_frame.o \
		$(TEST_BLD)/input_ps2_production.o
	$(HOST_CC) -o $@ $^

# GUI-2 plan Task 3: the exclusive physical-input lease.  The route owns
# the physical key state, the lease and the ACTIVE queues, so the fixture
# host-compiles the REAL kernel/driver/input_route.c (plus the REAL
# kernel/driver/input_ps2.c framer) and links them against the recording
# runtime in hosttests/mock/input_route_test_runtime.{c,h}.
#
# The recording runtime is not a convenience: the route's contract is a
# lock ORDER (route → NORMAL queue/poll) and an ATOMIC "authorize +
# dequeue + register waiter" window.  A no-op mock spinlock cannot show
# either, so the runtime records every acquisition with its nesting
# depth and the assertions look at the recorded order.
#
# Tested production symbols: input_route_init, input_route_open,
# input_route_bind, input_route_unbind, input_route_read,
# input_route_poll, input_route_keyboard_byte, input_route_mouse_event,
# input_route_release_file, input_route_set_normal_sinks.
.PHONY: test-gui-input-route
test-gui-input-route: $(TEST_BLD)/test_input_route.elf $(TEST_BLD)/test_gfx_input.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $(TEST_BLD)/test_input_route.elf \
	    $(TEST_BLD)/test_gfx_input.elf

ROUTE_HOST_CFLAGS := $(HOST_CFLAGS) -ffunction-sections -fdata-sections \
    -Wno-unused-function -D__SPINLOCK_H__ -DOS01_HOST_TEST \
    -include $(TEST_MOCK)/input_route_test_runtime.h \
    -I$(TEST_MOCK) $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)

$(TEST_BLD)/test_input_route.o: $(TEST_CASES)/test_input_route.c \
		$(TESTS_DIR)/kernel/include/driver/input_route.h \
		$(TEST_MOCK)/input_route_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(ROUTE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/input_route_production.o: $(TESTS_DIR)/kernel/driver/input_route.c \
		$(TESTS_DIR)/kernel/include/driver/input_route.h \
		$(TESTS_DIR)/kernel/include/driver/input_ps2.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(ROUTE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/input_route_runtime.o: $(TEST_MOCK)/input_route_test_runtime.c \
		$(TEST_MOCK)/input_route_test_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(ROUTE_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_input_route.elf: $(TEST_BLD)/test_input_route.o \
		$(TEST_BLD)/input_route_production.o \
		$(TEST_BLD)/input_route_runtime.o \
		$(TEST_BLD)/input_ps2_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

# GUI-2 plan Task 4: the gfx input-lease UAPI.  The fixture host-
# compiles the REAL kernel/driver/gfx.c (the GFX_BIND_INPUT /
# GFX_UNBIND_INPUT ioctls), the REAL kernel/driver/fb_state.c (the
# owner / lease coordinator) and the REAL kernel/driver/input_route.c
# (the lease itself) and asserts the spec §4.1 contract: the 32-byte
# INOUT struct, EFAULT / EBADF / EINVAL / EACCES / ESTALE / EBUSY, the
# exclusive lease, the copy-out rollback that must not revoke a newer
# generation, and the libgfx wrapper.
#
# The REAL libgfx/gfx.c is linked through the same --wrap harness the
# gfx client test uses, so the userspace wrapper is observed exactly as
# it calls the kernel.
GFX_INPUT_TEST_CFLAGS := $(HOST_CFLAGS) -DOS01_HOST_TEST -I$(TEST_MOCK) \
    -include $(TEST_MOCK)/fb_resolution_runtime.h \
    $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC) -I$(TESTS_DIR)/libgfx \
    -D'errno=(*__errno_location())' -include $(TEST_INC)/gfx_client_errno.h

$(TEST_BLD)/test_gfx_input.o: $(TEST_CASES)/test_gfx_input.c \
		$(TESTS_DIR)/kernel/include/uapi/gfx.h \
		$(TESTS_DIR)/kernel/include/driver/fb_state.h \
		$(TESTS_DIR)/kernel/include/driver/input_route.h \
		$(TESTS_DIR)/libgfx/gfx.h \
		$(TEST_MOCK)/fb_resolution_runtime.h \
		$(TEST_INC)/test_framework.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GFX_INPUT_TEST_CFLAGS) $(FRAMEWORK_INC) -c $< -o $@

$(TEST_BLD)/test_gfx_input.elf: $(TEST_BLD)/test_gfx_input.o \
		$(TEST_BLD)/gfx_production.o \
		$(TEST_BLD)/fb_state.o \
		$(TEST_BLD)/fb_map.o \
		$(TEST_BLD)/fb_resolution_runtime.o \
		$(TEST_BLD)/input_route_production.o \
		$(TEST_BLD)/input_route_runtime.o \
		$(TEST_BLD)/input_ps2_production.o \
		$(TEST_BLD)/libgfx_gfx_production.o
	$(HOST_CC) -lpthread \
	    -Wl,--wrap=open,--wrap=close,--wrap=ioctl,--wrap=malloc,--wrap=calloc,--wrap=free,--wrap=mmap,--wrap=munmap \
	    -o $@ $^

.PHONY: test-gui-gfx-input
test-gui-gfx-input: $(TEST_BLD)/test_gfx_input.elf
	$(TEST_BLD)/test_gfx_input.elf

# GUI-2 plan Task 5: the native input API surface (libgui/include/gui/
# gui_input.h) and the PURE userspace PS/2 decoder (libgui/src/
# input_ps2.c).  The decoder has no device access, no locks and no LVGL,
# so the fixture needs no mock at all: it host-compiles the REAL
# production decoder and drives it byte by byte, and asserts the spec
# §5.1 / §6.1 contract (US ASCII, the frozen modifier bit layout, the
# UP identity, the typematic make, the swallowed PrintScreen / Pause
# sequences, the 100 ms truncated-prefix expiry, and the keys that
# report their physical identity only).
#
# The decoder TU and the test TU both set errno, so they use the
# repo's errno ABI shim (the same mechanism as the gfx client test):
# hosttests/include/errno.h declares a non-TLS `extern int errno;`
# while host glibc provides a TLS errno, so `errno` is defined as
# glibc's TLS accessor for this binary only.
GUI_INPUT_HOST_CFLAGS := $(HOST_CFLAGS) \
    -I$(TESTS_DIR)/libgui/include -I$(TESTS_DIR)/libgui/internal \
    -I$(TEST_INC) $(LIBC_INC) \
    -D'errno=(*__errno_location())' -include $(TEST_INC)/gfx_client_errno.h

.PHONY: test-gui-input-decode
test-gui-input-decode: $(TEST_BLD)/test_gui_input_decode.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

$(TEST_BLD)/test_gui_input_decode.o: $(TEST_CASES)/test_gui_input_decode.c \
		$(TESTS_DIR)/libgui/include/gui/gui_input.h \
		$(TESTS_DIR)/libgui/internal/gui_input_ps2.h \
		$(TEST_INC)/test_framework.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GUI_INPUT_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/gui_input_ps2_production.o: $(TESTS_DIR)/libgui/src/input_ps2.c \
		$(TESTS_DIR)/libgui/include/gui/gui_input.h \
		$(TESTS_DIR)/libgui/internal/gui_input_ps2.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GUI_INPUT_HOST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_gui_input_decode.elf: $(TEST_BLD)/test_gui_input_decode.o \
		$(TEST_BLD)/gui_input_ps2_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

# GUI-2 plan Task 10 (fix round 1): the guest input probe's nonce rule.
# The nonce is interpolated into the control file's path, so it must be
# hex: a path escape is then impossible by construction.  The fixture
# host-compiles the REAL rule (user/test_gui_input/gui_input_nonce.h, the
# header the guest probe uses) and pins the accept / reject table and the
# fixed control path shape.
.PHONY: test-gui-input-nonce
test-gui-input-nonce: $(TEST_BLD)/test_gui_input_nonce.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

$(TEST_BLD)/test_gui_input_nonce.o: $(TEST_CASES)/test_gui_input_nonce.c \
		$(TESTS_DIR)/user/test_gui_input/gui_input_nonce.h \
		$(TEST_INC)/test_framework.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TESTS_DIR)/user/test_gui_input \
	    -I$(TEST_INC) -c $< -o $@

$(TEST_BLD)/test_gui_input_nonce.elf: $(TEST_BLD)/test_gui_input_nonce.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

# GUI-2 plan Task 6: the bounded input lifecycle, the FIFO, the fair pump
# and the platform error convergence.  The fixture host-compiles the REAL
# libgui input unit (libgui/src/input.c + libgui/src/input_lvgl.c), the
# REAL platform (libgui/src/platform.c) and the REAL PS/2 decoder
# (libgui/src/input_ps2.c) and links them against the recording runtime
# in hosttests/mock/gui_test_runtime.{c,h} plus the REAL LVGL archive.
#
# The assertions observe the production code paths: the creation order
# and its rollback, the FIFO capacity, the per-device pump budgets, the
# poll-flag → error mapping and the platform terminal cleanup.  The LVGL
# adapter hook is wrapped ONLY to inject a create failure; the real hook
# is linked and used by every other assertion (including the 100
# create/destroy cycles on the real LVGL release path).
GUI_INPUT_DIR := $(GUI_HOST_DIR)/input

.PHONY: test-gui-input-platform
test-gui-input-platform: $(TEST_BLD)/test_gui_input_platform.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

$(GUI_INPUT_DIR)/test_gui_input_platform.o: $(TEST_CASES)/test_gui_input_platform.c \
		$(TEST_MOCK)/gui_test_runtime.h $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_INPUT_DIR)/gui_test_runtime.o: $(TEST_MOCK)/gui_test_runtime.c $(TEST_MOCK)/gui_test_runtime.h $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_INPUT_DIR)/platform.o: $(TESTS_DIR)/libgui/src/platform.c $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_INPUT_DIR)/tick.o: $(TESTS_DIR)/libgui/src/tick.c $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_INPUT_DIR)/gfx_client_policy.o: $(TESTS_DIR)/user/common/gfx_client_policy.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

#
# GUI-2 输入单元的共享链接包装：记录型 mock（设备 / 时钟 / gfx）与 LVGL 的
# 观察点。两个 GUI 输入二进制共用这一份清单，避免两份列表各自漂移。
GUI_INPUT_WRAPS_COMMON := \
    -Wl,--wrap=clock_gettime \
    -Wl,--wrap=calloc \
    -Wl,--wrap=mmap \
    -Wl,--wrap=munmap \
    -Wl,--wrap=open \
    -Wl,--wrap=read \
    -Wl,--wrap=close \
    -Wl,--wrap=gfx_open \
    -Wl,--wrap=gfx_get_info \
    -Wl,--wrap=gfx_acquire \
    -Wl,--wrap=gfx_present \
    -Wl,--wrap=gfx_release \
    -Wl,--wrap=gfx_close \
    -Wl,--wrap=gfx_sprite_blit \
    -Wl,--wrap=gfx_bind_input \
    -Wl,--wrap=gfx_unbind_input \
    -Wl,--wrap=gfx_get_display_state \
    -Wl,--wrap=lv_display_flush_ready \
    -Wl,--wrap=lv_display_create \
    -Wl,--wrap=lv_indev_read \
    -Wl,--wrap=gui_input_lvgl_create

# input-platform 额外把 lv_timer_handler 换成只计数的桩（不渲染）。
GUI_INPUT_PLATFORM_WRAPS := $(GUI_INPUT_WRAPS_COMMON) \
    -Wl,--wrap=lv_timer_handler

$(TEST_BLD)/test_gui_input_platform.elf: \
		$(GUI_INPUT_DIR)/test_gui_input_platform.o \
		$(GUI_INPUT_DIR)/gui_test_runtime.o \
		$(GUI_INPUT_DIR)/platform.o \
		$(GUI_WIDGET_UNIT_OBJS) \
		$(GUI_THEME_OBJS) \
		$(GUI_INPUT_DIR)/tick.o \
		$(GUI_INPUT_UNIT_OBJS) \
		$(GUI_INPUT_DIR)/gfx_client_policy.o \
		$(GUI_LVGL_ARCHIVE)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_INPUT_PLATFORM_WRAPS) -o $@ $^ -lpthread -lm

# GUI-2 plan Task 7: the keyboard half of the LVGL adapter — keypad indev
# behaviour, the software repeat policy, native shortcut delivery and the
# in-process focus group with Tab navigation.
#
# The fixture host-compiles the REAL libgui input unit (libgui/src/input.c
# + input_lvgl.c + input_focus.c), the REAL platform and the REAL PS/2
# decoder, and links them against the recording runtime plus the REAL LVGL
# archive.  Nothing under test is mocked: the keypad indev, the focus group
# and the event handling are the production ones.
#
# Synthetic Set 1 bytes enter through the private injection bridge
# (gui_input_test_feed_keybytes), which is compiled ONLY for the host build
# (-DOS01_HOST_TEST) and therefore never reaches the public sysroot.
GUI_INPUT_LVGL_DIR := $(GUI_HOST_DIR)/input_lvgl

# The input-lvgl binary reuses the shared input wrapper set (the keypad read
# observation point) but keeps the REAL lv_timer_handler: the focus outline is
# asserted by sampling rendered pixels, so LVGL must actually refresh the
# display (the shared mock replaces the timer handler with a counting stub
# that never renders).
GUI_INPUT_LVGL_WRAPS := $(GUI_INPUT_WRAPS_COMMON)

.PHONY: test-gui-input-lvgl
test-gui-input-lvgl: $(TEST_BLD)/test_gui_input_lvgl.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

$(GUI_INPUT_LVGL_DIR)/test_gui_input_lvgl.o: $(TEST_CASES)/test_gui_input_lvgl.c \
		$(TEST_MOCK)/gui_test_runtime.h $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_INPUT_LVGL_DIR)/gui_test_runtime.o: $(TEST_MOCK)/gui_test_runtime.c $(TEST_MOCK)/gui_test_runtime.h $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_INPUT_LVGL_DIR)/platform.o: $(TESTS_DIR)/libgui/src/platform.c $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_INPUT_LVGL_DIR)/tick.o: $(TESTS_DIR)/libgui/src/tick.c $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(GUI_INPUT_LVGL_DIR)/gfx_client_policy.o: $(TESTS_DIR)/user/common/gfx_client_policy.c $(GUI_HOST_UAPI_HEADERS)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_HOST_CFLAGS) -MMD -MP -c $< -o $@

$(TEST_BLD)/test_gui_input_lvgl.elf: \
		$(GUI_INPUT_LVGL_DIR)/test_gui_input_lvgl.o \
		$(GUI_INPUT_LVGL_DIR)/gui_test_runtime.o \
		$(GUI_INPUT_LVGL_DIR)/platform.o \
		$(GUI_WIDGET_UNIT_OBJS) \
		$(GUI_THEME_OBJS) \
		$(GUI_INPUT_LVGL_DIR)/tick.o \
		$(GUI_INPUT_LVGL_DIR)/gfx_client_policy.o \
		$(GUI_INPUT_UNIT_OBJS) \
		$(GUI_LVGL_ARCHIVE)
	@mkdir -p $(dir $@)
	$(HOST_CC) $(GUI_INPUT_LVGL_WRAPS) -o $@ $^ -lpthread -lm

-include $(patsubst %.o,%.d,$(GUI_LVGL_OBJS))
-include $(GUI_DETERMINISTIC_DIR)/*.d
-include $(GUI_INTEGRATION_DIR)/*.d
-include $(GUI_INPUT_DIR)/*.d
-include $(GUI_INPUT_LVGL_DIR)/*.d
-include $(GUI_INPUT_UNIT_DIR)/*.d

# Phase2: production decoder/backend/adapters, device/PIC, neutral lookup,
# timer and IPI bodies. Only hardware/context and timer service boundaries
# are spied; the production percpu structure remains in runtime.c.
IRQ_CHAIN_HOST_CFLAGS := $(HOST_CFLAGS) -ffunction-sections -fdata-sections \
    -I$(TEST_MOCK)/intr_irq -I$(TEST_INC) $(KERNEL_INC)
IRQ_CHAIN_SOURCES := arch/x86_64/intr/irq_entry.c \
    arch/x86_64/intr/irq_hooks.c intr/vector_dispatch.c \
    arch/x86_64/intr/pic_8259a.c arch/x86_64/intr/apic/lapic_timer.c \
    arch/x86_64/intr/apic/ipi.c
IRQ_CHAIN_OBJS := $(TEST_BLD)/irq-chain/runtime.o \
    $(addprefix $(TEST_BLD)/irq-chain/,$(IRQ_CHAIN_SOURCES:.c=.o))

$(TEST_BLD)/irq-chain/%.o: $(TESTS_DIR)/kernel/%.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(IRQ_CHAIN_HOST_CFLAGS) $(LIBC_INC) -MMD -MP -c $< -o $@

$(TEST_BLD)/irq-chain/runtime.o: $(TEST_MOCK)/intr_irq/runtime.c
	@mkdir -p $(dir $@)
	$(HOST_CC) $(IRQ_CHAIN_HOST_CFLAGS) $(LIBC_INC) -MMD -MP -c $< -o $@

$(TEST_BLD)/test_intr_irq_x86.o: $(TEST_CASES)/test_intr_irq_x86.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(IRQ_CHAIN_HOST_CFLAGS) -idirafter $(TESTS_DIR)/libc/include -MMD -MP -c $< -o $@

$(TEST_BLD)/test_intr_irq_x86.elf: $(IRQ_CHAIN_OBJS) \
        $(TEST_BLD)/test_intr_irq_x86.o \
        $(TEST_BLD)/x86_intr_backend_production.o \
        $(TEST_BLD)/x86_intr_decode_production.o \
        $(TEST_BLD)/intr_dispatch_production.o
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

.PHONY: test-intr-irq-x86
test-intr-irq-x86: $(TEST_BLD)/test_intr_irq_x86.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) $<

-include $(IRQ_CHAIN_OBJS:.o=.d)
-include $(TEST_BLD)/test_intr_irq_x86.d

# Pure helpers shared verbatim by the scheduled IRQ probe and host fixtures.
$(TEST_BLD)/test_x86_intr_probe.elf: $(TEST_CASES)/test_x86_intr_probe.c $(TESTS_DIR)/kernel/include/selftest/x86_intr.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -idirafter $(TESTS_DIR)/kernel/include $< -o $@

# Exercise production MADT-to-LVT routing with only MMIO substituted.
$(TEST_BLD)/test_lapic_nmi.elf: $(TEST_CASES)/test_lapic_nmi.c $(TESTS_DIR)/kernel/arch/x86_64/intr/apic/nmi.c $(TESTS_DIR)/kernel/include/intr/apic.h $(TESTS_DIR)/kernel/arch/x86_64/intr/apic/acpi.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -Wno-unused-function -ffunction-sections -fdata-sections $(KERNEL_INC) $(LIBC_INC) $(TEST_CASES)/test_lapic_nmi.c $(TESTS_DIR)/kernel/arch/x86_64/intr/apic/nmi.c -Wl,--gc-sections -o $@

.PHONY: test-lapic-nmi
test-lapic-nmi: $(TEST_BLD)/test_lapic_nmi.elf
	$<
