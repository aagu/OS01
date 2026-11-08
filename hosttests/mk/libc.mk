# ── Libc Subsystem Host Tests (spec §5) ────────────────────────

$(eval $(call register_host_test,test_libc_string,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_list,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_vsprintf,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_fflush,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_stdio_registry,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_fread_fwrite_validate,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_getopt,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_aarch64_color_printk_abi,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_api,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_alloc,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_environ,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_shutdown,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_stream_state,libc,protocol-or-legacy))

TEST_BINS += \
    $(TEST_BLD)/test_libc_string.elf \
    $(TEST_BLD)/test_libc_list.elf \
    $(TEST_BLD)/test_libc_vsprintf.elf \
    $(TEST_BLD)/test_libc_fflush.elf \
    $(TEST_BLD)/test_libc_stdio_registry.elf \
    $(TEST_BLD)/test_libc_fread_fwrite_validate.elf \
    $(TEST_BLD)/test_libc_getopt.elf \
    $(TEST_BLD)/test_aarch64_color_printk_abi.elf \
    $(TEST_BLD)/test_libc_api.elf \
    $(TEST_BLD)/test_libc_alloc.elf \
    $(TEST_BLD)/test_libc_environ.elf \
    $(TEST_BLD)/test_libc_shutdown.elf \
    $(TEST_BLD)/test_libc_stream_state.elf

# getopt test links against libc_getopt.o whose globals are declared by <getopt.h>; build with -fcommon
$(TEST_BLD)/test_libc_getopt.o: $(TEST_CASES)/test_libc_getopt.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -fcommon $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_libc_getopt.elf: $(TEST_BLD)/test_libc_getopt.o $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

# vsprintf test exercises printf/vasprintf/write_all with the scripted write shim
$(TEST_BLD)/test_libc_vsprintf.o: $(TEST_CASES)/test_libc_vsprintf.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_MOCK) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_libc_vsprintf.elf: $(TEST_BLD)/test_libc_vsprintf.o $(LIBC_OBJS) \
		$(TEST_BLD)/libc_vasprintf.o $(TEST_BLD)/libc_printf.o \
		$(TEST_BLD)/libc_stdio_file.o $(TEST_BLD)/stdio_test_shims.o
	$(HOST_CC) -o $@ $^

# fflush test exercises printf + fflush + fopen/fclose + write-shim capture
$(TEST_BLD)/test_libc_fflush.o: $(TEST_CASES)/test_libc_fflush.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_MOCK) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_libc_fflush.elf: $(TEST_BLD)/test_libc_fflush.o $(LIBC_OBJS) \
		$(TEST_BLD)/libc_vasprintf.o $(TEST_BLD)/libc_printf.o \
		$(TEST_BLD)/libc_stdio_file.o $(TEST_BLD)/stdio_test_shims.o
	$(HOST_CC) -o $@ $^

# stdio-registry cap / fdopen / slot-reuse tests
$(TEST_BLD)/test_libc_stdio_registry.o: $(TEST_CASES)/test_libc_stdio_registry.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_MOCK) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_libc_stdio_registry.elf: \
		$(TEST_BLD)/test_libc_stdio_registry.o \
		$(TEST_BLD)/libc_stdio_file.o \
		$(TEST_BLD)/libc_stdio_extras.o \
		$(TEST_BLD)/libc_vsprintf.o \
		$(TEST_BLD)/libc_printf.o \
		$(TEST_BLD)/libc_floatconv.o \
		$(TEST_BLD)/stdio_test_shims.o
	$(HOST_CC) -o $@ $^

.PHONY: test-libc-stdio-registry
test-libc-stdio-registry: $(TEST_BLD)/test_libc_stdio_registry.elf
	$<

# fread/fwrite stream-validation test
$(TEST_BLD)/test_libc_fread_fwrite_validate.o: $(TEST_CASES)/test_libc_fread_fwrite_validate.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_libc_fread_fwrite_validate.elf: \
		$(TEST_BLD)/test_libc_fread_fwrite_validate.o \
		$(TEST_BLD)/libc_stdio_file.o \
		$(TEST_BLD)/libc_vsprintf.o \
		$(TEST_BLD)/libc_printf.o \
		$(TEST_BLD)/libc_floatconv.o \
		$(TEST_BLD)/stdio_test_shims.o
	$(HOST_CC) -o $@ $^

.PHONY: test-libc-fread-fwrite-validate
test-libc-fread-fwrite-validate: $(TEST_BLD)/test_libc_fread_fwrite_validate.elf
	$<

$(TEST_BLD)/test_libc_string.elf: $(TEST_BLD)/test_libc_string.o $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

$(TEST_BLD)/test_libc_list.elf: $(TEST_BLD)/test_libc_list.o $(LIBC_OBJS)
	$(HOST_CC) -o $@ $^

# color_printk / serial_printk ABI + behaviour
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

.PHONY: test-aarch64-color-printk-abi
test-aarch64-color-printk-abi: $(TEST_BLD)/test_aarch64_color_printk_abi.elf
	$(TEST_BLD)/test_aarch64_color_printk_abi.elf

# ── libc completion: api group (T1) ─────────────────────────────────
# The api group audits the PUBLIC HEADER contract of the OS01 libc:
# every public header is self-contained, has exactly one definition
# across translation units, and gives its declarations C linkage so a
# C++ consumer links against the same symbols.
#
# The manifest (which headers, which interfaces, which group owns them)
# lives in scripts/audit_libc_api.py — the single source of truth.  It is
# emitted as a C header for the test, so the test and the audit script
# can never disagree about the inventory.
#
# The binary links the REAL production libc string objects under their
# `os01_` prefix (see common.mk), so a call in the test can never
# resolve to the host libc's same-named implementation.
LIBC_API_MANIFEST := $(TEST_BLD)/libc_api_manifest.h
LIBC_API_PROBE_DIR := $(TEST_BLD)/api-probe

$(LIBC_API_MANIFEST): $(TESTS_DIR)/scripts/audit_libc_api.py
	@mkdir -p $(TEST_BLD)
	python3 $< --emit-c-header $@

$(TEST_BLD)/test_libc_api.o: $(TEST_CASES)/test_libc_api.c \
		$(LIBC_API_MANIFEST) $(TEST_MOCK)/libc_completion_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(OS01_LIBC_PREFIX_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) \
		-I$(TEST_MOCK) -I$(TEST_BLD) \
		-DOS01_API_CC='"$(HOST_CC)"' \
		-DOS01_API_CXX='"$(HOST_CXX)"' \
		-DOS01_API_NM='"$(NM_TOOL)"' \
		-DOS01_API_INCLUDE_DIR='"$(LIBC_INC_DIR)"' \
		-DOS01_API_PROBE_DIR='"$(LIBC_API_PROBE_DIR)"' \
		-DOS01_API_PREFIX_DEFS='"$(OS01_LIBC_PREFIX_DEFS)"' \
		-DOS01_API_PREFIXED_OBJS='"$(OS01_LIBC_PREFIX_OBJS)"' \
		-c $< -o $@

$(TEST_BLD)/test_libc_api.elf: $(TEST_BLD)/test_libc_api.o $(OS01_LIBC_PREFIX_OBJS)
	@mkdir -p $(LIBC_API_PROBE_DIR)
	$(HOST_CC) -o $@ $^

.PHONY: test-libc-api
test-libc-api: $(TEST_BLD)/test_libc_api.elf
	@# The manifest is only regenerated when the script or a header changes,
	@# so the drift check cannot live inside the manifest rule: it would be
	@# skipped on an ordinary rebuild.  Running it here checks the headers
	@# against the manifest on EVERY invocation of the api group.
	python3 $(TESTS_DIR)/scripts/audit_libc_api.py --check-headers \
	    --include-dir $(LIBC_INC_DIR)
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── libc completion: alloc group (T2) ─────────────────────────────────
# The alloc group exercises the REAL production allocator: libc/stdlib/
# malloc.c, calloc.c and reallocarray.c, compiled under the `os01_` prefix
# (see common.mk) so a call can never resolve to the host libc's
# same-named implementation.
#
# The allocator's heap is the mock in hosttests/mock/libc_heap_runtime.h:
# the production allocator grows its heap with SYS_brk, and the test
# translation unit supplies the syscall boundary (host_test_syscall) plus
# the ENOMEM growth fault injection.
$(TEST_BLD)/test_libc_alloc.o: $(TEST_CASES)/test_libc_alloc.c \
		$(TEST_MOCK)/libc_heap_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(OS01_LIBC_PREFIX_CFLAGS) \
		$(OS01_LIBC_ALLOC_PREFIX_DEFS) $(FRAMEWORK_INC) $(LIBC_INC) \
		-I$(TEST_MOCK) -c $< -o $@

$(TEST_BLD)/test_libc_alloc.elf: $(TEST_BLD)/test_libc_alloc.o \
		$(OS01_LIBC_PREFIX_OBJS) $(OS01_LIBC_ALLOC_OBJS)
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) -o $@ $^

.PHONY: test-libc-alloc
test-libc-alloc: $(TEST_BLD)/test_libc_alloc.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── libc completion: env group (T3) ─────────────────────────────────
# The env group drives the REAL production environment module
# (libc/stdlib/environ.c) under the `os01_` prefix, so a call in the
# test can never resolve to the host libc's same-named implementation.
#
# The production object is compiled WITHOUT the test_platform.h
# force-include: that header defines __is_libk, and the kernel-side copy
# of the module (which has no user errno) is not the one under test.
# The host test drives the USER environment.
#
# The allocator is the one boundary that is replaced: the module
# allocates its strings and its table through malloc/free/realloc, and
# the test supplies a ledger for those entry points so a chosen
# allocation can fail and so the test can see which strings the libc
# owns.
OS01_LIBC_ENV_FNS := getenv setenv unsetenv putenv clearenv environ
OS01_LIBC_ENV_PREFIX_DEFS := $(foreach f,$(OS01_LIBC_ENV_FNS),-D$(f)=$(OS01_LIBC_PREFIX)$(f))
OS01_LIBC_ENV_ALLOC_DEFS := \
    -Dmalloc=$(OS01_LIBC_PREFIX)malloc \
    -Dfree=$(OS01_LIBC_PREFIX)free \
    -Drealloc=$(OS01_LIBC_PREFIX)realloc
OS01_LIBC_ENV_DIR := $(TEST_BLD)/prefixed-env
OS01_LIBC_ENV_OBJS := $(OS01_LIBC_ENV_DIR)/environ.o

$(OS01_LIBC_ENV_DIR)/%.o: $(LIBC_SRC)/stdlib/%.c
	@mkdir -p $(OS01_LIBC_ENV_DIR)
	$(HOST_CC) $(HOST_CFLAGS) $(OS01_LIBC_ENV_PREFIX_DEFS) \
		$(OS01_LIBC_ENV_ALLOC_DEFS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_libc_environ.o: $(TEST_CASES)/test_libc_environ.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(OS01_LIBC_ENV_PREFIX_DEFS) \
		$(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_libc_environ.elf: $(TEST_BLD)/test_libc_environ.o \
		$(OS01_LIBC_ENV_OBJS)
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) -o $@ $^

.PHONY: test-libc-environ
test-libc-environ: $(TEST_BLD)/test_libc_environ.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── libc completion: shutdown group (T4) ─────────────────────────────
# The shutdown group drives the REAL production wrapper
# (libc/unistd/shutdown.c) under the `os01_` prefix.  The only thing
# that is replaced is the syscall boundary: <sys/syscall.h> routes every
# syscall through the `host_test_syscall` hook under OS01_HOST_TEST, so
# the test sees the exact syscall number and arguments.
#
# The production object is compiled WITHOUT the test_platform.h
# force-include: that header defines __is_libk, and the kernel-side copy
# of the wrapper (which fails explicitly) is not the one under test.
OS01_LIBC_SHUTDOWN_FNS := shutdown
OS01_LIBC_SHUTDOWN_PREFIX_DEFS := $(foreach f,$(OS01_LIBC_SHUTDOWN_FNS),-D$(f)=$(OS01_LIBC_PREFIX)$(f))
OS01_LIBC_SHUTDOWN_DIR := $(TEST_BLD)/prefixed-shutdown
OS01_LIBC_SHUTDOWN_OBJS := $(OS01_LIBC_SHUTDOWN_DIR)/shutdown.o

$(OS01_LIBC_SHUTDOWN_DIR)/%.o: $(LIBC_SRC)/unistd/%.c
	@mkdir -p $(OS01_LIBC_SHUTDOWN_DIR)
	$(HOST_CC) $(HOST_CFLAGS) $(OS01_LIBC_SHUTDOWN_PREFIX_DEFS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_libc_shutdown.o: $(TEST_CASES)/test_libc_shutdown.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(OS01_LIBC_SHUTDOWN_PREFIX_DEFS) \
		$(FRAMEWORK_INC) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_libc_shutdown.elf: $(TEST_BLD)/test_libc_shutdown.o \
		$(OS01_LIBC_SHUTDOWN_OBJS)
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) -o $@ $^

.PHONY: test-libc-shutdown
test-libc-shutdown: $(TEST_BLD)/test_libc_shutdown.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── libc completion: stream group (T5) ─────────────────────────────────
# The stream group drives the REAL production stream core
# (libc/stdio/stdio_file.c, stdio_extras.c, printf.c, putchar.c, puts.c,
# getchar_unlocked.c).  They are linked as they are — no renamed copies,
# no re-implementation — so a call in the test can never resolve to the
# host libc's same-named implementation.
#
# The boundary that IS replaced is the descriptor boundary: read(),
# write() and close() (hosttests/mock/libc_stream_runtime.c).  The mock
# serves scripted descriptors and passes every other descriptor on to
# the host kernel, so the test's own output still reaches the terminal
# while the stream state under test stays fully production code.
# The stream objects are compiled WITHOUT the test_platform.h
# force-include: that header defines __is_libk, and the kernel-side copy
# of the module is not the one under test.  The stream group drives the
# USER libc, exactly like the shipped libc.a is built.
OS01_LIBC_STREAM_DIR := $(TEST_BLD)/user-std
OS01_LIBC_STREAM_OBJS := \
    $(OS01_LIBC_STREAM_DIR)/stdio_file.o \
    $(OS01_LIBC_STREAM_DIR)/stdio_extras.o \
    $(OS01_LIBC_STREAM_DIR)/printf.o \
    $(OS01_LIBC_STREAM_DIR)/putchar.o \
    $(OS01_LIBC_STREAM_DIR)/puts.o \
    $(OS01_LIBC_STREAM_DIR)/getchar_unlocked.o \
    $(OS01_LIBC_STREAM_DIR)/vsprintf.o \
    $(OS01_LIBC_STREAM_DIR)/vsnprintf.o \
    $(OS01_LIBC_STREAM_DIR)/sprintf.o \
    $(OS01_LIBC_STREAM_DIR)/floatconv.o

$(OS01_LIBC_STREAM_DIR)/%.o: $(LIBC_SRC)/stdio/%.c
	@mkdir -p $(OS01_LIBC_STREAM_DIR)
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/libc_stream_runtime.o: $(TEST_MOCK)/libc_stream_runtime.c \
		$(TEST_MOCK)/libc_stream_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_MOCK) -I$(TEST_INC) -c $< -o $@

$(TEST_BLD)/test_libc_stream_state.o: $(TEST_CASES)/test_libc_stream_state.c \
		$(TEST_MOCK)/libc_stream_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TEST_MOCK) -c $< -o $@

$(TEST_BLD)/test_libc_stream_state.elf: $(TEST_BLD)/test_libc_stream_state.o \
		$(OS01_LIBC_STREAM_OBJS) \
		$(TEST_BLD)/libc_stream_runtime.o
	$(HOST_CC) -o $@ $^

.PHONY: test-libc-stream-state
test-libc-stream-state: $(TEST_BLD)/test_libc_stream_state.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<
