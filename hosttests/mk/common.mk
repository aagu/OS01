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

KERNEL_INC := -I$(TESTS_DIR)/kernel/include
LIBC_INC := -I$(TESTS_DIR)/libc/include
FRAMEWORK_INC := -I$(TEST_INC) -include $(TEST_MOCK)/test_platform.h

HOST_CFLAGS := -g -O0 -Wall -Wextra -Wno-unused-parameter \
               -Wno-address-of-packed-member -Wno-gnu-statement-expression \
               -Wno-macro-redefined -Wno-dangling-else \
               -Wno-non-literal-null-conversion -Wno-incompatible-library-redeclaration \
               -MMD -MP

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

$(TEST_BLD)/stdio_test_shims.o: $(TEST_MOCK)/stdio_test_shims.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_MOCK)/include -I$(TEST_INC) -c $< -o $@

$(TEST_BLD)/mock_kernel.o: $(TEST_MOCK)/mock_kernel.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) -g -O0 -c $< -o $@

# Default test case compilation
$(TEST_BLD)/%.o: $(TEST_CASES)/%.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@
