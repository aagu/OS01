TESTS_DIR := $(realpath ..)
TEST_SRC  := $(TESTS_DIR)/test
TEST_BLD := $(TEST_SRC)/build
TEST_MOCK := $(TEST_SRC)/mock
TEST_INC  := $(TEST_SRC)/include
TEST_CASES := $(TEST_SRC)/cases

HOST_CC   := clang

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
    $(TEST_BLD)/test_libc_getopt.elf \
    $(TEST_BLD)/test_vfs_basic.elf \
    $(TEST_BLD)/test_fat32_basic.elf \
    $(TEST_BLD)/test_pmm_basic.elf \
    $(TEST_BLD)/test_slab_basic.elf \
    $(TEST_BLD)/test_elf_validate.elf \
    $(TEST_BLD)/test_wait_basic.elf \
    $(TEST_BLD)/test_canon.elf \
    $(TEST_BLD)/test_terminal_core.elf \
    $(TEST_BLD)/test_tetris_logic.elf \
    $(TEST_BLD)/test_sync_primitives.elf \
    $(TEST_BLD)/test_poll_requested.elf \
    $(TEST_BLD)/test_bootinfo_abi.elf \
    $(TEST_BLD)/test_aarch64_elf_loader.elf

.PHONY: all clean run

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
	    if [ "$${rc}" -ne 0 ] && [ "$${rc}" -ne 1 ]; then \
	        echo "  >>> $$name CRASHED (rc=$$rc) <<<"; \
	        failed=$$((failed + 1)); \
	    elif [ "$${rc}" -eq 1 ]; then \
	        failed=$$((failed + 1)); \
	    fi; \
	    echo "  -> exit code: $$rc"; \
	done; \
	echo ""; \
	echo "=============================================="; \
	echo "  Suites: $$total | Failed: $$failed"; \
	echo "=============================================="; \
	exit $$failed

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

# Focused poll test compiles and links the real production paths.
$(TEST_BLD)/test_poll_requested.o: $(TEST_CASES)/test_poll_requested.c
	@mkdir -p $(TEST_BLD)
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

$(TEST_BLD)/test_bootinfo_abi.elf: $(TEST_BLD)/test_bootinfo_abi.o \
		$(TEST_BLD)/aarch64_boot_mode.o
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_bootinfo_abi.o: $(TEST_CASES)/test_bootinfo_abi.c \
		$(TESTS_DIR)/kernel/include/kernel/bootinfo.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TESTS_DIR)/kernel/include -c $< -o $@

$(TEST_BLD)/aarch64_boot_mode.o: $(TESTS_DIR)/kernel/arch/aarch64/main.c \
		$(TESTS_DIR)/kernel/include/kernel/bootinfo.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -DAARCH64_BOOT_MODE_HOST_TEST \
		-I$(TESTS_DIR)/kernel/include -c $< -o $@

$(TEST_BLD)/test_libc_list.elf: $(TEST_BLD)/test_libc_list.o $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_libc_vsprintf.elf: $(TEST_BLD)/test_libc_vsprintf.o $(LIBC_OBJS) \
		$(TEST_BLD)/libc_vasprintf.o $(TEST_BLD)/libc_printf.o \
		$(TEST_BLD)/libc_stdio_file.o $(TEST_BLD)/stdio_test_shims.o
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_libc_getopt.elf: $(TEST_BLD)/test_libc_getopt.o $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_vfs_basic.elf: $(TEST_BLD)/test_vfs_basic.o $(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_fat32_basic.elf: $(TEST_BLD)/test_fat32_basic.o $(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_pmm_basic.elf: $(TEST_BLD)/test_pmm_basic.o $(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_slab_basic.elf: $(TEST_BLD)/test_slab_basic.o $(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_elf_validate.elf: $(TEST_BLD)/test_elf_validate.o $(LIBC_OBJS) $(MOCK_OBJS)
	$(HOST_CC) -o $@ $^

# AArch64 UEFI ELF loader helpers are host-tested as pure fixed-address logic.
$(TEST_BLD)/test_aarch64_elf_loader.o: $(TEST_CASES)/test_aarch64_elf_loader.c \
		$(TESTS_DIR)/boot/uefi/aarch64/loader.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TESTS_DIR)/boot/uefi/aarch64 -c $< -o $@

$(TEST_BLD)/aarch64_elf_loader.o: $(TESTS_DIR)/boot/uefi/aarch64/elf.c \
		$(TESTS_DIR)/boot/uefi/aarch64/loader.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TESTS_DIR)/boot/uefi/aarch64 -c $< -o $@

$(TEST_BLD)/test_aarch64_elf_loader.elf: \
		$(TEST_BLD)/test_aarch64_elf_loader.o \
		$(TEST_BLD)/aarch64_elf_loader.o
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

# Keep the focused command usable from the repository root/task plan.
build/test_poll_requested.elf: $(TEST_BLD)/test_poll_requested.o $(POLL_PRODUCTION_OBJS)
	$(HOST_CC) -Wl,--gc-sections -o $@ $^

clean:
	rm -rf $(TEST_BLD)
