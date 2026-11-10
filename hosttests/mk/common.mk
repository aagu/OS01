# ── Common paths, compiler flags, and shared helper objects (spec §5) ──

TESTS_DIR := $(realpath ..)
TEST_SRC  := $(TESTS_DIR)/hosttests
TEST_BLD := $(HOST_TEST_BUILD_DIR)
TEST_MOCK := $(TEST_SRC)/mock
TEST_INC  := $(TEST_SRC)/include
TEST_CASES := $(TEST_SRC)/cases

COMPILER_LAUNCHER ?=
HOST_CC_BASE ?= clang
HOST_CC      := $(if $(strip $(COMPILER_LAUNCHER)),$(strip $(COMPILER_LAUNCHER)) )$(HOST_CC_BASE)
HOST_CXX_BASE ?= clang++
HOST_CXX     := $(if $(strip $(COMPILER_LAUNCHER)),$(strip $(COMPILER_LAUNCHER)) )$(HOST_CXX_BASE)

# nm is used by the host harnesses that inspect an object file's symbol
# table (the libc api group checks the C linkage of the declarations).
NM_TOOL ?= $(shell command -v nm 2>/dev/null || command -v llvm-nm 2>/dev/null)

KERNEL_INC := -I$(TESTS_DIR)/kernel/include
LIBC_INC_DIR := $(TESTS_DIR)/libc/include
LIBC_INC := -I$(LIBC_INC_DIR)
FRAMEWORK_INC := -I$(TEST_INC) -include $(TEST_MOCK)/test_platform.h

HOST_CFLAGS := -g -O0 -Wall -Wextra -Wno-unused-parameter \
               -Wno-address-of-packed-member -Wno-gnu-statement-expression \
               -Wno-macro-redefined -Wno-dangling-else \
               -Wno-non-literal-null-conversion -Wno-incompatible-library-redeclaration \
               -DOS01_HOST_TEST \
               -MMD -MP

POLL_HOST_CFLAGS := $(HOST_CFLAGS) -ffunction-sections -fdata-sections \
    -Wno-unused-function -D__SPINLOCK_H__ -DOS01_HOST_TEST \
    -include $(TEST_MOCK)/poll_test_runtime.h \
    -I$(TEST_MOCK)/include $(KERNEL_INC) $(LIBC_INC) -I$(TEST_INC)

SYNC_HOST_CFLAGS := $(HOST_CFLAGS) -I$(TEST_INC) $(KERNEL_INC)

LIBC_SRC := $(TESTS_DIR)/libc

# ── Host-test production-symbol prefix ──────────────────────────────
# A host test must never mistake the host libc's implementation of a
# POSIX name for the OS01 implementation.  The relevant production libc
# sources are therefore compiled for the host test under an `os01_`
# prefix, and the test translation units call the prefixed names.
#
# ONE mechanism is used, consistently, on both sides: one -D rename per
# prefixed function.  The flags are applied to
#
#   1. the production sources compiled for the host test
#      ($(OS01_LIBC_PREFIX_OBJS)), and
#   2. the test translation units that call them.
#
# The prefix is a HOST-TEST-ONLY convention: libc.a and libk.a are built
# without these flags, so an `os01_` name never reaches a shipped
# artifact and the userland ABI of libc/include/*.h is unchanged.
#
# Only true boundary functions are replaced with test doubles (the raw
# syscall entry, read/write and the allocator entry points needed for
# fault injection).  Everything else under test is real production code.
#
# NOTE: $(LIBC_OBJS) below is the LEGACY path — the pre-existing libc host
# tests link the unprefixed production objects.  It still works (the
# objects define the symbol, so the host libc cannot interpose), but it is
# a second convention.  NEW host tests must use the prefixed objects
# ($(OS01_LIBC_PREFIX_OBJS)) instead; migrating the legacy tests is a
# separate cleanup, tracked in the plan's ledger.
OS01_LIBC_PREFIX := os01_
OS01_LIBC_PREFIXED_FNS := memcpy memset memmove memcmp strlen strcmp strncmp strcpy
OS01_LIBC_PREFIX_DEFS := $(foreach f,$(OS01_LIBC_PREFIXED_FNS),-D$(f)=$(OS01_LIBC_PREFIX)$(f))
OS01_LIBC_PREFIX_CFLAGS := $(OS01_LIBC_PREFIX_DEFS)
OS01_LIBC_PREFIX_DIR := $(TEST_BLD)/prefixed
OS01_LIBC_PREFIX_OBJS := $(addprefix $(OS01_LIBC_PREFIX_DIR)/,$(addsuffix .o,$(OS01_LIBC_PREFIXED_FNS)))

# The same production sources, compiled with the prefix.  These are the
# REAL libc/string/*.c implementations.
$(OS01_LIBC_PREFIX_DIR)/%.o: $(LIBC_SRC)/string/%.c
	@mkdir -p $(OS01_LIBC_PREFIX_DIR)
	$(HOST_CC) $(HOST_CFLAGS) $(OS01_LIBC_PREFIX_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

# ── Host-test production-symbol prefix: the allocator ──────────────────
# The allocator lives in different production sources (libc/stdlib/*.c), so
# it gets its own prefixed object set.  It is one object per SOURCE FILE,
# not one per symbol: free() and realloc() are defined in malloc.c.
#
# The allocator objects are compiled WITHOUT the test_platform.h
# force-include.  That header defines __is_libk, and the allocator's kernel
# path (the slab allocator) is not what the host test drives: the host test
# exercises the USER allocator, the SYS_brk heap.  Everything else about the
# production code stays untouched — same sources, same compiler flags.
OS01_LIBC_ALLOC_FNS := malloc free calloc realloc reallocarray posix_memalign aligned_alloc
OS01_LIBC_ALLOC_PREFIX_DEFS := $(foreach f,$(OS01_LIBC_ALLOC_FNS),-D$(f)=$(OS01_LIBC_PREFIX)$(f))
OS01_LIBC_ALLOC_DIR := $(TEST_BLD)/prefixed-alloc
OS01_LIBC_ALLOC_OBJS := \
    $(OS01_LIBC_ALLOC_DIR)/malloc.o \
    $(OS01_LIBC_ALLOC_DIR)/calloc.o \
    $(OS01_LIBC_ALLOC_DIR)/reallocarray.o

$(OS01_LIBC_ALLOC_DIR)/%.o: $(LIBC_SRC)/stdlib/%.c
	@mkdir -p $(OS01_LIBC_ALLOC_DIR)
	$(HOST_CC) $(HOST_CFLAGS) $(OS01_LIBC_ALLOC_PREFIX_DEFS) $(LIBC_INC) -c $< -o $@

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

# ── Shared Libc Object Compilation ───────────────────────────

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

$(TEST_BLD)/libc_stdio_extras.o: $(LIBC_SRC)/stdio/stdio_extras.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/libc_putchar.o: $(LIBC_SRC)/stdio/putchar.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/libc_puts.o: $(LIBC_SRC)/stdio/puts.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/libc_getchar_unlocked.o: $(LIBC_SRC)/stdio/getchar_unlocked.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/stdio_test_shims.o: $(TEST_MOCK)/stdio_test_shims.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_MOCK)/include -I$(TEST_INC) -c $< -o $@

$(TEST_BLD)/mock_kernel.o: $(TEST_MOCK)/mock_kernel.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) -g -O0 -c $< -o $@

# ARCH-10 Task 6: the RLIMIT_NOFILE soft-limit snapshot stub for the
# host harnesses that compile kernel/fs/file.c against a reduced mock
# task_t (poll_test_runtime.h / gfx_test_runtime.h).  The production
# kernel/sched/resource.c cannot be compiled against those mocks, so
# every binary that links such a file.o MUST link this stub — the
# link must not depend on --gc-sections dropping the install
# functions.  See hosttests/mock/nofile_soft_stub.c.
$(TEST_BLD)/nofile_soft_stub.o: $(TEST_MOCK)/nofile_soft_stub.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(KERNEL_INC) -c $< -o $@

NOFILE_SOFT_STUB_OBJ := $(TEST_BLD)/nofile_soft_stub.o

# libc-completion Task T6: the O_APPEND call site in kernel/fs/file.c
# dispatches through vfs_append() (kernel/fs/vfs.c).  The harnesses that
# compile kernel/fs/file.c WITHOUT kernel/fs/vfs.c link this stub so the
# link stays closed; the real VFS append test links the production object
# instead and must NOT link this stub.  See
# hosttests/mock/vfs_append_link_stub.c.
$(TEST_BLD)/vfs_append_link_stub.o: $(TEST_MOCK)/vfs_append_link_stub.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -c $< -o $@

VFS_APPEND_LINK_STUB_OBJ := $(TEST_BLD)/vfs_append_link_stub.o

# Default test case compilation
$(TEST_BLD)/%.o: $(TEST_CASES)/%.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@
