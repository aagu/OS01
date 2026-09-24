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
    -Wno-unused-function -D__SPINLOCK_H__ \
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
=======
    $(TEST_BLD)/test_libc_fread_fwrite_validate.elf \
    $(TEST_BLD)/test_libc_stdio_registry.elf \
    $(TEST_BLD)/test_vfs_basic.elf \
    $(TEST_BLD)/test_fat32_basic.elf \
    $(TEST_BLD)/test_pmm_basic.elf \
    $(TEST_BLD)/test_pmm_ram_rel_index.elf \
    $(TEST_BLD)/test_slab_basic.elf \
    $(TEST_BLD)/test_elf_validate.elf \
    $(TEST_BLD)/test_wait_basic.elf \
    $(TEST_BLD)/test_canon.elf \
    $(TEST_BLD)/test_terminal_core.elf \
    $(TEST_BLD)/test_tetris_logic.elf \
    $(TEST_BLD)/test_sync_primitives.elf \
    $(TEST_BLD)/test_poll_requested.elf \
    $(TEST_BLD)/test_bootinfo_abi.elf \
    $(TEST_BLD)/test_runtime_udivti3.elf \
    $(TEST_BLD)/test_lwip_rand.elf \
    $(TEST_BLD)/test_gic_driver.elf \
    $(TEST_BLD)/test_gic_marker_lines.elf \
    $(TEST_BLD)/test_gic_probe.elf \
    $(TEST_BLD)/test_clocksource.elf

.PHONY: all clean run test-runtime-udivti3 runtime-udivti3-link-probe test_lwip_rand test_gic_driver test_gic_marker_lines test_gic_probe test_clocksource

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

=======
$(TEST_BLD)/test_libc_fread_fwrite_validate.o: $(TEST_CASES)/test_libc_fread_fwrite_validate.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_MOCK) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

# P2-8: stdio-registry cap / fdopen / slot-reuse tests — needs the mock
# shim header for shim_write_count() (currently unused but kept for
# symmetry with test_libc_fflush, which exercises the same code paths).
$(TEST_BLD)/test_libc_stdio_registry.o: $(TEST_CASES)/test_libc_stdio_registry.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_MOCK) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

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

=======
$(TEST_BLD)/test_libc_fread_fwrite_validate.elf: \
		$(TEST_BLD)/test_libc_fread_fwrite_validate.o \
		$(TEST_BLD)/libc_vsprintf.o \
		$(TEST_BLD)/libc_printf.o \
		$(TEST_BLD)/libc_floatconv.o \
		$(TEST_BLD)/stdio_test_shims.o
	$(HOST_CC) -o $@ $^

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

test-libc-fread-fwrite-validate: $(TEST_BLD)/test_libc_fread_fwrite_validate.elf
	$<

$(TEST_BLD)/test_libc_getopt.elf: $(TEST_BLD)/test_libc_getopt.o $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_vfs_basic.elf: $(TEST_BLD)/test_vfs_basic.o $(LIBC_OBJS) $(MOCK_OBJS)

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

$(TEST_BLD)/test_pmm_ram_rel_index.elf: $(TEST_BLD)/test_pmm_ram_rel_index.o \
		$(TEST_BLD)/pmm_production.o $(TEST_BLD)/pmm_arch_default.o
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_slab_basic.elf: $(TEST_BLD)/test_slab_basic.o $(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_elf_validate.elf: $(TEST_BLD)/test_elf_validate.o $(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -o $@ $^

# standalone test (no kernel/libc deps)
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
# kernel/arch/aarch64/gic_driver.c against mock-MMIO arrays (spec §4.1).
# gic_driver.c is dependency-free (no arch asm, no UART logging); the
# pt_regs type is forward-declared in <arch/aarch64/gic.h> and the test
# never defines a pt_regs object (R1-4: dispatch cases pass NULL).
GIC_HOST_CFLAGS := $(HOST_CFLAGS) $(KERNEL_INC)

$(TEST_BLD)/gic_driver_production.o: $(TESTS_DIR)/kernel/arch/aarch64/gic_driver.c \
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

# GICv2 marker lines: source-level RED check (Task 2.1). QEMU aarch64-uefi
# is currently blocked by thirdpart/{posix-uefi,mbedtls,lwip} submodule
# emptiness; the marker lines that the kernel would emit are checked at
# the source-file level instead (full RED on the missing markers, GREEN
# once Task 2.2 wires them up in main.c / irq_probe.c).
GIC_MARKER_CFLAGS := $(HOST_CFLAGS) -DOS01_KERNEL_SRC=\"$(TESTS_DIR)\" \
                     -I$(TEST_INC) -include $(TEST_MOCK)/test_platform.h

$(TEST_BLD)/test_gic_marker_lines.o: $(TEST_CASES)/test_gic_marker_lines.c \
		$(TESTS_DIR)/kernel/arch/aarch64/gic.c \
		$(TESTS_DIR)/kernel/arch/aarch64/main.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(GIC_MARKER_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_gic_marker_lines.elf: $(TEST_BLD)/test_gic_marker_lines.o
	$(HOST_CC) -o $@ $<

test_gic_marker_lines: $(TEST_BLD)/test_gic_marker_lines.elf
	$<

# GIC clobber-probe encoding contract — host-side RED for the QEMU TIMEOUT bug.
# Same compile pattern as test_gic_marker_lines: source-level check via
# OS01_KERNEL_SRC. The C wrapper (gic_send_sgi) is exercised against the
# production gic_driver.c (mock MMIO) AND the asm source is scanned for the
# known-buggy immediate literal.
$(TEST_BLD)/test_gic_probe.o: $(TEST_CASES)/test_gic_probe.c \
		$(TESTS_DIR)/kernel/arch/aarch64/irq_probe.c \
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

clean:
	rm -rf $(TEST_BLD)
