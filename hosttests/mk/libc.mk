# ── Libc Subsystem Host Tests (spec §5) ────────────────────────

# Shared host oracle binaries.  These are separate host programs built
# with the HOST compiler and no OS01 code; the strtox/floatscan/scanf/
# time tests execute them through *_ORACLE_PATH and compare the result
# with the OS01 implementation.  They are NOT TEST_BINS (the runner must
# not execute them as tests), so the aggregate `all`/`run` goals must
# list them explicitly -- otherwise a clean-tree `make test-host` fails
# every oracle-backed test with "oracle produced only 0 of N rows".
LIBC_HOST_ORACLES := \
    $(TEST_BLD)/strtox_oracle \
    $(TEST_BLD)/floatscan_oracle \
    $(TEST_BLD)/scanf_oracle \
    $(TEST_BLD)/time_oracle

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
$(eval $(call register_host_test,test_libc_stdio_io,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_line,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_stdio_format,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_integer,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_float_parse,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_path,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_scan,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_time,libc,protocol-or-legacy))
$(eval $(call register_host_test,test_libc_dir,libc,protocol-or-legacy))

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
    $(TEST_BLD)/test_libc_stream_state.elf \
    $(TEST_BLD)/test_libc_stdio_io.elf \
    $(TEST_BLD)/test_libc_line.elf \
    $(TEST_BLD)/test_libc_stdio_format.elf \
    $(TEST_BLD)/test_libc_integer.elf \
    $(TEST_BLD)/test_libc_float_parse.elf \
    $(TEST_BLD)/test_libc_path.elf \
    $(TEST_BLD)/test_libc_scan.elf \
    $(TEST_BLD)/test_libc_time.elf \
    $(TEST_BLD)/test_libc_dir.elf

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

# ── libc completion: io group (T7) ───────────────────────────────
# Stream modes + reliable item IO + the fcntl wrapper.  The
# production stdio objects are the SAME user-mode objects the stream
# group compiles (OS01_LIBC_STREAM_DIR pattern rule); fcntl.c joins
# them with its own rule, and the replaced boundary is the descriptor
# mock (libc_stream_runtime.o), whose SYS_fcntl hook forwards to the
# host kernel so the wrapper is driven against real descriptors.
$(OS01_LIBC_STREAM_DIR)/fcntl.o: $(LIBC_SRC)/unistd/fcntl.c
	@mkdir -p $(OS01_LIBC_STREAM_DIR)
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_libc_stdio_io.o: $(TEST_CASES)/test_libc_stdio_io.c \
        $(TEST_MOCK)/libc_stream_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TEST_MOCK) -c $< -o $@

$(TEST_BLD)/test_libc_stdio_io.elf: $(TEST_BLD)/test_libc_stdio_io.o \
        $(OS01_LIBC_STREAM_OBJS) \
        $(OS01_LIBC_STREAM_DIR)/vasprintf.o \
        $(OS01_LIBC_STREAM_DIR)/fcntl.o \
        $(TEST_BLD)/libc_stream_runtime.o
	$(HOST_CC) -o $@ $^

.PHONY: test-libc-stdio-io
test-libc-stdio-io: $(TEST_BLD)/test_libc_stdio_io.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── libc completion: line group (T8) ─────────────────────────────
# Dynamic line reading (getdelim/getline) + the standard character
# interfaces.  Production stdio + the T8 getdelim.o; the descriptor
# mock is the same one the stream/io groups use, and the allocation
# failures come from -Wl,--wrap on malloc/realloc (armed counters in
# the case file), so no mock allocator is needed.
$(OS01_LIBC_STREAM_DIR)/getdelim.o: $(LIBC_SRC)/stdio/getdelim.c
	@mkdir -p $(OS01_LIBC_STREAM_DIR)
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_libc_line.o: $(TEST_CASES)/test_libc_line.c \
        $(TEST_MOCK)/libc_stream_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TEST_MOCK) -c $< -o $@

$(TEST_BLD)/test_libc_line.elf: $(TEST_BLD)/test_libc_line.o \
        $(OS01_LIBC_STREAM_OBJS) \
        $(OS01_LIBC_STREAM_DIR)/vasprintf.o \
        $(OS01_LIBC_STREAM_DIR)/fcntl.o \
        $(OS01_LIBC_STREAM_DIR)/getdelim.o \
        $(TEST_BLD)/libc_stream_runtime.o
	$(HOST_CC) -Wl,--wrap=malloc -Wl,--wrap=realloc -o $@ $^

.PHONY: test-libc-line
test-libc-line: $(TEST_BLD)/test_libc_line.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── libc completion: format group (T9) ───────────────────────────
# Formatted fd IO (dprintf/vdprintf/asprintf) + the seek family
# (stdio_seek.o) + the REAL lseek wrapper (unistd/lseek.o, explicit
# rule like fcntl.o).  asprintf.o / stdio_seek.o ride the stream-dir
# pattern rule; the descriptor mock forwards SYS_lseek/SYS_fcntl to
# the host kernel, and allocation failures are --wrap armed again.
$(OS01_LIBC_STREAM_DIR)/lseek.o: $(LIBC_SRC)/unistd/lseek.c
	@mkdir -p $(OS01_LIBC_STREAM_DIR)
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/test_libc_stdio_format.o: $(TEST_CASES)/test_libc_stdio_format.c \
        $(TEST_MOCK)/libc_stream_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) -I$(TEST_MOCK) -c $< -o $@

$(TEST_BLD)/test_libc_stdio_format.elf: $(TEST_BLD)/test_libc_stdio_format.o \
        $(OS01_LIBC_STREAM_OBJS) \
        $(OS01_LIBC_STREAM_DIR)/fcntl.o \
        $(OS01_LIBC_STREAM_DIR)/getdelim.o \
        $(OS01_LIBC_STREAM_DIR)/vasprintf.o \
        $(OS01_LIBC_STREAM_DIR)/asprintf.o \
        $(OS01_LIBC_STREAM_DIR)/stdio_seek.o \
        $(OS01_LIBC_STREAM_DIR)/lseek.o \
        $(TEST_BLD)/libc_stream_runtime.o
	$(HOST_CC) -Wl,--wrap=malloc -o $@ $^

.PHONY: test-libc-stdio-format
test-libc-stdio-format: $(TEST_BLD)/test_libc_stdio_format.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── libc completion: integer group (T10) ─────────────────────────
# The strto* family against the SHARED glibc oracle: the oracle is a
# separate host binary built with NO OS01 include paths (it cannot
# help but run the host's strto*), and the test popens it and compares
# byte-identical result tuples.  The production objects are the REAL
# libc/stdlib converters compiled as the user libc (their names shadow
# the host's by first definition); the stream objects ride along for
# snprintf's formatter.
$(TEST_BLD)/strtox_oracle: $(TESTS_DIR)/hosttests/oracle/strtox_oracle.c \
        $(TESTS_DIR)/hosttests/data/strtox_cases.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) -O0 -Wall -o $@ $<

$(TEST_BLD)/user-int/strtol.o: $(LIBC_SRC)/stdlib/strtol.c \
        $(LIBC_SRC)/include/stdlib/strtox.h
	@mkdir -p $(TEST_BLD)/user-int
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/user-int/strtoll.o: $(LIBC_SRC)/stdlib/strtoll.c
	@mkdir -p $(TEST_BLD)/user-int
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

# The absolute-value family (D01/T2): the PRODUCTION libc/stdlib/abs.c,
# compiled under the `os01_` prefix, so a call in the test can never
# resolve to the host libc's abs/labs/llabs — and the link fails if the
# production file did not define them at all.
#
# The object is compiled WITHOUT the test_platform.h force-include: that
# header defines __is_libk, and the kernel-side copy of the module is
# not the one under test.  The host test drives the USER libc.
OS01_LIBC_ABS_FNS := abs labs llabs
OS01_LIBC_ABS_PREFIX_DEFS := $(foreach f,$(OS01_LIBC_ABS_FNS),-D$(f)=$(OS01_LIBC_PREFIX)$(f))
OS01_LIBC_ABS_DIR := $(TEST_BLD)/prefixed-abs
OS01_LIBC_ABS_OBJS := $(OS01_LIBC_ABS_DIR)/abs.o

$(OS01_LIBC_ABS_DIR)/%.o: $(LIBC_SRC)/stdlib/%.c
	@mkdir -p $(OS01_LIBC_ABS_DIR)
	$(HOST_CC) $(HOST_CFLAGS) $(OS01_LIBC_ABS_PREFIX_DEFS) $(LIBC_INC) -c $< -o $@

INTEGER_TEST_CFLAGS := $(HOST_CFLAGS) $(OS01_LIBC_ABS_PREFIX_DEFS) \
    $(FRAMEWORK_INC) $(LIBC_INC) \
    -I$(TESTS_DIR)/hosttests/data \
    -D'STRTOX_ORACLE_PATH="$(TEST_BLD)/strtox_oracle"'

$(TEST_BLD)/test_libc_integer.o: $(TEST_CASES)/test_libc_integer.c \
        $(TESTS_DIR)/hosttests/data/strtox_cases.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(INTEGER_TEST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_libc_integer.elf: $(TEST_BLD)/test_libc_integer.o \
        $(TEST_BLD)/user-int/strtol.o \
        $(TEST_BLD)/user-int/strtoll.o \
        $(OS01_LIBC_ABS_OBJS) \
        $(OS01_LIBC_STREAM_OBJS) \
        $(TEST_BLD)/libc_stream_runtime.o
	$(HOST_CC) -o $@ $^

.PHONY: test-libc-integer
test-libc-integer: $(TEST_BLD)/test_libc_integer.elf $(TEST_BLD)/strtox_oracle
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── libc completion: float group (T11) ───────────────────────────
# The musl floatscan port against the SHARED glibc oracle: the oracle
# is a separate host binary built with NO OS01 include paths; the test
# compares raw IEEE BIT PATTERNS (float 4B, double 8B, the 10
# significant bytes of the x87 80-bit long double) plus consumed
# length and errno.  Production objects: the REAL strtod.c +
# floatscan.c compiled as the user libc, riding with the stream
# objects for snprintf's formatter.
$(TEST_BLD)/floatscan_oracle: $(TESTS_DIR)/hosttests/oracle/floatscan_oracle.c \
        $(TESTS_DIR)/hosttests/data/floatscan_cases.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) -O0 -Wall -o $@ $<

$(TEST_BLD)/user-int/strtod.o: $(LIBC_SRC)/stdlib/strtod.c \
        $(LIBC_SRC)/include/stdlib/floatscan.h
	@mkdir -p $(TEST_BLD)/user-int
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/user-int/floatscan.o: $(LIBC_SRC)/stdlib/floatscan.c \
        $(LIBC_SRC)/include/stdlib/floatscan.h
	@mkdir -p $(TEST_BLD)/user-int
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

FLOAT_TEST_CFLAGS := $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) \
    -I$(TESTS_DIR)/hosttests/data \
    -D'FLOATSCAN_ORACLE_PATH="$(TEST_BLD)/floatscan_oracle"' \
    -Dfabs=$(OS01_LIBC_PREFIX)fabs

$(TEST_BLD)/test_libc_float_parse.o: $(TEST_CASES)/test_libc_float_parse.c \
        $(TESTS_DIR)/hosttests/data/floatscan_cases.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(FLOAT_TEST_CFLAGS) -c $< -o $@

# The second translation unit of the fabs probe: the SAME source compiled
# with -DOS01_FABS_PROBE_TU, so it carries nothing but the probe function.
# It is compiled with -fno-builtin, so the compiler's builtin cannot
# answer the fabs() call either — the header's implementation is the only
# possible answer.  It links into the same binary as the test
# translation unit, so a header that defined the symbol twice would show
# up as a duplicate definition.
$(TEST_BLD)/user-float/fabs_probe.o: $(TEST_CASES)/test_libc_float_parse.c
	@mkdir -p $(TEST_BLD)/user-float
	$(HOST_CC) $(FLOAT_TEST_CFLAGS) -DOS01_FABS_PROBE_TU -fno-builtin \
	    -c $< -o $@

$(TEST_BLD)/test_libc_float_parse.elf: $(TEST_BLD)/test_libc_float_parse.o \
        $(TEST_BLD)/user-float/fabs_probe.o \
        $(TEST_BLD)/user-int/strtod.o \
        $(TEST_BLD)/user-int/floatscan.o \
        $(OS01_LIBC_STREAM_OBJS) \
        $(TEST_BLD)/libc_stream_runtime.o
	$(HOST_CC) -o $@ $^

.PHONY: test-libc-float-parse
test-libc-float-parse: $(TEST_BLD)/test_libc_float_parse.elf $(TEST_BLD)/floatscan_oracle
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── libc completion: scan group (T12) ────────────────────────────
# The in-memory scanf core (libc/stdio/vsscanf.c + libc/stdio/sscanf.c)
# against the SHARED glibc oracle: the oracle is a separate host binary
# built with NO OS01 include paths (it cannot help but run the host's
# sscanf), and the test compares the CONVERSION COUNT plus the raw
# bytes of every argument slot — the conversion decides how many bytes
# are written, so a wrong store type, a wrongly NUL-terminated %3c or
# a suppressed conversion that wrongly consumed a va_arg all show up.
# Production objects: the REAL libc/stdio/sscanf.c + vsscanf.c
# (compiled as the user libc, so their definitions shadow the host's),
# plus the T10 integer core (the bounded __strtox_limit) and the T11
# float scanner that the float conversions use.
$(TEST_BLD)/scanf_oracle: $(TESTS_DIR)/hosttests/oracle/scanf_oracle.c \
        $(TESTS_DIR)/hosttests/data/scan_cases.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) -O0 -Wall -o $@ $<

$(TEST_BLD)/user-int/vsscanf.o: $(LIBC_SRC)/stdio/vsscanf.c \
        $(LIBC_SRC)/include/stdlib/strtox.h \
        $(LIBC_SRC)/include/stdlib/floatscan.h
	@mkdir -p $(TEST_BLD)/user-int
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/user-int/sscanf.o: $(LIBC_SRC)/stdio/sscanf.c
	@mkdir -p $(TEST_BLD)/user-int
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

# -Wno-format: the test deliberately feeds the scanner invalid and
# edge-case format strings ("%Ld", "%q", "%0d") that the compiler's
# format checker rightly complains about.
SCAN_TEST_CFLAGS := $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) \
    -I$(TESTS_DIR)/hosttests/data \
    -Wno-format \
    -D'SCANF_ORACLE_PATH="$(TEST_BLD)/scanf_oracle"'

$(TEST_BLD)/test_libc_scan.o: $(TEST_CASES)/test_libc_scan.c \
        $(TESTS_DIR)/hosttests/data/scan_cases.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(SCAN_TEST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_libc_scan.elf: $(TEST_BLD)/test_libc_scan.o \
        $(TEST_BLD)/user-int/vsscanf.o \
        $(TEST_BLD)/user-int/sscanf.o \
        $(TEST_BLD)/user-int/strtol.o \
        $(TEST_BLD)/user-int/floatscan.o \
        $(OS01_LIBC_STREAM_OBJS) \
        $(TEST_BLD)/libc_stream_runtime.o
	$(HOST_CC) -o $@ $^

.PHONY: test-libc-scan
test-libc-scan: $(TEST_BLD)/test_libc_scan.elf $(TEST_BLD)/scanf_oracle
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── libc completion: time group (T13) ──────────────────────────────
# The calendar core (libc/time/calendar.c), the POSIX TZ parser
# (libc/time/tz.c), the formatter (libc/time/strftime.c) and the public
# entry points (libc/time/time.c + time_r.c) against the SHARED glibc
# oracle: the oracle is a separate host binary built with NO OS01
# include paths (it cannot help but run the host's gmtime/localtime/
# mktime/strftime/asctime), and the test compares the rows of the
# shared table byte for byte.
#
# Production objects: the REAL libc/time/*.c compiled as the user libc,
# so their definitions shadow the host's.  Nothing else is linked: the
# group needs no stream machinery, and the test provides the plain
# `int errno` the libc declares (the host's errno is a TLS symbol).
$(TEST_BLD)/time_oracle: $(TESTS_DIR)/hosttests/oracle/time_oracle.c \
        $(TESTS_DIR)/hosttests/data/time_cases.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) -O0 -Wall -o $@ $<

$(TEST_BLD)/user-time/calendar.o: $(LIBC_SRC)/time/calendar.c \
        $(LIBC_SRC)/include/time/calendar.h
	@mkdir -p $(TEST_BLD)/user-time
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/user-time/tz.o: $(LIBC_SRC)/time/tz.c \
        $(LIBC_SRC)/include/time/calendar.h
	@mkdir -p $(TEST_BLD)/user-time
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/user-time/strftime.o: $(LIBC_SRC)/time/strftime.c \
        $(LIBC_SRC)/include/time/calendar.h
	@mkdir -p $(TEST_BLD)/user-time
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/user-time/time.o: $(LIBC_SRC)/time/time.c \
        $(LIBC_SRC)/include/time/calendar.h
	@mkdir -p $(TEST_BLD)/user-time
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/user-time/time_r.o: $(LIBC_SRC)/time/time_r.c \
        $(LIBC_SRC)/include/time/calendar.h
	@mkdir -p $(TEST_BLD)/user-time
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

# strptime is part of the time module under test (it has to round-trip
# with strftime), so the PRODUCTION parser is linked as well — and with
# it the C-locale helpers it uses, so that no part of the round-trip
# silently falls back to the host's libc.
$(TEST_BLD)/user-time/strptime.o: $(LIBC_SRC)/time/strptime.c
	@mkdir -p $(TEST_BLD)/user-time
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/user-time/ctype.o: $(LIBC_SRC)/ctype/ctype.c
	@mkdir -p $(TEST_BLD)/user-time
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/user-time/string_extras.o: $(LIBC_SRC)/string/extras.c
	@mkdir -p $(TEST_BLD)/user-time
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/libc_time_runtime.o: $(TEST_MOCK)/libc_time_runtime.c
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) -I$(TEST_MOCK) -I$(TEST_INC) -c $< -o $@

TIME_TEST_CFLAGS := $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) \
    -I$(TESTS_DIR)/hosttests/data \
    -D'TIME_ORACLE_PATH="$(TEST_BLD)/time_oracle"'

$(TEST_BLD)/test_libc_time.o: $(TEST_CASES)/test_libc_time.c \
        $(TESTS_DIR)/hosttests/data/time_cases.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(TIME_TEST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_libc_time.elf: $(TEST_BLD)/test_libc_time.o \
        $(TEST_BLD)/user-time/calendar.o \
        $(TEST_BLD)/user-time/tz.o \
        $(TEST_BLD)/user-time/strftime.o \
        $(TEST_BLD)/user-time/time.o \
        $(TEST_BLD)/user-time/time_r.o \
        $(TEST_BLD)/user-time/strptime.o \
        $(TEST_BLD)/user-time/ctype.o \
        $(TEST_BLD)/user-time/string_extras.o \
        $(TEST_BLD)/libc_time_runtime.o
	$(HOST_CC) -o $@ $^

.PHONY: test-libc-time
test-libc-time: $(TEST_BLD)/test_libc_time.elf $(TEST_BLD)/time_oracle
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout $(HOST_TEST_TIMEOUT) \
	    $<

# ── libc completion: path group (T14) ────────────────────────────────
# realpath() and the variadic exec wrappers (execl/execlp/execle) plus
# the execvp() PATH search, against a FAKE FILESYSTEM and a fake exec
# boundary (hosttests/mock/libc_path_runtime.c).  The production sources
# are the real ones: libc/stdlib/realpath.c, libc/unistd/execl.c,
# libc/unistd/execve.c and the production lstat/readlink/stat/getcwd
# wrappers — only the syscalls below them are replaced.
$(TEST_BLD)/user-int/realpath.o: $(LIBC_SRC)/stdlib/realpath.c
	@mkdir -p $(TEST_BLD)/user-int
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/user-int/execl.o: $(LIBC_SRC)/unistd/execl.c
	@mkdir -p $(TEST_BLD)/user-int
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/user-int/execve.o: $(LIBC_SRC)/unistd/execve.c
	@mkdir -p $(TEST_BLD)/user-int
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/user-int/lstat.o: $(LIBC_SRC)/sys/stat/lstat.c
	@mkdir -p $(TEST_BLD)/user-int
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/user-int/stat.o: $(LIBC_SRC)/unistd/stat.c
	@mkdir -p $(TEST_BLD)/user-int
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/user-int/readlink.o: $(LIBC_SRC)/unistd/readlink.c
	@mkdir -p $(TEST_BLD)/user-int
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/user-int/getcwd.o: $(LIBC_SRC)/unistd/getcwd.c
	@mkdir -p $(TEST_BLD)/user-int
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/libc_path_runtime.o: $(TEST_MOCK)/libc_path_runtime.c \
        $(TEST_MOCK)/libc_path_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -I$(TEST_MOCK) -c $< -o $@

# The removal family (D01/T3): the PRODUCTION libc/stdio/remove.c plus the
# production unlink/rmdir wrappers it is built on, linked under the
# `os01_` prefix so that the host libc's remove/unlink/rmdir can never
# answer for them.  The objects are compiled WITHOUT the test_platform.h
# force-include: that header defines __is_libk, and the kernel-side copy
# of the wrappers (which fail explicitly) is not the one under test.
#
# lstat() is NOT renamed: the path group already links the production
# lstat wrapper, and remove() classifies the path with it.
OS01_LIBC_REMOVE_FNS := remove unlink rmdir
OS01_LIBC_REMOVE_PREFIX_DEFS := $(foreach f,$(OS01_LIBC_REMOVE_FNS),-D$(f)=$(OS01_LIBC_PREFIX)$(f))
OS01_LIBC_REMOVE_DIR := $(TEST_BLD)/prefixed-remove
OS01_LIBC_REMOVE_OBJS := \
    $(OS01_LIBC_REMOVE_DIR)/remove.o \
    $(OS01_LIBC_REMOVE_DIR)/unlink.o \
    $(OS01_LIBC_REMOVE_DIR)/rmdir.o

$(OS01_LIBC_REMOVE_DIR)/remove.o: $(LIBC_SRC)/stdio/remove.c
	@mkdir -p $(OS01_LIBC_REMOVE_DIR)
	$(HOST_CC) $(HOST_CFLAGS) $(OS01_LIBC_REMOVE_PREFIX_DEFS) $(LIBC_INC) -c $< -o $@

$(OS01_LIBC_REMOVE_DIR)/unlink.o: $(LIBC_SRC)/unistd/unlink.c
	@mkdir -p $(OS01_LIBC_REMOVE_DIR)
	$(HOST_CC) $(HOST_CFLAGS) $(OS01_LIBC_REMOVE_PREFIX_DEFS) $(LIBC_INC) -c $< -o $@

$(OS01_LIBC_REMOVE_DIR)/rmdir.o: $(LIBC_SRC)/unistd/rmdir.c
	@mkdir -p $(OS01_LIBC_REMOVE_DIR)
	$(HOST_CC) $(HOST_CFLAGS) $(OS01_LIBC_REMOVE_PREFIX_DEFS) $(LIBC_INC) -c $< -o $@

PATH_TEST_CFLAGS := $(HOST_CFLAGS) $(OS01_LIBC_REMOVE_PREFIX_DEFS) \
    $(FRAMEWORK_INC) $(LIBC_INC) \
    -I$(TEST_MOCK)

$(TEST_BLD)/test_libc_path.o: $(TEST_CASES)/test_libc_path.c \
        $(TEST_MOCK)/libc_path_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(PATH_TEST_CFLAGS) -c $< -o $@

$(TEST_BLD)/test_libc_path.elf: $(TEST_BLD)/test_libc_path.o \
        $(TEST_BLD)/user-int/realpath.o \
        $(TEST_BLD)/user-int/execl.o \
        $(TEST_BLD)/user-int/execve.o \
        $(TEST_BLD)/user-int/lstat.o \
        $(TEST_BLD)/user-int/stat.o \
        $(TEST_BLD)/user-int/readlink.o \
        $(TEST_BLD)/user-int/getcwd.o \
        $(OS01_LIBC_REMOVE_OBJS) \
        $(OS01_LIBC_STREAM_OBJS) \
        $(TEST_BLD)/libc_stream_runtime.o \
        $(TEST_BLD)/libc_path_runtime.o
	$(HOST_CC) -o $@ $^

.PHONY: test-libc-path
test-libc-path: $(TEST_BLD)/test_libc_path.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout 60 $(TEST_BLD)/test_libc_path.elf

# ── libc completion: dir group (T15) ────────────────────────────────
# The directory descriptor and the sorted scan: opendir/readdir/closedir
# with the record validation and the errno semantics, dirfd/fdopendir
# with the descriptor ownership rules, and scandir/alphasort.  The
# PRODUCTION sources are linked (libc/unistd/dirent.c,
# libc/stdlib/scandir.c and the production open/fstat/close wrappers);
# only the directory itself is replaced, by
# hosttests/mock/libc_dir_runtime.c.
$(TEST_BLD)/user-int/dirent.o: $(LIBC_SRC)/unistd/dirent.c
	@mkdir -p $(TEST_BLD)/user-int
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/user-int/scandir.o: $(LIBC_SRC)/stdlib/scandir.c
	@mkdir -p $(TEST_BLD)/user-int
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/user-int/fstat.o: $(LIBC_SRC)/unistd/fstat.c
	@mkdir -p $(TEST_BLD)/user-int
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/user-int/open.o: $(LIBC_SRC)/unistd/open.c
	@mkdir -p $(TEST_BLD)/user-int
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/user-int/close.o: $(LIBC_SRC)/unistd/close.c
	@mkdir -p $(TEST_BLD)/user-int
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/libc_dir_runtime.o: $(TEST_MOCK)/libc_dir_runtime.c \
        $(TEST_MOCK)/libc_dir_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -I$(TEST_MOCK) -c $< -o $@

DIR_TEST_CFLAGS := $(HOST_CFLAGS) $(FRAMEWORK_INC) $(LIBC_INC) \
    -I$(TEST_MOCK)

$(TEST_BLD)/test_libc_dir.o: $(TEST_CASES)/test_libc_dir.c \
        $(TEST_MOCK)/libc_dir_runtime.h
	@mkdir -p $(TEST_BLD)
	$(HOST_CC) $(DIR_TEST_CFLAGS) -c $< -o $@

# The malloc/realloc/free of the libc are interposed with the linker's
# --wrap so that an allocation failure can be injected at a chosen point
# (the group counts the live allocations to prove that the failure path
# releases everything it took).
$(TEST_BLD)/test_libc_dir.elf: $(TEST_BLD)/test_libc_dir.o \
        $(TEST_BLD)/user-int/dirent.o \
        $(TEST_BLD)/user-int/scandir.o \
        $(TEST_BLD)/user-int/fstat.o \
        $(TEST_BLD)/user-int/open.o \
        $(OS01_LIBC_STREAM_OBJS) \
        $(TEST_BLD)/libc_stream_runtime.o \
        $(TEST_BLD)/libc_dir_runtime.o
	$(HOST_CC) -Wl,--wrap=malloc -Wl,--wrap=realloc -Wl,--wrap=free \
	    -o $@ $^

.PHONY: test-libc-dir
test-libc-dir: $(TEST_BLD)/test_libc_dir.elf
	python3 $(TESTS_DIR)/qemutests/run_hosttests.py \
	    --build-dir $(BUILD_DIR) --profile $(PROFILE) \
	    --timeout 60 $(TEST_BLD)/test_libc_dir.elf

# ── the RED build of the dir group (T15 evidence) ───────────────────
# The RED build links the PRE-T15 state: dirent.c as it was at 7aeb732a
# (the commit before the T15 implementation).  That implementation
#   - does not check the type in opendir()  → a regular file opens;
#   - does not set errno when the kernel fails → the errno stays;
#   - does not copy the kernel's d_off      → the cookie is lost;
# and the sorted-scan API (dirfd/fdopendir/scandir/alphasort) had NO
# definition at all.
#
# RED-1 (behavioural): the new-API cases are compiled out (those symbols
#   had no definition before T15, so the link would fail) and the OLD
#   dirent.c is linked instead of the current one — the cases that cover
#   the new behaviour then fail as ASSERTIONS.
# RED-2 (link): the archive members T15 added are deleted from a copy of
#   libc.a and a probe is linked against the result; the CONTROL link
#   against the pristine archive must succeed.  That half is done by the
#   root-level `test-libc-dir-red` target (mk/components/run.mk).
#
# The old dirent.c is taken from the tree itself, so the RED state is the
# historical implementation and not a hand-written approximation.
$(TEST_BLD)/red/dirent_pre_t15.c:
	@mkdir -p $(TEST_BLD)/red
	git show 7aeb732a:libc/unistd/dirent.c > $@

$(TEST_BLD)/red/dirent_pre_t15.o: $(TEST_BLD)/red/dirent_pre_t15.c
	$(HOST_CC) $(HOST_CFLAGS) $(LIBC_INC) -c $< -o $@

$(TEST_BLD)/red/test_libc_dir_red.o: $(TEST_CASES)/test_libc_dir.c \
        $(TEST_MOCK)/libc_dir_runtime.h
	@mkdir -p $(TEST_BLD)/red
	$(HOST_CC) $(DIR_TEST_CFLAGS) -DDIR_RED_NO_NEW_API \
	    -DDIR_RED_NO_BAD_RECORDS -c $< -o $@

.PHONY: test-libc-dir-red
test-libc-dir-red: $(TEST_BLD)/red/test_libc_dir_red.o \
        $(TEST_BLD)/red/dirent_pre_t15.o
	@echo "=== RED-1 (behaviour): the pre-T15 dirent.c has no type check, no errno and no d_off ==="
	@set +e; \
	  $(HOST_CC) -Wl,--wrap=malloc -Wl,--wrap=realloc -Wl,--wrap=free \
	    -o $(TEST_BLD)/red/test_libc_dir_red.elf \
	    $(TEST_BLD)/red/test_libc_dir_red.o \
	    $(TEST_BLD)/red/dirent_pre_t15.o \
	    $(TEST_BLD)/user-int/fstat.o \
	    $(TEST_BLD)/user-int/open.o \
	    $(OS01_LIBC_STREAM_OBJS) \
	    $(TEST_BLD)/libc_stream_runtime.o \
	    $(TEST_BLD)/libc_dir_runtime.o; \
	  rc_link=$$?; \
	  if [ $$rc_link -ne 0 ]; then \
	    echo "ERROR: the RED build did not link — the behavioural RED would prove nothing"; \
	    exit 1; \
	  fi; \
	  $(TEST_BLD)/red/test_libc_dir_red.elf; \
	  rc_run=$$?; \
	  echo "RED-1 exit code: $$rc_run"; \
	  if [ $$rc_run -eq 0 ]; then \
	    echo "ERROR: the RED run passed — the RED state is not reproduced"; \
	    exit 1; \
	  fi; \
	  exit 1

# ── the RED build of the path group (T14 evidence) ───────────────────
# The RED build links the PRE-T14 state, i.e. the commit that preceded
# the T14 implementation (b7c33ba6): realpath was an empty stub that
# answered NULL (it lived in libc/stdlib/malloc.c) and execl/execlp/
# execle had NO definition anywhere in the tree.
#
# Two transcripts, both reproducible from this target:
#   RED-1 (behavioural): the stub realpath is linked and the exec cases
#     are compiled out, so every realpath case fails as an ASSERTION
#     instead of crashing or failing to link.
#   RED-2 (link): the exec cases stay in and libc/unistd/execl.c is NOT
#     linked, so the link itself fails with "undefined reference to
#     execl" — the proof that the wrappers had no definition before T14.
#
# The stub realpath lives in the test file itself (see the PATH_RED_STUB
# block in hosttests/cases/test_libc_path.c) so that the RED state is
# self-contained and does not need the pre-T14 malloc.c dragged in.
$(TEST_BLD)/red/test_libc_path_red.o: $(TEST_CASES)/test_libc_path.c \
        $(TEST_MOCK)/libc_path_runtime.h
	@mkdir -p $(TEST_BLD)/red
	$(HOST_CC) $(PATH_TEST_CFLAGS) -DPATH_RED_STUB -DPATH_RED_NO_EXECL \
	    -c $< -o $@

# The RED transcript of the path group: the behavioural RED.  The LINK
# RED for the exec wrappers is NOT done here — the host's glibc defines
# execl/execlp/execle, so a host link can never show the hole.  It is
# reproduced against OS01's own libc archive by the root-level
# `test-libc-path-red` target (mk/components/run.mk).
.PHONY: test-libc-path-red
test-libc-path-red: $(TEST_BLD)/red/test_libc_path_red.o \
        $(TEST_BLD)/user-int/lstat.o \
        $(TEST_BLD)/user-int/stat.o \
        $(TEST_BLD)/user-int/readlink.o \
        $(TEST_BLD)/user-int/getcwd.o \
        $(OS01_LIBC_STREAM_OBJS) \
        $(TEST_BLD)/libc_stream_runtime.o \
        $(TEST_BLD)/libc_path_runtime.o
	@echo "=== RED-1 (behaviour): realpath was a stub that answered NULL ==="
	@set +e; \
	  $(HOST_CC) -o $(TEST_BLD)/red/test_libc_path_red.elf \
	    $(TEST_BLD)/red/test_libc_path_red.o \
	    $(TEST_BLD)/user-int/lstat.o \
	    $(TEST_BLD)/user-int/stat.o \
	    $(TEST_BLD)/user-int/readlink.o \
	    $(TEST_BLD)/user-int/getcwd.o \
	    $(OS01_LIBC_STREAM_OBJS) \
	    $(TEST_BLD)/libc_stream_runtime.o \
	    $(TEST_BLD)/libc_path_runtime.o; \
	  $(TEST_BLD)/red/test_libc_path_red.elf; \
	  rc_run=$$?; \
	  echo "RED-1 exit code: $$rc_run"; \
	  if [ $$rc_run -eq 0 ]; then \
	    echo "ERROR: the RED run passed — the RED state is not reproduced"; \
	    exit 1; \
	  fi; \
	  exit 1
